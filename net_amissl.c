/*
 * net_amissl.c - real HTTPS transport for AmigaMCP, via AmiSSL 5.x.
 *
 * The production backend: TLS to api.anthropic.com straight from the Amiga,
 * no proxy, no bridge. Built only with -DUSE_AMISSL and the AmiSSL SDK on the
 * include path (see the Makefile's `amissl` target).
 *
 * The init/seed sequence below follows the official AmiSSL SDK example
 * (Developer/Examples/https.c): open utility + bsdsocket + amisslmaster,
 * InitAmiSSLMaster (returns a BOOL - TRUE on success), OpenAmiSSL, InitAmiSSL,
 * then seed the RNG by hand (the Amiga has no /dev/urandom, so without this
 * the TLS handshake fails).
 *
 * Certificate verification is OFF by default ("pas prise de tete", and we
 * call one known fixed host). SSL_CTX_set_default_verify_paths() is wired up
 * so you can flip to SSL_VERIFY_PEER once AmiSSL's CA bundle is in place.
 */
#include <exec/types.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <errno.h>

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/socket.h>
#define __NOLIBBASE__
#include <proto/utility.h>
#undef __NOLIBBASE__
#include <proto/amissl.h>
#include <proto/amisslmaster.h>
#include <clib/alib_protos.h>      /* RangeRand / RangeSeed (amiga.lib) */

#include <amissl/amissl.h>
#include <libraries/amissl.h>
#include <libraries/amisslmaster.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/rand.h>

#include <stdlib.h>
#include <string.h>

#include "net.h"

extern struct Library *SocketBase;

/* RangeSeed isn't in every alib_protos.h; declare it (amiga.lib provides it). */
extern void RangeSeed(unsigned long seed);

struct Library *AmiSSLMasterBase = NULL;
struct Library *AmiSSLBase = NULL;
struct Library *UtilityBase = NULL;

struct net_conn {
  int s;
  SSL_CTX *ctx;
  SSL *ssl;
  int ssl_up;
};

const char *net_kind(void) { return "tls"; }

static int amissl_up = 0;

static void seed_rng(void) {
  unsigned short buf[64]; /* 128 bytes */
  struct DateStamp ds;
  int i;
  DateStamp(&ds);
  RangeSeed((ULONG)ds.ds_Tick ^ ((ULONG)ds.ds_Minute << 11) ^
            (ULONG)FindTask(NULL));
  for (i = 0; i < 64; i++) buf[i] = (unsigned short)RangeRand(65535);
  RAND_seed(buf, sizeof(buf));
}

static int amissl_start(const char **err) {
  if (amissl_up) return 1;

  if (!(UtilityBase = OpenLibrary("utility.library", 0))) {
    if (err) *err = "no utility.library";
    return 0;
  }
  if (!(AmiSSLMasterBase = OpenLibrary("amisslmaster.library",
                                       AMISSLMASTER_MIN_VERSION))) {
    if (err) *err = "no amisslmaster.library";
    return 0;
  }
  if (!InitAmiSSLMaster(AMISSL_CURRENT_VERSION, TRUE)) {
    if (err) *err = "AmiSSL version too old";
    return 0;
  }
  if (!(AmiSSLBase = OpenAmiSSL())) {
    if (err) *err = "OpenAmiSSL failed";
    return 0;
  }
  if (InitAmiSSL(AmiSSL_ErrNoPtr, (ULONG)&errno,
                 AmiSSL_SocketBase, (ULONG)SocketBase,
                 TAG_DONE) != 0) {
    if (err) *err = "InitAmiSSL failed";
    return 0;
  }

  OPENSSL_init_ssl(OPENSSL_INIT_SSL_DEFAULT | OPENSSL_INIT_ADD_ALL_CIPHERS |
                       OPENSSL_INIT_ADD_ALL_DIGESTS,
                   NULL);
  seed_rng();

  amissl_up = 1;
  return 1;
}

static void amissl_stop(void) {
  if (amissl_up) { CleanupAmiSSLA(NULL); amissl_up = 0; }
  if (AmiSSLBase) { CloseAmiSSL(); AmiSSLBase = NULL; }
  if (AmiSSLMasterBase) { CloseLibrary(AmiSSLMasterBase); AmiSSLMasterBase = NULL; }
  if (UtilityBase) { CloseLibrary(UtilityBase); UtilityBase = NULL; }
}

net_conn *net_open(const char *host, int port, const char **err) {
  net_conn *c;
  struct sockaddr_in addr;
  struct hostent *he;

  if (!amissl_start(err)) { amissl_stop(); return NULL; }

  c = (net_conn *)malloc(sizeof(net_conn));
  if (!c) { if (err) *err = "out of memory"; return NULL; }
  memset(c, 0, sizeof(*c));
  c->s = -1;

  c->s = socket(AF_INET, SOCK_STREAM, 0);
  if (c->s < 0) {
    if (err) *err = "socket() failed";
    net_close(c);
    return NULL;
  }

  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = HTONS(port);
  he = gethostbyname((UBYTE *)host);
  if (!he || !he->h_addr_list || !he->h_addr_list[0]) {
    if (err) *err = "host not found";
    net_close(c);
    return NULL;
  }
  memcpy(&addr.sin_addr, he->h_addr_list[0], sizeof(addr.sin_addr));
  if (connect(c->s, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    if (err) *err = "connect() failed";
    net_close(c);
    return NULL;
  }

  c->ctx = SSL_CTX_new(TLS_client_method());
  if (!c->ctx) {
    if (err) *err = "SSL_CTX_new failed";
    net_close(c);
    return NULL;
  }
  SSL_CTX_set_default_verify_paths(c->ctx);
  SSL_CTX_set_verify(c->ctx, SSL_VERIFY_NONE, NULL);

  c->ssl = SSL_new(c->ctx);
  if (!c->ssl) {
    if (err) *err = "SSL_new failed";
    net_close(c);
    return NULL;
  }

  SSL_set_fd(c->ssl, c->s);
  SSL_set_tlsext_host_name(c->ssl, host); /* SNI */

  if (SSL_connect(c->ssl) != 1) {
    if (err) *err = "TLS handshake failed";
    net_close(c);
    return NULL;
  }
  c->ssl_up = 1;
  return c;
}

int net_write_all(net_conn *c, const void *buf, long n) {
  const char *p = (const char *)buf;
  long sent = 0;
  while (sent < n) {
    int r = SSL_write(c->ssl, p + sent, (int)(n - sent));
    if (r <= 0) return -1;
    sent += r;
  }
  return 0;
}

long net_read(net_conn *c, void *buf, long n) {
  int r = SSL_read(c->ssl, buf, (int)n);
  if (r < 0) {
    if (SSL_get_error(c->ssl, r) == SSL_ERROR_ZERO_RETURN) return 0;
    return -1;
  }
  return r;
}

void net_close(net_conn *c) {
  if (c) {
    if (c->ssl) { if (c->ssl_up) SSL_shutdown(c->ssl); SSL_free(c->ssl); }
    if (c->ctx) SSL_CTX_free(c->ctx);
    if (c->s >= 0) CloseSocket(c->s);
    free(c);
  }
  amissl_stop();
}
