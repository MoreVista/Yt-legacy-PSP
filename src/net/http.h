#ifndef YT_NET_HTTP_H
#define YT_NET_HTTP_H

#include "../common.h"
#include <pspnet.h>
#include <pspnet_inet.h>
#include <pspnet_resolver.h>
#include <pspnet_apctl.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define HTTP_MAX_REDIR  5

typedef struct {
	int    fd;
	char  *buf;        /* received bytes; [pos, len) not yet consumed */
	int    cap;
	int    len;
	int    pos;

	bool   headers_done;
	bool   chunked;
	long   chunk_left;  /* bytes left in the current chunk; 0 = read a size line */
	bool   eof;         /* the socket has closed */
	bool   done;        /* the body has ended */

	int    status;
	long   content_length;  /* -1 when absent */
	long   served;          /* body bytes already handed to the caller */
	char   location[512];   /* Location header, for redirects */
} http_stream_t;

/* ---- network bring-up ---- */
int  net_init(void);
void net_term(void);
int  net_wait_wifi(int timeout_sec);
int  net_resolve(const char *host, struct in_addr *out);

/* ---- streaming ---- */
int  http_open(http_stream_t *hs, const char *host, int port,
               const char *method, const char *path);
int  http_read(http_stream_t *hs, void *dst, int len);
int  http_get_status(http_stream_t *hs);
void http_close(http_stream_t *hs);

/*
 * One-shot GET. Follows up to HTTP_MAX_REDIR redirects. On success stores a
 * NUL-terminated malloc'd body in *out_body and its length in *out_len.
 * Returns 0 on success, negative on failure.
 */
int  http_get(const char *host, int port, const char *path,
              char **out_body, int *out_len, int *out_status);


void url_encode(const char *in, char *out, int out_size);

/*
 * Splits an absolute or origin-form URL into its parts. The scheme, if any,
 * is ignored (the client is HTTP-only), so an https:// googlevideo URL yields
 * the same host/port/path that the equivalent http:// URL would.
 * Returns 0 on success, -1 when the URL is unusable.
 */
int  url_split(const char *url, char *host, int host_size,
               int *port, char *path, int path_size);

#endif
