/*
 * AmigaMCP - a native AmigaDOS command that talks to an LLM API over HTTPS,
 *            straight from a 68k Amiga. No bridge, no PC in the loop.
 *
 *   AmigaMCP MODEL claude-opus-4-8 INSTRUCTION "mange des nouilles" TOKENS=14400
 *   AmigaMCP RESUME_JOBS
 *
 * Config lives in a plain text file (default SYS:.claude/config) - the Amiga
 * needs neither 8.3 names nor file extensions:
 *
 *   key       sk-ant-...
 *   model     claude-opus-4-8
 *   provider  anthropic            ; or "openai" (covers GPT, Gemini-compat,
 *   host      api.anthropic.com    ;  Mistral, local llama.cpp, ...)
 *   path      /v1/messages
 *   port      443
 *   version   2023-06-01
 *
 * Every request becomes a durable job under SYS:.claude/ :
 *   2026-06-12-153012-JOB_PENDING   written before the call
 *   2026-06-12-153012-JOB_DONE      renamed once the answer is in
 *   2026-06-12-153012.md            prompt + answer transcript
 *
 * The on-disk job IS the resilience: if the machine reboots mid-call, the
 * PENDING file survives, and `AmigaMCP RESUME_JOBS` (drop it in WBStartup or
 * the startup-sequence) replays it. Stability is not required of the process,
 * only of the filesystem - which on this kind of Amiga already expects a
 * hard reset.
 *
 * Transport is chosen at link time (see net.h / Makefile): net_amissl.c for
 * real HTTPS, net_plain.c for HTTP endpoints.
 */
#include <exec/types.h>
#include <dos/dos.h>
#include <dos/rdargs.h>
#include <dos/datetime.h>
#include <dos/dosextens.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "jsmn.h"
#include "net.h"

struct Library *SocketBase = NULL;

/* The CLI scans the binary for this cookie to size our stack. OpenSSL/TLS in
 * the AmiSSL path is stack-hungry; 64K keeps the handshake safe. (Harmless to
 * the cleartext build.) */
const char stack_cookie[] = "$STACK:65536";

#define DEF_DIR     "SYS:.claude"
#define DEF_TOKENS  4096L
#define MAX_TOKENS  2048

/* ------------------------------------------------------------------ */
/* dbuf - growable byte buffer                                         */
/* ------------------------------------------------------------------ */
typedef struct { unsigned char *p; long len; long cap; } dbuf;

static int dbuf_init(dbuf *d) {
  d->p = (unsigned char *)malloc(256);
  d->len = 0; d->cap = d->p ? 256 : 0;
  return d->p != NULL;
}
static void dbuf_free(dbuf *d) { if (d->p) free(d->p); d->p = NULL; d->len = d->cap = 0; }
static int dbuf_ensure(dbuf *d, long extra) {
  long need = d->len + extra;
  if (need <= d->cap) return 1;
  while (d->cap < need) d->cap = d->cap < 256 ? 256 : d->cap * 2;
  d->p = (unsigned char *)realloc(d->p, d->cap);
  return d->p != NULL;
}
static void dbuf_putb(dbuf *d, int c) { if (dbuf_ensure(d, 1)) d->p[d->len++] = (unsigned char)c; }
static void dbuf_put(dbuf *d, const void *s, long n) {
  if (n <= 0) return;
  if (dbuf_ensure(d, n)) { memcpy(d->p + d->len, s, n); d->len += n; }
}
static void dbuf_puts(dbuf *d, const char *s) { dbuf_put(d, s, (long)strlen(s)); }

/* JSON-escape a string into the buffer (bytes >=0x7f -> \u00XX). */
static void dbuf_json_str(dbuf *d, const unsigned char *s, long n) {
  long i; static const char hex[] = "0123456789abcdef";
  for (i = 0; i < n; i++) {
    unsigned char c = s[i];
    switch (c) {
    case '"':  dbuf_puts(d, "\\\""); break;
    case '\\': dbuf_puts(d, "\\\\"); break;
    case '\n': dbuf_puts(d, "\\n"); break;
    case '\r': dbuf_puts(d, "\\r"); break;
    case '\t': dbuf_puts(d, "\\t"); break;
    default:
      if (c < 0x20 || c >= 0x7f) {
        dbuf_puts(d, "\\u00");
        dbuf_putb(d, hex[(c >> 4) & 0xf]); dbuf_putb(d, hex[c & 0xf]);
      } else dbuf_putb(d, c);
    }
  }
}

/* ------------------------------------------------------------------ */
/* jsmn helpers                                                        */
/* ------------------------------------------------------------------ */
static int tok_eq(const char *js, jsmntok_t *t, const char *s) {
  int len = t->end - t->start;
  return ((int)strlen(s) == len) && strncmp(js + t->start, s, len) == 0;
}
static int tok_skip(jsmntok_t *t, int i) {
  int j, n;
  switch (t[i].type) {
  case JSMN_OBJECT:
    n = t[i].size; i++;
    for (j = 0; j < n; j++) { i = tok_skip(t, i); i = tok_skip(t, i); }
    return i;
  case JSMN_ARRAY:
    n = t[i].size; i++;
    for (j = 0; j < n; j++) i = tok_skip(t, i);
    return i;
  default: return i + 1;
  }
}
static int find_key(const char *js, jsmntok_t *t, int oi, const char *key) {
  int n, j, i;
  if (oi < 0 || t[oi].type != JSMN_OBJECT) return -1;
  n = t[oi].size; i = oi + 1;
  for (j = 0; j < n; j++) {
    int ki = i, vi = i + 1;
    if (t[ki].type == JSMN_STRING && tok_eq(js, &t[ki], key)) return vi;
    i = tok_skip(t, vi);
  }
  return -1;
}
/* append the JSON-unescaped contents of a string token to a dbuf */
static void tok_unescape_into(dbuf *d, const char *js, jsmntok_t *t) {
  int n = t->end - t->start, i = 0;
  const char *s = js + t->start;
  while (i < n) {
    char c = s[i++];
    if (c == '\\' && i < n) {
      char e = s[i++];
      switch (e) {
      case 'n': dbuf_putb(d, '\n'); break;
      case 't': dbuf_putb(d, '\t'); break;
      case 'r': dbuf_putb(d, '\r'); break;
      case '"': dbuf_putb(d, '"'); break;
      case '\\': dbuf_putb(d, '\\'); break;
      case '/': dbuf_putb(d, '/'); break;
      case 'b': dbuf_putb(d, '\b'); break;
      case 'f': dbuf_putb(d, '\f'); break;
      case 'u':
        if (i + 4 <= n) {
          int v = 0, k;
          for (k = 0; k < 4; k++) {
            char h = s[i + k];
            v = (v << 4) | ((h >= '0' && h <= '9') ? h - '0'
                            : (h >= 'a' && h <= 'f') ? h - 'a' + 10
                            : (h >= 'A' && h <= 'F') ? h - 'A' + 10 : 0);
          }
          i += 4;
          dbuf_putb(d, v < 256 ? (char)v : '?');
        }
        break;
      default: dbuf_putb(d, e); break;
      }
    } else dbuf_putb(d, c);
  }
}

/* ------------------------------------------------------------------ */
/* file helpers                                                        */
/* ------------------------------------------------------------------ */
static char *load_file(const char *path, long *outlen) {
  BPTR fh = Open((STRPTR)path, MODE_OLDFILE);
  dbuf d; char chunk[512]; LONG r;
  if (!fh) return NULL;
  if (!dbuf_init(&d)) { Close(fh); return NULL; }
  while ((r = Read(fh, chunk, sizeof(chunk))) > 0) dbuf_put(&d, chunk, r);
  Close(fh);
  dbuf_putb(&d, 0); /* NUL terminate for string use */
  if (outlen) *outlen = d.len - 1;
  return (char *)d.p;
}
static int save_file(const char *path, const void *buf, long n) {
  BPTR fh = Open((STRPTR)path, MODE_NEWFILE);
  if (!fh) return 0;
  Write(fh, (APTR)buf, n);
  Close(fh);
  return 1;
}

/* ------------------------------------------------------------------ */
/* config / job model                                                  */
/* ------------------------------------------------------------------ */
struct job {
  char model[128];
  char provider[32];
  char host[128];
  char path[160];
  int  port;
  char version[40];
  long tokens;
  char *instruction; /* malloc'd */
};

static void job_defaults(struct job *j) {
  strcpy(j->model, "claude-opus-4-8");
  strcpy(j->provider, "anthropic");
  strcpy(j->host, "api.anthropic.com");
  strcpy(j->path, "/v1/messages");
  j->port = 443;
  strcpy(j->version, "2023-06-01");
  j->tokens = DEF_TOKENS;
  j->instruction = NULL;
}

/* copy one whitespace-trimmed word value into dst */
static void set_field(char *dst, int cap, const char *val) {
  int n = 0;
  while (*val == ' ' || *val == '\t') val++;
  while (val[n] && val[n] != '\n' && val[n] != '\r' && n < cap - 1) n++;
  while (n > 0 && (val[n - 1] == ' ' || val[n - 1] == '\t')) n--;
  memcpy(dst, val, n); dst[n] = 0;
}

/* Apply "keyword value" lines from a text blob to a job. Recognised keys:
 * key/model/provider/host/path/port/version/tokens/instruction. The special
 * "instruction" key (no value on its line) means: the rest of the blob, from
 * the next line to the end, is the instruction text. Returns the API key (if
 * a "key" line was present) in keyout. */
static void apply_lines(char *text, struct job *j, char *keyout, int keycap) {
  char *p = text;
  while (p && *p) {
    char *eol = p;
    char *kw = p;
    int kwlen;
    while (*eol && *eol != '\n') eol++;
    /* keyword = first token */
    while (*kw == ' ' || *kw == '\t') kw++;
    kwlen = 0;
    while (kw[kwlen] && kw[kwlen] != ' ' && kw[kwlen] != '\t' &&
           kw[kwlen] != '\n' && kw[kwlen] != '\r')
      kwlen++;
    if (kwlen && kw[0] != '#' && kw[0] != ';') {
      const char *val = kw + kwlen;
      if      (!strncmp(kw, "key", 3) && kwlen == 3 && keyout) set_field(keyout, keycap, val);
      else if (!strncmp(kw, "model", 5) && kwlen == 5) set_field(j->model, sizeof(j->model), val);
      else if (!strncmp(kw, "provider", 8) && kwlen == 8) set_field(j->provider, sizeof(j->provider), val);
      else if (!strncmp(kw, "host", 4) && kwlen == 4) set_field(j->host, sizeof(j->host), val);
      else if (!strncmp(kw, "path", 4) && kwlen == 4) set_field(j->path, sizeof(j->path), val);
      else if (!strncmp(kw, "port", 4) && kwlen == 4) { char b[16]; set_field(b, sizeof(b), val); j->port = atoi(b); }
      else if (!strncmp(kw, "version", 7) && kwlen == 7) set_field(j->version, sizeof(j->version), val);
      else if (!strncmp(kw, "tokens", 6) && kwlen == 6) { char b[16]; set_field(b, sizeof(b), val); j->tokens = atol(b); }
      else if (!strncmp(kw, "instruction", 11) && kwlen == 11) {
        char *start = (*eol == '\n') ? eol + 1 : eol;
        long len = (long)strlen(start);
        j->instruction = (char *)malloc(len + 1);
        if (j->instruction) memcpy(j->instruction, start, len + 1);
        return; /* rest of blob consumed */
      }
    }
    p = (*eol == '\n') ? eol + 1 : eol;
  }
}

/* ------------------------------------------------------------------ */
/* date stamp -> "YYYY-MM-DD-HHMMSS"                                   */
/* ------------------------------------------------------------------ */
static void now_stamp(char *out) {
  struct DateStamp ds;
  struct DateTime dt;
  char date[16], tim[16], day[16];
  DateStamp(&ds);
  dt.dat_Stamp = ds;
  dt.dat_Format = FORMAT_INT; /* date -> "YY-MM-DD" */
  dt.dat_Flags = 0;
  dt.dat_StrDay = day;
  dt.dat_StrDate = date;
  dt.dat_StrTime = tim; /* "HH:MM:SS" */
  DateToStr(&dt);
  sprintf(out, "20%s-%c%c%c%c%c%c", date,
          tim[0], tim[1], tim[3], tim[4], tim[6], tim[7]);
}

/* ------------------------------------------------------------------ */
/* HTTP request / response                                             */
/* ------------------------------------------------------------------ */
static void build_request(dbuf *req, const struct job *j, const char *key) {
  dbuf body;
  char hdr[512];
  dbuf_init(&body);
  dbuf_puts(&body, "{\"model\":\"");
  dbuf_puts(&body, j->model);
  dbuf_puts(&body, "\",\"max_tokens\":");
  { char n[16]; sprintf(n, "%ld", j->tokens); dbuf_puts(&body, n); }
  dbuf_puts(&body, ",\"messages\":[{\"role\":\"user\",\"content\":\"");
  dbuf_json_str(&body, (const unsigned char *)j->instruction,
                (long)strlen(j->instruction));
  dbuf_puts(&body, "\"}]}");

  sprintf(hdr, "POST %s HTTP/1.1\r\nHost: %s\r\n", j->path, j->host);
  dbuf_puts(req, hdr);
  if (!strcmp(j->provider, "openai")) {
    sprintf(hdr, "Authorization: Bearer %s\r\n", key);
    dbuf_puts(req, hdr);
  } else {
    sprintf(hdr, "x-api-key: %s\r\nanthropic-version: %s\r\n", key, j->version);
    dbuf_puts(req, hdr);
  }
  sprintf(hdr, "content-type: application/json\r\ncontent-length: %ld\r\n"
               "connection: close\r\n\r\n", body.len);
  dbuf_puts(req, hdr);
  dbuf_put(req, body.p, body.len);
  dbuf_free(&body);
}

/* Pull the assistant text out of an API JSON body into `out`.
 * Returns 1 on success, 0 if nothing usable was found. *iserr set when the
 * extracted text is an API error message. */
static int extract_text(const char *body, long blen, const struct job *j,
                        dbuf *out, int *iserr) {
  jsmn_parser pr;
  jsmntok_t *tok;
  int n, ci, ei;
  *iserr = 0;
  tok = (jsmntok_t *)malloc(sizeof(jsmntok_t) * MAX_TOKENS);
  if (!tok) return 0;
  jsmn_init(&pr);
  n = jsmn_parse(&pr, body, blen, tok, MAX_TOKENS);
  if (n < 1 || tok[0].type != JSMN_OBJECT) { free(tok); return 0; }

  if (!strcmp(j->provider, "openai")) {
    ci = find_key(body, tok, 0, "choices");
    if (ci >= 0 && tok[ci].type == JSMN_ARRAY && tok[ci].size > 0) {
      int e0 = ci + 1;
      int msg = find_key(body, tok, e0, "message");
      if (msg >= 0) {
        int ct = find_key(body, tok, msg, "content");
        if (ct >= 0 && tok[ct].type == JSMN_STRING) {
          tok_unescape_into(out, body, &tok[ct]); free(tok); return 1;
        }
      }
    }
  } else {
    ci = find_key(body, tok, 0, "content");
    if (ci >= 0 && tok[ci].type == JSMN_ARRAY && tok[ci].size > 0) {
      int e0 = ci + 1;
      int ti = find_key(body, tok, e0, "text");
      if (ti >= 0 && tok[ti].type == JSMN_STRING) {
        tok_unescape_into(out, body, &tok[ti]); free(tok); return 1;
      }
    }
  }
  /* error fallback: {"error":{"message":"..."}} or {"message":"..."} */
  ei = find_key(body, tok, 0, "error");
  if (ei >= 0 && tok[ei].type == JSMN_OBJECT) {
    int mi = find_key(body, tok, ei, "message");
    if (mi >= 0 && tok[mi].type == JSMN_STRING) {
      tok_unescape_into(out, body, &tok[mi]); *iserr = 1; free(tok); return 1;
    }
  }
  ei = find_key(body, tok, 0, "message");
  if (ei >= 0 && tok[ei].type == JSMN_STRING) {
    tok_unescape_into(out, body, &tok[ei]); *iserr = 1; free(tok); return 1;
  }
  free(tok);
  return 0;
}

/* Perform one API call. On success fills `result` with the assistant text and
 * returns 1. On any failure returns 0 (and the caller leaves the job PENDING
 * so RESUME_JOBS can retry later). */
static int run_job(const struct job *j, const char *key, dbuf *result) {
  net_conn *c;
  const char *err = NULL;
  dbuf req, resp;
  char chunk[1024];
  long r;
  char *body;
  long blen;
  int iserr = 0, ok;

  if (!j->instruction || !j->instruction[0]) {
    printf("AmigaMCP: empty instruction\n");
    return 0;
  }
  if (!key || !key[0]) {
    printf("AmigaMCP: no API key in config\n");
    return 0;
  }

  printf("AmigaMCP: %s %s -> %s:%d (%s)\n", j->provider, j->model, j->host,
         j->port, net_kind());

  c = net_open(j->host, j->port, &err);
  if (!c) { printf("AmigaMCP: connect failed: %s\n", err ? err : "?"); return 0; }

  dbuf_init(&req);
  build_request(&req, j, key);
  if (net_write_all(c, req.p, req.len) < 0) {
    printf("AmigaMCP: send failed\n");
    dbuf_free(&req); net_close(c); return 0;
  }
  dbuf_free(&req);

  dbuf_init(&resp);
  while ((r = net_read(c, chunk, sizeof(chunk))) > 0) dbuf_put(&resp, chunk, r);
  net_close(c);
  if (r < 0 && resp.len == 0) { printf("AmigaMCP: read failed\n"); dbuf_free(&resp); return 0; }
  dbuf_putb(&resp, 0);

  /* split headers / body */
  body = strstr((char *)resp.p, "\r\n\r\n");
  if (body) body += 4; else body = (char *)resp.p;
  blen = resp.len - 1 - (body - (char *)resp.p);

  ok = extract_text(body, blen, j, result, &iserr);
  if (!ok) {
    printf("AmigaMCP: could not parse API response\n");
    dbuf_free(&resp);
    return 0;
  }
  dbuf_free(&resp);
  if (iserr) {
    printf("AmigaMCP: API error: %.*s\n", (int)result->len, result->p);
    return 0;
  }
  return 1;
}

/* ------------------------------------------------------------------ */
/* job persistence                                                     */
/* ------------------------------------------------------------------ */
static void join_path(char *out, const char *dir, const char *name) {
  sprintf(out, "%s/%s", dir, name);
}

static void write_pending(const char *dir, const char *stamp,
                          const struct job *j) {
  char path[256], name[64];
  dbuf d;
  sprintf(name, "%s-JOB_PENDING", stamp);
  join_path(path, dir, name);
  dbuf_init(&d);
  dbuf_puts(&d, "# AmigaMCP job\nstatus pending\n");
  dbuf_puts(&d, "provider "); dbuf_puts(&d, j->provider); dbuf_putb(&d, '\n');
  dbuf_puts(&d, "host ");     dbuf_puts(&d, j->host);     dbuf_putb(&d, '\n');
  dbuf_puts(&d, "path ");     dbuf_puts(&d, j->path);     dbuf_putb(&d, '\n');
  { char b[16]; sprintf(b, "port %d\n", j->port); dbuf_puts(&d, b); }
  dbuf_puts(&d, "version ");  dbuf_puts(&d, j->version);  dbuf_putb(&d, '\n');
  dbuf_puts(&d, "model ");    dbuf_puts(&d, j->model);    dbuf_putb(&d, '\n');
  { char b[24]; sprintf(b, "tokens %ld\n", j->tokens); dbuf_puts(&d, b); }
  dbuf_puts(&d, "instruction\n");
  dbuf_puts(&d, j->instruction ? j->instruction : "");
  save_file(path, d.p, d.len);
  dbuf_free(&d);
}

/* rename PENDING -> DONE and drop a .md transcript */
static void finalize(const char *dir, const char *stamp, const struct job *j,
                     const dbuf *result) {
  char pend[256], done[256], md[256], name[64];
  dbuf d;

  sprintf(name, "%s-JOB_PENDING", stamp); join_path(pend, dir, name);
  sprintf(name, "%s-JOB_DONE", stamp);    join_path(done, dir, name);
  sprintf(name, "%s.md", stamp);          join_path(md, dir, name);

  dbuf_init(&d);
  dbuf_puts(&d, "# AmigaMCP ");  dbuf_puts(&d, stamp); dbuf_putb(&d, '\n');
  dbuf_puts(&d, "\nmodel: ");    dbuf_puts(&d, j->model); dbuf_putb(&d, '\n');
  dbuf_puts(&d, "\n## Instruction\n\n");
  dbuf_puts(&d, j->instruction ? j->instruction : "");
  dbuf_puts(&d, "\n\n## Response\n\n");
  dbuf_put(&d, result->p, result->len);
  dbuf_putb(&d, '\n');
  save_file(md, d.p, d.len);
  dbuf_free(&d);

  Rename((STRPTR)pend, (STRPTR)done);
}

/* ------------------------------------------------------------------ */
/* RESUME_JOBS: replay every *-JOB_PENDING in the directory            */
/* ------------------------------------------------------------------ */
static int ends_with(const char *s, const char *suf) {
  int ls = strlen(s), lf = strlen(suf);
  return ls >= lf && !strcmp(s + ls - lf, suf);
}

static void resume_one(const char *dir, const char *fname, const char *key) {
  char path[256];
  char *text;
  long len;
  struct job j;
  dbuf result;
  char stamp[32];
  int slen;

  /* stamp = filename without the "-JOB_PENDING" suffix */
  slen = (int)strlen(fname) - (int)strlen("-JOB_PENDING");
  if (slen <= 0 || slen >= (int)sizeof(stamp)) return;
  memcpy(stamp, fname, slen); stamp[slen] = 0;

  join_path(path, dir, fname);
  text = load_file(path, &len);
  if (!text) return;

  job_defaults(&j);
  apply_lines(text, &j, NULL, 0); /* key stays from config */

  printf("AmigaMCP: resuming %s\n", fname);
  if (dbuf_init(&result) && run_job(&j, key, &result)) {
    finalize(dir, stamp, &j, &result);
    printf("AmigaMCP: %s done\n", stamp);
  } else {
    printf("AmigaMCP: %s still pending\n", stamp);
  }
  dbuf_free(&result);
  if (j.instruction) free(j.instruction);
  free(text);
}

static void resume_jobs(const char *dir, const char *key) {
  BPTR lock;
  struct FileInfoBlock *fib;
  dbuf names; /* NUL-separated list, gathered before we mutate the dir */
  long off;

  lock = Lock((STRPTR)dir, ACCESS_READ);
  if (!lock) { printf("AmigaMCP: cannot open %s\n", dir); return; }
  fib = (struct FileInfoBlock *)AllocDosObject(DOS_FIB, NULL);
  if (!fib) { UnLock(lock); return; }

  dbuf_init(&names);
  if (Examine(lock, fib)) {
    while (ExNext(lock, fib)) {
      if (fib->fib_DirEntryType < 0 && ends_with(fib->fib_FileName, "-JOB_PENDING")) {
        dbuf_puts(&names, fib->fib_FileName);
        dbuf_putb(&names, 0);
      }
    }
  }
  FreeDosObject(DOS_FIB, fib);
  UnLock(lock);

  for (off = 0; off < names.len; ) {
    const char *name = (const char *)names.p + off;
    resume_one(dir, name, key);
    off += strlen(name) + 1;
  }
  if (names.len == 0) printf("AmigaMCP: no pending jobs\n");
  dbuf_free(&names);
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */
#define T_MODEL 0
#define T_INSTR 1
#define T_TOKENS 2
#define T_RESUME 3
#define T_DIR 4
#define T_CONFIG 5
#define T_NOJOB 6

int main(void) {
  static const char *templ =
      "MODEL/K,INSTRUCTION/K,TOKENS/K/N,RESUME_JOBS/S,DIR/K,CONFIG/K,NOJOB/S";
  LONG args[7];
  struct RDArgs *rd;
  const char *dir, *cfgpath;
  char cfgbuf[256];
  char key[256];
  struct job j;
  char *cfgtext;
  int rc = 0;

  memset(args, 0, sizeof(args));
  rd = ReadArgs((STRPTR)templ, args, NULL);
  if (!rd) {
    printf("Usage: AmigaMCP MODEL <m> INSTRUCTION \"...\" [TOKENS=N]\n"
           "       AmigaMCP RESUME_JOBS\n"
           "       [DIR=<dir>] [CONFIG=<file>] [NOJOB]\n");
    return 20;
  }

  SocketBase = OpenLibrary("bsdsocket.library", 4);
  if (!SocketBase) {
    printf("AmigaMCP: bsdsocket.library not available (TCP stack running?)\n");
    FreeArgs(rd);
    return 20;
  }

  dir = args[T_DIR] ? (const char *)args[T_DIR] : DEF_DIR;

  /* make sure the harness dir exists */
  {
    BPTR l = Lock((STRPTR)dir, ACCESS_READ);
    if (l) UnLock(l);
    else { BPTR d = CreateDir((STRPTR)dir); if (d) UnLock(d); }
  }

  /* config: CONFIG=... or <dir>/config */
  if (args[T_CONFIG]) cfgpath = (const char *)args[T_CONFIG];
  else { join_path(cfgbuf, dir, "config"); cfgpath = cfgbuf; }

  job_defaults(&j);
  key[0] = 0;
  cfgtext = load_file(cfgpath, NULL);
  if (cfgtext) {
    char *saveinstr;
    /* config must not define an instruction; guard against it */
    apply_lines(cfgtext, &j, key, sizeof(key));
    saveinstr = j.instruction; j.instruction = NULL;
    if (saveinstr) free(saveinstr);
    free(cfgtext);
  } else {
    printf("AmigaMCP: no config at %s (need at least an API key)\n", cfgpath);
  }

  if (args[T_RESUME]) {
    resume_jobs(dir, key);
    goto done;
  }

  /* one-shot job */
  if (args[T_MODEL])  set_field(j.model, sizeof(j.model), (const char *)args[T_MODEL]);
  if (args[T_TOKENS]) j.tokens = *((LONG *)args[T_TOKENS]);
  if (args[T_INSTR]) {
    const char *instr = (const char *)args[T_INSTR];
    j.instruction = (char *)malloc(strlen(instr) + 1);
    if (j.instruction) strcpy(j.instruction, instr);
  }
  if (!j.instruction) {
    printf("AmigaMCP: nothing to do - give INSTRUCTION or RESUME_JOBS\n");
    rc = 20; goto done;
  }

  {
    char stamp[32];
    dbuf result;
    now_stamp(stamp);
    if (!args[T_NOJOB]) write_pending(dir, stamp, &j);
    dbuf_init(&result);
    if (run_job(&j, key, &result)) {
      printf("\n%.*s\n", (int)result.len, result.p);
      if (!args[T_NOJOB]) finalize(dir, stamp, &j, &result);
    } else {
      if (!args[T_NOJOB])
        printf("AmigaMCP: left %s-JOB_PENDING for RESUME_JOBS\n", stamp);
      rc = 10;
    }
    dbuf_free(&result);
  }

done:
  if (j.instruction) free(j.instruction);
  CloseLibrary(SocketBase);
  FreeArgs(rd);
  return rc;
}
