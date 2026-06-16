/*
 * net.h - transport seam for AmigaMCP.
 *
 * Two interchangeable backends implement this tiny interface:
 *   net_plain.c   - raw bsdsocket TCP (HTTP, or HTTPS-terminating proxy)
 *   net_amissl.c  - AmiSSL TLS over bsdsocket (real HTTPS), built with
 *                   -DUSE_AMISSL when the AmiSSL SDK is present.
 *
 * The HTTP logic in AmigaMCP.c only ever talks to this seam, so switching
 * between cleartext and TLS is a link-time choice, nothing more.
 */
#ifndef AMIGAMCP_NET_H
#define AMIGAMCP_NET_H

typedef struct net_conn net_conn;

/* Open a connection to host:port. On failure returns NULL and, if err is
 * non-NULL, points *err at a static human-readable reason. */
net_conn *net_open(const char *host, int port, const char **err);

/* Write exactly n bytes (loops internally). Returns 0 on success, -1 on error. */
int net_write_all(net_conn *c, const void *buf, long n);

/* Read up to n bytes. Returns count (>0), 0 at end of stream, -1 on error. */
long net_read(net_conn *c, void *buf, long n);

void net_close(net_conn *c);

/* "plain" or "tls" - for banners/diagnostics. */
const char *net_kind(void);

#endif
