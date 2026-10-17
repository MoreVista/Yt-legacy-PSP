#ifndef YT_NET_INVIDIOUS_H
#define YT_NET_INVIDIOUS_H

#include "../common.h"
#include "json.h"

#define INV_MAX_RESULTS   40
#define INV_TITLE_LEN     160
#define INV_AUTHOR_LEN    64
#define INV_ID_LEN        24
#define INV_ITAG_LEN      16
#define INV_URL_LEN       512
/*
 * Stream URLs are signed googlevideo query strings rewritten onto the
 * Invidious host with local=true; they run to about 2100 characters.
 */
#define INV_STREAM_URL_LEN 2560

typedef struct {
	char id[INV_ID_LEN];
	char title[INV_TITLE_LEN];
	char author[INV_AUTHOR_LEN];
	int  length_seconds;
	long view_count;
	char thumbnail[INV_URL_LEN];
} inv_video_t;

typedef struct {
	inv_video_t items[INV_MAX_RESULTS];
	int         count;
} inv_results_t;

/*
 * A selectable stream. The player picks from these after inspecting
 * /api/v1/videos/:id. Progressive MP4 entries carry both audio and video in
 * one container, which is what the demuxer expects; adaptive-only entries
 * (video-only or audio-only) are kept so a manual choice is still possible.
 */
typedef struct {
	char itag[INV_ITAG_LEN];
	char container[16];      /* mp4 / webm */
	char encoding[16];       /* h264 / vp9 / av01 */
	char quality_label[24];  /* "360p", "medium" */
	char type[64];           /* video/mp4; codecs="avc1..., mp4a..." */
	char url[INV_STREAM_URL_LEN];  /* absolute http:// URL */
	int  width;
	int  height;
	int  fps;
	int  video_bitrate;      /* kbps, 0 when unknown */
	int  audio_bitrate;      /* kbps, 0 when audio-less */
	int  audio_channels;
	int  audio_sample_rate;
	long content_length;     /* bytes, -1 when unknown */
	bool has_video;
	bool has_audio;
} inv_format_t;

#define INV_MAX_FORMATS 24

typedef struct {
	char        video_id[INV_ID_LEN];
	char        title[INV_TITLE_LEN];
	char        author[INV_AUTHOR_LEN];
	int         length_seconds;
	char        description[512];
	inv_format_t formats[INV_MAX_FORMATS];
	int         format_count;
} inv_video_detail_t;

/* All of these take the configured host/port and speak plain HTTP. */
int inv_search(const char *host, int port, const char *query,
               inv_results_t *out);
int inv_trending(const char *host, int port, inv_results_t *out);
int inv_video_detail(const char *host, int port, const char *video_id,
                     inv_video_detail_t *out);

/* Chooses the best progressive MP4 the PSP can decode; -1 when none exists. */
int inv_pick_format(const inv_video_detail_t *detail);

#endif
