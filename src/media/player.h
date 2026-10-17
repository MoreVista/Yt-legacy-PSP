#ifndef YT_MEDIA_PLAYER_H
#define YT_MEDIA_PLAYER_H

#include "../common.h"
#include "mp4demux.h"
#include "video.h"
#include "audio.h"
#include "download.h"

/*
 * Playback of a downloaded progressive MP4.
 *
 * Video samples are decoded in file (decode) order and paced against the
 * audio clock -- the timestamp of the AAC block being played -- so pausing
 * the audio stops the picture too. Without an audio track the system clock
 * stands in. Seeking restarts at the keyframe at or
 * before the target, which is what the stss table provides.
 */

typedef enum {
	PLAYER_IDLE,
	PLAYER_READY,
	PLAYER_PLAYING,
	PLAYER_PAUSED,
	PLAYER_ENDED,
	PLAYER_ERROR
} player_state_t;

typedef struct {
	player_state_t state;

	mp4_track_t     track;
	video_decoder_t decoder;
	audio_player_t  audio;
	bool            has_audio;
	SceUID          fd;              /* the MP4, open for the whole playback */

	char         path[256];
	int          current_sample;
	uint32_t     current_time;       /* decode time of the next sample */
	uint64_t     clock_base_us;      /* system time at which time 0 is due */
	int          width;
	int          height;
	int          frames_decoded;
	int          frames_failed;
	int          frames_late;        /* samples decoded without being shown */
	bool         have_picture;       /* decoder.rgb holds a frame */

	/* Set while the file is still downloading; NULL for a complete file. */
	const download_state_t *dl;
	bool         buffering;          /* waiting for the download */

	uint8_t     *sample_buf;         /* one MP4 sample, 64-byte aligned */
	int          sample_buf_size;

	char         message[128];
} player_t;

int  player_open(player_t *p, const char *path);

/*
 * Plays a file that is still downloading: samples past dl->received are not
 * read, and playback pauses to buffer when it catches up with the download.
 */
void player_attach_download(player_t *p, const download_state_t *dl);
void player_close(player_t *p);

/*
 * Decodes whatever samples are due now. Call once per display frame.
 * Returns false when playback has finished or failed.
 */
bool player_step(player_t *p);

/* Restarts at the keyframe at or before `seconds`. */
int  player_seek(player_t *p, int seconds);

void player_play(player_t *p);
void player_pause(player_t *p);
void player_toggle_pause(player_t *p);

double player_position_seconds(const player_t *p);
double player_duration_seconds(const player_t *p);

#endif
