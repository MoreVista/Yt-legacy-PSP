#include "download.h"
#include "../util/log.h"
#include "../util/config.h"
#include <pspiofilemgr.h>
#include <pspthreadman.h>

/*
 * googlevideo signed URLs carry the whole request in the query string and run
 * past a thousand characters, so the path buffer is sized for that rather than
 * for the short Invidious API paths.
 */
#define URL_PATH_MAX 3072

typedef struct {
	download_state_t *st;
	char host[256];
	int  port;
	char path[URL_PATH_MAX];
} dl_args_t;

bool download_cancel_requested(void)
{
	return false;
}

void download_begin(download_state_t *st, const char *url, const char *path,
                    long expected)
{
	(void)url;
	memset(st, 0, sizeof(*st));
	st->thread = -1;
	st->expected = expected;
	snprintf(st->path, sizeof(st->path), "%s", path);
}

void download_poll(download_state_t *st)
{
	(void)st;
}

int download_run(download_state_t *st, const char *url, const char *path,
                 long expected, void (*progress)(download_state_t *, void *),
                 void *user)
{
	dl_args_t args;
	memset(&args, 0, sizeof(args));
	args.st = st;

	st->expected = expected;
	st->received = 0;
	st->result = -1;
	snprintf(st->path, sizeof(st->path), "%s", path);

	if (url_split(url, args.host, sizeof(args.host), &args.port,
	              args.path, sizeof(args.path)) < 0) {
		log_printf("[dl] bad url: %s", url);
		return -1;
	}

	log_printf("[dl] %s -> %s:%d (%.60s)", st->path, args.host, args.port,
	           args.path);

	http_stream_t hs;
	int status = 0;
	if (http_open(&hs, args.host, args.port, "GET", args.path) < 0) {
		log_printf("[dl] connect failed");
		return -1;
	}

	/* Read enough to learn the status and the content length. */
	char probe[4096];
	int got = http_read(&hs, probe, sizeof(probe));
	if (got < 0) {
		http_close(&hs);
		return -1;
	}
	status = http_get_status(&hs);
	/* Muxed formats often carry no clen; the response itself knows. */
	if (st->expected <= 0 && hs.content_length > 0)
		st->expected = hs.content_length;
	if (status != 200 && status != 206) {
		log_printf("[dl] http status %d", status);
		http_close(&hs);
		return -1;
	}

	SceUID fd = sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
	if (fd < 0) {
		log_printf("[dl] cannot open %s (0x%08x)", path, fd);
		http_close(&hs);
		return -1;
	}

	/* Replay what the probe already consumed. */
	long written = 0;
	if (got > 0) {
		sceIoWrite(fd, probe, got);
		written += got;
	}
	st->received = written;

	char buf[DOWNLOAD_CHUNK];
	for (;;) {
		if (st->cancel) {
			log_printf("[dl] cancelled at %ld bytes", st->received);
			sceIoClose(fd);
			sceIoRemove(path);
			http_close(&hs);
			st->result = -2;
			return -2;
		}

		int n = http_read(&hs, buf, sizeof(buf));
		if (n <= 0)
			break;

		int w = sceIoWrite(fd, buf, n);
		if (w != n) {
			log_printf("[dl] write failed (wrote %d of %d)", w, n);
			sceIoClose(fd);
			sceIoRemove(path);
			http_close(&hs);
			return -1;
		}
		st->received += n;

		if (progress)
			progress(st, user);

		/* Keep the UI thread responsive. */
		sceKernelDelayThread(0);
#ifdef DOWNLOAD_THROTTLE_KBPS
		/* Diagnostic builds: simulate a slow link to exercise buffering. */
		sceKernelDelayThread((int)((long long)n * 1000000 /
		                           (DOWNLOAD_THROTTLE_KBPS * 1024)));
#endif
	}

	sceIoClose(fd);
	http_close(&hs);

	st->finished = true;
	st->result = 0;
	log_printf("[dl] done: %ld bytes -> %s", st->received, path);
	return 0;
}

typedef struct {
	download_state_t *st;
	long  expected;
	char  path[256];
	char  url[URL_PATH_MAX];
} dl_job_t;

static int download_thread(SceSize args, void *argp)
{
	dl_job_t *job = *(dl_job_t **)argp;
	(void)args;
	int r = download_run(job->st, job->url, job->path, job->expected,
	                     NULL, NULL);
	job->st->result = r;
	job->st->done = true;
	free(job);
	return 0;
}

int download_start(download_state_t *st, const char *url, const char *path,
                   long expected)
{
	download_begin(st, url, path, expected);
	dl_job_t *job = malloc(sizeof(*job));
	if (!job)
		return -1;
	job->st = st;
	job->expected = expected;
	snprintf(job->path, sizeof(job->path), "%s", path);
	snprintf(job->url, sizeof(job->url), "%s", url);

	/*
	 * Below the main thread's priority (32): playback keeps the CPU it
	 * needs, and the download takes the time it spends waiting for vblank.
	 * The stack holds download_run's 32 KB chunk buffer.
	 */
	st->thread = sceKernelCreateThread("download", download_thread, 36,
	                                   0x10000, PSP_THREAD_ATTR_USER, NULL);
	if (st->thread < 0) {
		log_printf("[dl] thread failed 0x%08x", st->thread);
		free(job);
		return -1;
	}
	sceKernelStartThread(st->thread, sizeof(job), &job);
	return 0;
}

void download_join(download_state_t *st)
{
	if (st->thread < 0)
		return;
	sceKernelWaitThreadEnd(st->thread, NULL);
	sceKernelDeleteThread(st->thread);
	st->thread = -1;
}
