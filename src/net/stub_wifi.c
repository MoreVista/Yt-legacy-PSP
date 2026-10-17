/*
 * Stub network layer for the no-WiFi diagnostic build.
 *
 * The console reports SCE_KERNEL_ERROR_LIBRARY_NOTFOUND (0x8002013C) for the
 * full build, and single-module probes show sceNet, sceNetApctl and
 * sceVideocodec each load fine on their own. This replaces the socket and
 * access-point code so the rest of the player -- GU setup, the demuxer, the
 * Media Engine -- can be exercised without any network import at all, which
 * narrows the failure to either the network stack or something else entirely.
 */
#include "../common.h"
#include "http.h"

int net_init(void)
{
	return -1;
}

void net_term(void)
{
}

int net_wait_wifi(int timeout_sec)
{
	(void)timeout_sec;
	return -1;
}

int net_resolve(const char *host, struct in_addr *out)
{
	(void)host;
	(void)out;
	return -1;
}

int http_open(http_stream_t *hs, const char *host, int port,
              const char *method, const char *path)
{
	(void)host; (void)port; (void)method; (void)path;
	if (hs)
		memset(hs, 0, sizeof(*hs));
	hs->fd = -1;
	return -1;
}

int http_read(http_stream_t *hs, void *dst, int len)
{
	(void)hs; (void)dst; (void)len;
	return 0;
}

int http_get_status(http_stream_t *hs)
{
	(void)hs;
	return 0;
}

void http_close(http_stream_t *hs)
{
	if (hs)
		memset(hs, 0, sizeof(*hs));
}

int http_get(const char *host, int port, const char *path,
             char **out_body, int *out_len, int *out_status)
{
	(void)host; (void)port; (void)path;
	*out_body = NULL;
	*out_len = 0;
	if (out_status)
		*out_status = 0;
	return -1;
}

int url_split(const char *url, char *host, int host_size,
              int *port, char *path, int path_size)
{
	(void)url;
	if (host && host_size > 0) host[0] = 0;
	if (port) *port = 0;
	if (path && path_size > 0) path[0] = 0;
	return -1;
}

void url_encode(const char *in, char *out, int out_size)
{
	(void)in;
	if (out && out_size > 0)
		out[0] = 0;
}
