#ifndef YT_MEDIA_AUDIO_H
#define YT_MEDIA_AUDIO_H

#include "../common.h"
#include "mp4demux.h"

/*
 * AAC playback, and the clock video is paced against.
 *
 * The MP4's AAC samples go to sceAudiocodec as they are, one sample in,
 * 1024 stereo frames out, with the codec workspace in main RAM. That is
 * PMPlayer Advance's audiodecoder.c, which GoTube's imports also match.
 *
 * Decoding happens on the caller's thread (the main loop, which also drives
 * the video decoder, so the Media Engine is never used from two threads).
 * The PCM goes into a small ring that an output thread drains with
 * sceAudioSRCOutputBlocking. The SRC channel takes 44.1 or 48 kHz alike.
 *
 * The clock is the timestamp of the block being played. Pausing stops the
 * output thread, so the clock -- and the video following it -- stops too.
 */

#define AUDIO_FRAMES     1024          /* PCM frames per AAC sample */
#define AUDIO_RING       16            /* blocks; ~0.37 s at 44.1 kHz */

typedef struct {
	int16_t  pcm[AUDIO_FRAMES * 2];
	uint32_t pts;                      /* in the audio timescale */
} audio_block_t;

typedef struct {
	bool     open;
	const mp4_track_t *track;
	SceUID   fd;                       /* separate from the video reader */
	char     path[256];
	int      sample_rate;

	unsigned long *codec;              /* 65-word sceAudiocodec control block */
	void    *codec_work;
	uint8_t *in_buf;

	audio_block_t *ring;
	volatile int write_idx;            /* advanced by the decoding thread */
	volatile int read_idx;             /* advanced by the output thread */
	int      next_sample;              /* next AAC sample to decode */
	volatile bool decode_done;         /* no more samples */

	SceUID   thread;
	volatile bool playing;
	volatile bool quit;
	volatile bool flush;               /* output thread: drop the ring */
	volatile bool flushed;
	volatile bool started;             /* first block has been submitted */
	volatile uint32_t clock_pts;       /* pts of the block being played */
	int      errors;
} audio_player_t;

int  audio_open(audio_player_t *ap, const mp4_track_t *track, const char *path);
void audio_close(audio_player_t *ap);

/*
 * Decodes into the ring until it is full, or until the next sample lies past
 * byte `avail` of the file (still downloading). Call from the main loop.
 */
void audio_fill(audio_player_t *ap, long avail);

/* File offset just past the next sample to decode; 0 when none is left. */
long audio_next_end(const audio_player_t *ap);

void audio_set_playing(audio_player_t *ap, bool playing);

/* Drops buffered audio and restarts decoding at `seconds`. */
void audio_seek(audio_player_t *ap, double seconds);

/* Playback position in seconds; false until audio has started. */
bool audio_clock(const audio_player_t *ap, double *seconds);

/* True once every sample has been played out. */
bool audio_finished(const audio_player_t *ap);

#endif
