/* Native (host) test of AmigaMCP's portable logic: extract_text + apply_lines.
 * These are byte-for-byte the functions from AmigaMCP.c, with the AmigaDOS
 * bits removed. Run with any host gcc. Not part of the Amiga build. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "jsmn.h"

typedef struct { unsigned char *p; long len; long cap; } dbuf;
static int dbuf_init(dbuf *d){d->p=malloc(256);d->len=0;d->cap=d->p?256:0;return d->p!=NULL;}
static void dbuf_free(dbuf *d){if(d->p)free(d->p);d->p=NULL;d->len=d->cap=0;}
static int dbuf_ensure(dbuf *d,long e){long n=d->len+e;if(n<=d->cap)return 1;while(d->cap<n)d->cap=d->cap<256?256:d->cap*2;d->p=realloc(d->p,d->cap);return d->p!=NULL;}
static void dbuf_putb(dbuf *d,int c){if(dbuf_ensure(d,1))d->p[d->len++]=(unsigned char)c;}
static void dbuf_put(dbuf *d,const void *s,long n){if(n<=0)return;if(dbuf_ensure(d,n)){memcpy(d->p+d->len,s,n);d->len+=n;}}

static int tok_eq(const char *js,jsmntok_t *t,const char *s){int len=t->end-t->start;return ((int)strlen(s)==len)&&strncmp(js+t->start,s,len)==0;}
static int tok_skip(jsmntok_t *t,int i){int j,n;switch(t[i].type){case JSMN_OBJECT:n=t[i].size;i++;for(j=0;j<n;j++){i=tok_skip(t,i);i=tok_skip(t,i);}return i;case JSMN_ARRAY:n=t[i].size;i++;for(j=0;j<n;j++)i=tok_skip(t,i);return i;default:return i+1;}}
static int find_key(const char *js,jsmntok_t *t,int oi,const char *key){int n,j,i;if(oi<0||t[oi].type!=JSMN_OBJECT)return -1;n=t[oi].size;i=oi+1;for(j=0;j<n;j++){int ki=i,vi=i+1;if(t[ki].type==JSMN_STRING&&tok_eq(js,&t[ki],key))return vi;i=tok_skip(t,vi);}return -1;}
static void tok_unescape_into(dbuf *d,const char *js,jsmntok_t *t){int n=t->end-t->start,i=0;const char *s=js+t->start;while(i<n){char c=s[i++];if(c=='\\'&&i<n){char e=s[i++];switch(e){case 'n':dbuf_putb(d,'\n');break;case 't':dbuf_putb(d,'\t');break;case '"':dbuf_putb(d,'"');break;case '\\':dbuf_putb(d,'\\');break;case '/':dbuf_putb(d,'/');break;case 'b':dbuf_putb(d,'\b');break;case 'f':dbuf_putb(d,'\f');break;case 'u':if(i+4<=n){int v=0,k;for(k=0;k<4;k++){char h=s[i+k];v=(v<<4)|((h>='0'&&h<='9')?h-'0':(h>='a'&&h<='f')?h-'a'+10:(h>='A'&&h<='F')?h-'A'+10:0);}i+=4;dbuf_putb(d,v<256?(char)v:'?');}break;default:dbuf_putb(d,e);}}else dbuf_putb(d,c);}}

#define MAX_TOKENS 2048
struct job{char model[128];char provider[32];char host[128];char path[160];int port;char version[40];long tokens;char *instruction;};

static int extract_text(const char *body,long blen,const struct job *j,dbuf *out,int *iserr){
  jsmn_parser pr;jsmntok_t *tok;int n,ci,ei;*iserr=0;tok=malloc(sizeof(jsmntok_t)*MAX_TOKENS);if(!tok)return 0;
  jsmn_init(&pr);n=jsmn_parse(&pr,body,blen,tok,MAX_TOKENS);if(n<1||tok[0].type!=JSMN_OBJECT){free(tok);return 0;}
  if(!strcmp(j->provider,"openai")){ci=find_key(body,tok,0,"choices");if(ci>=0&&tok[ci].type==JSMN_ARRAY&&tok[ci].size>0){int e0=ci+1;int msg=find_key(body,tok,e0,"message");if(msg>=0){int ct=find_key(body,tok,msg,"content");if(ct>=0&&tok[ct].type==JSMN_STRING){tok_unescape_into(out,body,&tok[ct]);free(tok);return 1;}}}}
  else{ci=find_key(body,tok,0,"content");if(ci>=0&&tok[ci].type==JSMN_ARRAY&&tok[ci].size>0){int e0=ci+1;int ti=find_key(body,tok,e0,"text");if(ti>=0&&tok[ti].type==JSMN_STRING){tok_unescape_into(out,body,&tok[ti]);free(tok);return 1;}}}
  ei=find_key(body,tok,0,"error");if(ei>=0&&tok[ei].type==JSMN_OBJECT){int mi=find_key(body,tok,ei,"message");if(mi>=0&&tok[mi].type==JSMN_STRING){tok_unescape_into(out,body,&tok[mi]);*iserr=1;free(tok);return 1;}}
  ei=find_key(body,tok,0,"message");if(ei>=0&&tok[ei].type==JSMN_STRING){tok_unescape_into(out,body,&tok[ei]);*iserr=1;free(tok);return 1;}
  free(tok);return 0;
}

static void set_field(char *dst,int cap,const char *val){int n=0;while(*val==' '||*val=='\t')val++;while(val[n]&&val[n]!='\n'&&val[n]!='\r'&&n<cap-1)n++;while(n>0&&(val[n-1]==' '||val[n-1]=='\t'))n--;memcpy(dst,val,n);dst[n]=0;}
static void job_defaults(struct job *j){strcpy(j->model,"claude-opus-4-8");strcpy(j->provider,"anthropic");strcpy(j->host,"api.anthropic.com");strcpy(j->path,"/v1/messages");j->port=443;strcpy(j->version,"2023-06-01");j->tokens=4096;j->instruction=NULL;}
static void apply_lines(char *text,struct job *j,char *keyout,int keycap){
  char *p=text;
  while(p&&*p){char *eol=p;char *kw=p;int kwlen;while(*eol&&*eol!='\n')eol++;while(*kw==' '||*kw=='\t')kw++;kwlen=0;while(kw[kwlen]&&kw[kwlen]!=' '&&kw[kwlen]!='\t'&&kw[kwlen]!='\n'&&kw[kwlen]!='\r')kwlen++;
    if(kwlen&&kw[0]!='#'&&kw[0]!=';'){const char *val=kw+kwlen;
      if(!strncmp(kw,"key",3)&&kwlen==3&&keyout)set_field(keyout,keycap,val);
      else if(!strncmp(kw,"model",5)&&kwlen==5)set_field(j->model,sizeof(j->model),val);
      else if(!strncmp(kw,"provider",8)&&kwlen==8)set_field(j->provider,sizeof(j->provider),val);
      else if(!strncmp(kw,"host",4)&&kwlen==4)set_field(j->host,sizeof(j->host),val);
      else if(!strncmp(kw,"path",4)&&kwlen==4)set_field(j->path,sizeof(j->path),val);
      else if(!strncmp(kw,"port",4)&&kwlen==4){char b[16];set_field(b,sizeof(b),val);j->port=atoi(b);}
      else if(!strncmp(kw,"version",7)&&kwlen==7)set_field(j->version,sizeof(j->version),val);
      else if(!strncmp(kw,"tokens",6)&&kwlen==6){char b[16];set_field(b,sizeof(b),val);j->tokens=atol(b);}
      else if(!strncmp(kw,"instruction",11)&&kwlen==11){char *start=(*eol=='\n')?eol+1:eol;long len=(long)strlen(start);j->instruction=malloc(len+1);if(j->instruction)memcpy(j->instruction,start,len+1);return;}
    }
    p=(*eol=='\n')?eol+1:eol;
  }
}

static int fail=0;
static void check(const char *label,int cond){printf("  [%s] %s\n",cond?"PASS":"FAIL",label);if(!cond)fail=1;}
static void show(const char *label,const char *body,const char *prov,const char *expect,int experr){
  struct job j;job_defaults(&j);strcpy(j.provider,prov);
  dbuf o;dbuf_init(&o);int e=0;
  int ok=extract_text(body,strlen(body),&j,&o,&e);dbuf_putb(&o,0);
  printf("%s: ok=%d err=%d -> [%s]\n",label,ok,e,ok?(char*)o.p:"(none)");
  if(expect){check("text matches",ok&&!strcmp((char*)o.p,expect));check("err flag",e==experr);}
  else check("correctly returned nothing",!ok);
  dbuf_free(&o);
}

int main(void){
  const char *ANTH = "Mange des nouilles\nMiam \xe9"; /* é -> 0xe9 Latin-1 */
  printf("=== extract_text ===\n");
  show("anthropic ok","{\"id\":\"msg_1\",\"content\":[{\"type\":\"text\",\"text\":\"Mange des nouilles\\nMiam \\u00e9\"}],\"model\":\"x\"}","anthropic",ANTH,0);
  show("anthropic err","{\"type\":\"error\",\"error\":{\"type\":\"auth\",\"message\":\"invalid x-api-key\"}}","anthropic","invalid x-api-key",1);
  show("openai ok","{\"choices\":[{\"index\":0,\"message\":{\"role\":\"assistant\",\"content\":\"Bonjour Amiga\"}}]}","openai","Bonjour Amiga",0);
  show("openai err","{\"error\":{\"message\":\"model not found\",\"code\":404}}","openai","model not found",1);
  show("garbage",">>> not json","anthropic",NULL,0);
  show("empty content arr","{\"content\":[]}","anthropic",NULL,0);

  printf("\n=== apply_lines (config) ===\n");
  {
    char cfg[]="# comment\nkey  sk-ant-XYZ \nmodel claude-opus-4-8\nprovider openai\nport 8080\ntokens 14400\n";
    struct job j;job_defaults(&j);char key[256]={0};apply_lines(cfg,&j,key,sizeof key);
    check("key parsed",!strcmp(key,"sk-ant-XYZ"));
    check("model parsed",!strcmp(j.model,"claude-opus-4-8"));
    check("provider parsed",!strcmp(j.provider,"openai"));
    check("port parsed",j.port==8080);
    check("tokens parsed",j.tokens==14400);
    check("comment ignored / no instruction",j.instruction==NULL);
  }

  printf("\n=== apply_lines (job file, multiline instruction) ===\n");
  {
    char jb[]="status pending\nmodel claude-opus-4-8\ntokens 14400\ninstruction\nmange des nouilles\nsur deux lignes\n";
    struct job j;job_defaults(&j);char key[256]={0};apply_lines(jb,&j,key,sizeof key);
    check("tokens",j.tokens==14400);
    check("instruction multiline",j.instruction&&!strcmp(j.instruction,"mange des nouilles\nsur deux lignes\n"));
    printf("  instruction=[%s]\n",j.instruction?j.instruction:"(null)");
  }

  printf("\n%s\n",fail?"*** SOME TESTS FAILED ***":"ALL TESTS PASSED");
  return fail;
}
