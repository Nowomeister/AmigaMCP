/*
 * net_plain.c - cleartext bsdsocket transport for AmigaMCP.
 *
 * Used for HTTP endpoints (a local llama.cpp, an Ollama box, or an
 * HTTPS-terminating proxy on the LAN). For real HTTPS to api.anthropic.com
 * link net_amissl.c instead. SocketBase is opened by main() in AmigaMCP.c.
 */
#include <exec/types.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <proto/socket.h>

#include <stdlib.h>
#include <string.h>

#include "net.h"

extern struct Library *SocketBase;

struct net_conn {
  int s;
};

const char *net_kind(void) { return "plain"; }

net_conn *net_open(const char *host, int port, const char **err) {
  net_conn *c;
  struct sockaddr_in addr;
  struct hostent *he;

  c = (net_conn *)malloc(sizeof(net_conn));
  if (!c) { if (err) *err = "out of memory"; return NULL; }

  c->s = socket(AF_INET, SOCK_STREAM, 0);
  if (c->s < 0) { if (err) *err = "socket() failed"; free(c); return NULL; }

  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = HTONS(port);

  he = gethostbyname((UBYTE *)host);
  if (!he || !he->h_addr_list || !he->h_addr_list[0]) {
    if (err) *err = "host not found";
    CloseSocket(c->s);
    free(c);
    return NULL;
  }
  memcpy(&addr.sin_addr, he->h_addr_list[0], sizeof(addr.sin_addr));

  if (connect(c->s, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    if (err) *err = "connect() failed";
    CloseSocket(c->s);
    free(c);
    return NULL;
  }
  return c;
}

int net_write_all(net_conn *c, const void *buf, long n) {
  const char *p = (const char *)buf;
  long sent = 0;
  while (sent < n) {
    int r = send(c->s, (UBYTE *)(p + sent), n - sent, 0);
    if (r <= 0) return -1;
    sent += r;
  }
  return 0;
}

long net_read(net_conn *c, void *buf, long n) {
  return recv(c->s, buf, n, 0);
}

void net_close(net_conn *c) {
  if (!c) return;
  CloseSocket(c->s);
  free(c);
}
