#include "http.h"
#include "../util/log.h"
#include <ctype.h>
#include <psputility.h>

/* newlib here has no strcasestr(); emulate it for header scanning. */
static char *find_ci(const char *hay, const char *needle)
{
	size_t n = strlen(needle);
	for (; *hay; hay++) {
		size_t i = 0;
		while (i < n && hay[i] &&
		       tolower((unsigned char)hay[i]) == tolower((unsigned char)needle[i]))
			i++;
		if (i == n)
			return (char *)hay;
	}
	return NULL;
}

/* ------------------------------------------------------------------ */
/* network bring-up                                                    */
/* ------------------------------------------------------------------ */

static bool  g_net_up = false;
static int  g_resolver_id = -1;
static void *g_resolver_buf = NULL;

/*
 * The network modules are not linked in, and sceNetInit fails with
 * SCE_KERNEL_ERROR_LIBRARY_IS_NOT_LINKED (0x8002013a) unless they are loaded
 * first -- the XMB only loads them for the system menu. Loading them by path
 * does not work: the firmware files are pspnet*.prx, not net*.prx, and the
 * guessed names all came back 0x80010002 on the device. sceUtilityLoadNetModule
 * is the route retail games use, and the SDK's netdialog sample does the same.
 *
 * COMMON brings in pspnet, INET brings in inet/apctl/resolver. A module that is
 * already loaded returns an error here, which is harmless, so failures are only
 * logged and sceNetInit reports whether the stack is actually usable.
 */
static void net_load_modules(void)
{
	int r = sceUtilityLoadNetModule(PSP_NET_MODULE_COMMON);
	log_printf("[net] load COMMON = 0x%08x", r);
	r = sceUtilityLoadNetModule(PSP_NET_MODULE_INET);
	log_printf("[net] load INET = 0x%08x", r);
}

int net_init(void)
{
	int r;

	if (g_net_up)
		return 0;

	net_load_modules();

	/*
	 * The first argument is the memory pool for the whole network stack.
	 * 0x100 (256 bytes) passed in PPSSPP, which ignores it, and hung the
	 * real console inside sceNetInit. These are the SDK samples' values.
	 */
	log_printf("[net] sceNetInit...");
	r = sceNetInit(0x20000, 0x20, 0x1000, 0x20, 0x1000);
	if (r < 0) {
		log_printf("[net] sceNetInit failed 0x%08x", r);
		return r;
	}

	log_printf("[net] sceNetApctlInit...");
	r = sceNetApctlInit(0x1800, 0x30);
	if (r < 0) {
		log_printf("[net] sceNetApctlInit failed 0x%08x", r);
		goto fail_net;
	}

	log_printf("[net] sceNetInetInit...");
	r = sceNetInetInit();
	if (r < 0) {
		log_printf("[net] sceNetInetInit failed 0x%08x", r);
		goto fail_apctl;
	}

	log_printf("[net] sceNetResolverInit...");
	r = sceNetResolverInit();
	if (r < 0) {
		log_printf("[net] sceNetResolverInit failed 0x%08x", r);
		goto fail_inet;
	}

	g_resolver_buf = malloc(0x4000);
	if (!g_resolver_buf) {
		log_printf("[net] no memory for resolver");
		goto fail_resolver;
	}

	r = sceNetResolverCreate(&g_resolver_id, g_resolver_buf, 0x4000);
	if (r < 0) {
		log_printf("[net] sceNetResolverCreate failed 0x%08x", r);
		free(g_resolver_buf);
		g_resolver_buf = NULL;
		goto fail_resolver;
	}

	g_net_up = true;
	log_printf("[net] initialised");
	return 0;

fail_resolver:
	sceNetResolverTerm();
fail_inet:
	sceNetInetTerm();
fail_apctl:
	sceNetApctlTerm();
fail_net:
	sceNetTerm();
	return r;
}

void net_term(void)
{
	if (!g_net_up)
		return;

	if (g_resolver_id >= 0) {
		sceNetResolverDelete(g_resolver_id);
		g_resolver_id = -1;
	}
	if (g_resolver_buf) {
		free(g_resolver_buf);
		g_resolver_buf = NULL;
	}
	sceNetResolverTerm();
	sceNetInetTerm();
	sceNetApctlTerm();
	sceNetTerm();
	sceUtilityUnloadNetModule(PSP_NET_MODULE_INET);
	sceUtilityUnloadNetModule(PSP_NET_MODULE_COMMON);
	g_net_up = false;
}

/*
 * Apctl state 4 is "got IP"; 3 is still waiting on DHCP, so a socket opened
 * then has no address. Profiles are 1-based, as in the SDK's net samples:
 * 1 is the first connection saved in the XMB network settings.
 */
#define APCTL_STATE_GOT_IP 4

int net_wait_wifi(int timeout_sec)
{
	int state = 0;

	sceNetApctlGetState(&state);
	if (state == APCTL_STATE_GOT_IP)
		return 0;

	log_printf("[net] connecting to access point (profile 1)...");
	int r = sceNetApctlConnect(1);
	if (r < 0)
		log_printf("[net] sceNetApctlConnect failed 0x%08x", r);

	int last = -1;
	for (int i = 0; i < timeout_sec * 10; i++) {
		sceKernelDelayThread(100000);   /* 100 ms */
		sceNetApctlGetState(&state);
		if (state != last) {
			log_printf("[net] apctl state %d", state);
			last = state;
		}
		if (state == APCTL_STATE_GOT_IP) {
			log_printf("[net] link up");
			return 0;
		}
	}

	log_printf("[net] wifi timeout (state=%d)", state);
	return -1;
}

int net_resolve(const char *host, struct in_addr *out)
{
	/* inet_addr returns INADDR_NONE (0xffffffff) for a failed parse. */
	unsigned long ip = inet_addr(host);
	if (ip != 0xFFFFFFFFu) {
		out->s_addr = (uint32_t)ip;
		return 0;
	}

	memset(out, 0, sizeof(*out));
	int r = sceNetResolverStartNtoA(g_resolver_id, host, out, 60, 2);
	if (r < 0)
		log_printf("[net] resolve %s failed 0x%08x", host, r);
	return r;
}

/* ------------------------------------------------------------------ */
/* stream internals                                                    */
/* ------------------------------------------------------------------ */

static int send_all(int fd, const void *data, int len)
{
	const char *p = (const char *)data;
	while (len > 0) {
		int n = sceNetInetSend(fd, p, len, 0);
		if (n <= 0)
			return -1;
		p += n;
		len -= n;
	}
	return 0;
}

/*
 * Response reader. hs->buf[hs->pos .. hs->len) holds received bytes not yet
 * consumed; everything before pos is spent and is dropped on the next fill.
 * The headers are parsed once, then the body is handed out either up to
 * Content-Length, by chunk framing, or until the server closes.
 */

#define HTTP_RECV_TIMEOUT_MS 30000

/* Pull more bytes from the socket. Returns >0 on data, 0 on EOF, <0 on error. */
static int fill(http_stream_t *hs)
{
	if (hs->eof)
		return 0;

	if (hs->pos > 0) {
		memmove(hs->buf, hs->buf + hs->pos, hs->len - hs->pos);
		hs->len -= hs->pos;
		hs->pos = 0;
	}
	if (hs->cap - hs->len < 4096) {
		int newcap = hs->cap ? hs->cap * 2 : 16384;
		char *nb = realloc(hs->buf, newcap);
		if (!nb)
			return -1;
		hs->buf = nb;
		hs->cap = newcap;
	}

	/*
	 * A recv can come back EAGAIN/EINTR without the connection being over;
	 * retry those for up to HTTP_RECV_TIMEOUT_MS before giving up.
	 */
	int n;
	for (int waited = 0; ; waited += 10) {
		n = sceNetInetRecv(hs->fd, hs->buf + hs->len, hs->cap - hs->len, 0);
		if (n >= 0)
			break;
		int err = sceNetInetGetErrno();
		if ((err == 11 /* EAGAIN */ || err == 4 /* EINTR */) &&
		    waited < HTTP_RECV_TIMEOUT_MS) {
			sceKernelDelayThread(10000);
			continue;
		}
		log_printf("[http] recv failed %d errno %d", n, err);
		break;
	}
	if (n <= 0) {
		if (n == 0 && !hs->headers_done)
			log_printf("[http] connection closed before headers");
		hs->eof = true;
		return n;
	}
	hs->len += n;
	return n;
}

/* Finds "\n" in the unconsumed bytes, or NULL. */
static char *find_newline(http_stream_t *hs)
{
	return memchr(hs->buf + hs->pos, '\n', hs->len - hs->pos);
}

/* Parses the status line and headers once they are all buffered. */
static int read_headers(http_stream_t *hs)
{
	for (;;) {
		char *start = hs->buf + hs->pos;
		int avail = hs->len - hs->pos;
		char *hdr_end = NULL;
		for (int i = 0; i + 1 < avail; i++) {
			if (start[i] == '\n' &&
			    (start[i + 1] == '\n' ||
			     (start[i + 1] == '\r' && i + 2 < avail && start[i + 2] == '\n'))) {
				hdr_end = start + i + 1;
				break;
			}
		}
		if (hdr_end) {
			int body_at = (int)(hdr_end - hs->buf) + ((*hdr_end == '\r') ? 2 : 1);
			*hdr_end = 0;

			hs->status = 0;
			if (!strncmp(start, "HTTP/", 5)) {
				char *sp = strchr(start, ' ');
				if (sp)
					hs->status = atoi(sp + 1);
			}

			for (char *line = strchr(start, '\n'); line; ) {
				line++;
				char *eol = strchr(line, '\n');
				if (eol)
					*eol = 0;
				if (!strncasecmp(line, "Content-Length:", 15))
					hs->content_length = strtol(line + 15, NULL, 10);
				else if (!strncasecmp(line, "Location:", 9)) {
					const char *v = line + 9;
					while (*v == ' ')
						v++;
					snprintf(hs->location, sizeof(hs->location), "%s", v);
					char *cr = strchr(hs->location, '\r');
					if (cr)
						*cr = 0;
				} else if (!strncasecmp(line, "Transfer-Encoding:", 18) &&
				         find_ci(line, "chunked"))
					hs->chunked = true;
				line = eol;
			}

			hs->pos = body_at;
			hs->headers_done = true;
			return 0;
		}
		if (fill(hs) <= 0)
			return -1;
	}
}

/*
 * Moves to the next chunk's data. Each chunk is "<hex size>\r\n<data>\r\n",
 * and a zero size ends the body. Returns 1 at data, 0 at the end of the body,
 * <0 on a truncated stream.
 */
static int next_chunk(http_stream_t *hs)
{
	for (;;) {
		/* The CRLF that closes the previous chunk's data. */
		while (hs->pos < hs->len &&
		       (hs->buf[hs->pos] == '\r' || hs->buf[hs->pos] == '\n'))
			hs->pos++;

		char *nl = find_newline(hs);
		if (!nl) {
			if (fill(hs) <= 0)
				return -1;
			continue;
		}
		*nl = 0;
		long size = strtol(hs->buf + hs->pos, NULL, 16);
		hs->pos = (int)(nl - hs->buf) + 1;
		if (size <= 0) {
			hs->done = true;
			return 0;
		}
		hs->chunk_left = size;
		return 1;
	}
}

int http_open(http_stream_t *hs, const char *host, int port,
              const char *method, const char *path)
{
	struct in_addr addr;
	int fd, r;

	memset(hs, 0, sizeof(*hs));
	hs->fd = -1;
	hs->content_length = -1;

	r = net_resolve(host, &addr);
	if (r < 0)
		return r;

	fd = sceNetInetSocket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		log_printf("[http] socket failed 0x%08x", fd);
		return fd;
	}

	struct sockaddr_in sa;
	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_port = htons((uint16_t)port);
	sa.sin_addr = addr;

	r = sceNetInetConnect(fd, (struct sockaddr *)&sa, sizeof(sa));
	if (r < 0) {
		log_printf("[http] connect %s:%d failed 0x%08x", host, port, r);
		sceNetInetClose(fd);
		return r;
	}
	hs->fd = fd;

	/* Stream paths are signed query strings of ~2 KB, so size for those. */
	char req[3584];
	int n = snprintf(req, sizeof(req),
		"%s %s HTTP/1.1\r\n"
		"Host: %s:%d\r\n"
		"User-Agent: Mozilla/5.0 (PSP; YtLegacyPSP/1.0)\r\n"
		"Accept: */*\r\n"
		"Connection: close\r\n"
		"\r\n",
		method, path, host, port);
	if (n >= (int)sizeof(req)) {
		log_printf("[http] request too long (%d bytes)", n);
		http_close(hs);
		return -1;
	}

	if (send_all(fd, req, n) < 0) {
		http_close(hs);
		return -1;
	}
	return 0;
}

int http_read(http_stream_t *hs, void *dst, int len)
{
	char *out = (char *)dst;
	int produced = 0;

	if (!hs->headers_done && read_headers(hs) < 0)
		return -1;

	while (produced < len && !hs->done) {
		long limit = len - produced;

		if (hs->chunked) {
			if (hs->chunk_left == 0 && next_chunk(hs) <= 0)
				break;
			if (limit > hs->chunk_left)
				limit = hs->chunk_left;
		} else if (hs->content_length >= 0) {
			long remaining = hs->content_length - hs->served;
			if (remaining <= 0) {
				hs->done = true;
				break;
			}
			if (limit > remaining)
				limit = remaining;
		}

		if (hs->pos >= hs->len) {
			if (fill(hs) <= 0) {
				hs->done = true;   /* closed: the body ends here */
				break;
			}
			continue;
		}

		int avail = hs->len - hs->pos;
		if (avail > limit)
			avail = (int)limit;
		memcpy(out + produced, hs->buf + hs->pos, avail);
		hs->pos += avail;
		hs->served += avail;
		produced += avail;
		if (hs->chunked)
			hs->chunk_left -= avail;
	}

	return produced;
}

int http_get_status(http_stream_t *hs)
{
	return hs->status;
}

void http_close(http_stream_t *hs)
{
	if (hs->fd >= 0) {
		sceNetInetClose(hs->fd);
		hs->fd = -1;
	}
	if (hs->buf) {
		free(hs->buf);
		hs->buf = NULL;
	}
	hs->cap = hs->len = hs->pos = 0;
}

/*
 * Follow redirects by re-issuing GETs. Only absolute-path locations are
 * followed; absolute URLs are re-parsed into host/port/path.
 */
int url_split(const char *url, char *host, int hostcap,
              int *port, char *path, int pathcap)
{
	if (!url || !*url)
		return -1;

	const char *p = strstr(url, "://");
	p = p ? p + 3 : url;

	const char *slash = strchr(p, '/');
	const char *end = slash ? slash : p + strlen(p);

	const char *colon = memchr(p, ':', end - p);
	if (colon) {
		int hl = (int)(colon - p);
		if (hl >= hostcap)
			hl = hostcap - 1;
		memcpy(host, p, hl);
		host[hl] = 0;
		*port = atoi(colon + 1);
	} else {
		int hl = (int)(end - p);
		if (hl >= hostcap)
			hl = hostcap - 1;
		memcpy(host, p, hl);
		host[hl] = 0;
		*port = 80;
	}

	if (slash)
		snprintf(path, pathcap, "%s", slash);
	else
		snprintf(path, pathcap, "/");
	return 0;
}

int http_get(const char *host, int port, const char *path,
             char **out_body, int *out_len, int *out_status)
{
	char cur_host[128], cur_path[512];
	int  cur_port = port;

	snprintf(cur_host, sizeof(cur_host), "%s", host);
	snprintf(cur_path, sizeof(cur_path), "%s", path);

	for (int redirect = 0; redirect <= HTTP_MAX_REDIR; redirect++) {
		http_stream_t hs;
		if (http_open(&hs, cur_host, cur_port, "GET", cur_path) < 0)
			return -1;

		/* Pull just enough to learn the status and any Location header. */
		char scratch[4096];
		int got = http_read(&hs, scratch, sizeof(scratch) - 1);
		if (got < 0) {
			http_close(&hs);
			return -1;
		}
		scratch[got] = 0;

		int status = http_get_status(&hs);

		if (status == 301 || status == 302 || status == 303 ||
		    status == 307 || status == 308) {
			const char *loc = hs.location;
			if (!*loc) {
				http_close(&hs);
				return -1;
			}
			if (strstr(loc, "://"))
				url_split(loc, cur_host, sizeof(cur_host),
				          &cur_port, cur_path, sizeof(cur_path));
			else
				snprintf(cur_path, sizeof(cur_path), "%s", loc);
			log_printf("[http] %d -> %s", status, cur_path);
			http_close(&hs);
			continue;
		}

		if (status < 200 || status >= 300) {
			log_printf("[http] status %d for %s", status, cur_path);
			http_close(&hs);
			if (out_status)
				*out_status = status;
			return -1;
		}

		/* Buffer the whole body. */
		int cap = got + 65536;
		char *body = malloc(cap);
		if (!body) {
			http_close(&hs);
			return -1;
		}
		memcpy(body, scratch, got);
		int total = got;

		for (;;) {
			if (total + 32768 > cap) {
				int ncap = cap * 2;
				char *nb = realloc(body, ncap);
				if (!nb) {
					free(body);
					http_close(&hs);
					return -1;
				}
				body = nb;
				cap = ncap;
			}
			int n = http_read(&hs, body + total, cap - total);
			if (n <= 0)
				break;
			total += n;
		}
		http_close(&hs);

		body[total] = 0;
		*out_body = body;
		*out_len = total;
		if (out_status)
			*out_status = status;
		return 0;
	}

	log_printf("[http] too many redirects");
	return -1;
}

void url_encode(const char *in, char *out, int out_size)
{
	static const char hex[] = "0123456789ABCDEF";
	int o = 0;
	for (const unsigned char *p = (const unsigned char *)in; *p && o < out_size - 4; p++) {
		if (*p == ' ')
			out[o++] = '+';
		else if (isalnum(*p) || *p == '-' || *p == '_' || *p == '.' || *p == '~')
			out[o++] = (char)*p;
		else {
			out[o++] = '%';
			out[o++] = hex[(*p >> 4) & 0xF];
			out[o++] = hex[*p & 0xF];
		}
	}
	out[o] = 0;
}
