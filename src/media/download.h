#ifndef YT_MEDIA_DOWNLOAD_H
#define YT_MEDIA_DOWNLOAD_H

#include "../common.h"
#include "../net/http.h"
#include <pspctrl.h>

#define DOWNLOAD_CHUNK 32768

typedef struct {
	volatile bool cancel;
	volatile bool finished;
	volatile int  result;
	volatile long received;    /* bytes on the memory stick so far */
	volatile bool done;        /* the background download has returned */
	SceUID thread;
	long   expected;      /* -1 when the server did not report a length */
	int    last_percent;
	char   path[256];
} download_state_t;

void download_begin(download_state_t *st, const char *url, const char *path,
                    long expected);
void download_poll(download_state_t *st);

/* Blocking download that runs on the calling thread and yields to the UI. */
int  download_run(download_state_t *st, const char *url, const char *path,
                  long expected, void (*progress)(download_state_t *, void *),
                  void *user);

/*
 * Background download, for playing while the file arrives: runs download_run
 * on its own thread. Watch `received`, `done` and `result`; set `cancel` to
 * stop it. download_join must be called before the state is reused.
 */
int  download_start(download_state_t *st, const char *url, const char *path,
                    long expected);
void download_join(download_state_t *st);

/* True when the button combination currently held means "abort". */
bool download_cancel_requested(void);

#endif
