#include "player.h"
#include "../util/log.h"
#include <malloc.h>
#include <pspiofilemgr.h>
#include <pspthreadman.h>

#define SAMPLE_BUF_SIZE (512 * 1024)

/* How far behind schedule decoding may fall before frames are skipped. */
#define MAX_CATCHUP_SAMPLES 3

/* After running dry, wait for this much more of the file (~3 s at 360p). */
#define BUFFER_MARGIN (256 * 1024)

static uint64_t now_us(void)
{
	return sceKernelGetSystemTimeWide();
}

static uint64_t ticks_to_us(const player_t *p, uint32_t ticks)
{
	return (uint64_t)ticks * 1000000ull / p->track.video_timescale;
}

static int index_for_time(const mp4_track_t *t, uint32_t ticks)
{
	/* The keyframe at or before the target: decoding can only resume there. */
	int best = 0;
	for (int i = 0; i < t->video_count; i++) {
		if (t->video_samples[i].time_offset > ticks)
			break;
		if (t->video_samples[i].is_sync)
			best = i;
	}
	return best;
}

static void fail(player_t *p, const char *msg)
{
	p->state = PLAYER_ERROR;
	snprintf(p->message, sizeof(p->message), "%s", msg);
}

int player_open(player_t *p, const char *path)
{
	memset(p, 0, sizeof(*p));
	p->fd = -1;
	snprintf(p->path, sizeof(p->path), "%s", path);

#ifndef PLAYER_AUDIO_TEST
	if (!video_system_ready()) {
		fail(p, "hardware decoder unavailable");
		return -1;
	}
#endif
	if (mp4_open(path, &p->track) != 0) {
		fail(p, "cannot open file");
		return -1;
	}
	if (!p->track.has_video || p->track.video_count == 0 ||
	    p->track.video_timescale == 0) {
		fail(p, "no video track");
		mp4_close(&p->track);
		return -1;
	}

	p->width = (int)p->track.video_width;
	p->height = (int)p->track.video_height;
	log_printf("[player] %dx%d profile=0x%02x level=%d samples=%d",
	           p->width, p->height, p->track.profile_idc,
	           p->track.level_idc, p->track.video_count);

	p->fd = sceIoOpen(path, PSP_O_RDONLY, 0);
	p->sample_buf = memalign(64, SAMPLE_BUF_SIZE);
	p->sample_buf_size = SAMPLE_BUF_SIZE;
	if (p->fd < 0 || !p->sample_buf) {
		player_close(p);
		fail(p, "cannot read file");
		return -1;
	}

#ifdef PLAYER_AUDIO_TEST
	/* Diagnostic builds: audio only, where the emulator has no decoder. */
	if (video_system_ready() && video_open(&p->decoder, &p->track) != 0) {
#else
	if (video_open(&p->decoder, &p->track) != 0) {
#endif
		player_close(p);
		fail(p, "decoder init failed");
		return -1;
	}
	/* Audio is optional: without it the picture still plays. */
	p->has_audio = audio_open(&p->audio, &p->track, path) == 0;
	if (!p->has_audio)
		log_printf("[player] no audio, video clock only");

	p->state = PLAYER_READY;
	snprintf(p->message, sizeof(p->message), "ready");
	return 0;
}

void player_close(player_t *p)
{
	audio_close(&p->audio);
	video_close(&p->decoder);
	if (p->fd >= 0)
		sceIoClose(p->fd);
	free(p->sample_buf);
	mp4_close(&p->track);
	memset(p, 0, sizeof(*p));
	p->fd = -1;
}

static int read_sample(player_t *p, const mp4_sample_t *s)
{
	if ((int)s->size > p->sample_buf_size)
		return -1;
	return mp4_read_at(&p->fd, p->path, s->offset, p->sample_buf, s->size);
}

/* Decodes the next sample. Returns pictures produced, or <0 on a failure. */
static int decode_next(player_t *p)
{
	const mp4_sample_t *s = &p->track.video_samples[p->current_sample];
	p->current_sample++;
	p->current_time = s->time_offset + s->duration;

#ifdef PLAYER_AUDIO_TEST
	if (!p->decoder.open)
		return 0;
#endif
	int n = read_sample(p, s);
	if (n != (int)s->size) {
		if (++p->frames_failed <= 5)
			log_printf("[player] sample %d read %d of %u at %u",
			           p->current_sample - 1, n, s->size, s->offset);
		return -1;
	}
	int r = video_decode(&p->decoder, p->sample_buf, n);
	if (r < 0) {
		p->frames_failed++;
		if (p->frames_failed <= 5)
			log_printf("[player] sample %d decode 0x%08x",
			           p->current_sample - 1, r);
		/* A failed decoder must not be fed on: new context, resync at a
		 * keyframe. */
		video_reset(&p->decoder);
		while (p->current_sample < p->track.video_count &&
		       !p->track.video_samples[p->current_sample].is_sync)
			p->current_sample++;
		return r;
	}
	if (r > 0) {
		p->frames_decoded++;
		p->have_picture = true;
	}
	return r;
}

/* Where playback is now, in microseconds of media time. */
static uint64_t media_now_us(const player_t *p)
{
	double t;
	if (p->has_audio && audio_clock(&p->audio, &t))
		return (uint64_t)(t * 1000000.0);
	uint64_t now = now_us();
	return now > p->clock_base_us ? now - p->clock_base_us : 0;
}

void player_attach_download(player_t *p, const download_state_t *dl)
{
	p->dl = dl;
}

/* Bytes of the file that may be read. */
static long available(const player_t *p)
{
	if (!p->dl || (p->dl->done && p->dl->result == 0))
		return 0x7FFFFFFF;
	return p->dl->received;
}

/* The furthest byte the next video and audio samples need. */
static long next_needed(const player_t *p)
{
	long need = 0;
	if (p->current_sample < p->track.video_count) {
		const mp4_sample_t *s = &p->track.video_samples[p->current_sample];
		need = (long)(s->offset + s->size);
	}
	if (p->has_audio) {
		long a = audio_next_end(&p->audio);
		if (a > need)
			need = a;
	}
	return need;
}

/*
 * Pauses the clock when playback reaches the end of what has downloaded,
 * and resumes once a margin is in. Returns true while buffering.
 */
static bool buffer_check(player_t *p)
{
	if (!p->dl)
		return false;
	long avail = available(p);
	long need = next_needed(p);
	bool finished = p->dl->done;

	if (p->buffering) {
		if (!finished && avail < need + BUFFER_MARGIN)
			return true;
		p->buffering = false;
		log_printf("[player] buffered, resuming at %.1fs",
		           player_position_seconds(p));
		p->clock_base_us = now_us() - ticks_to_us(p, p->current_time);
		audio_set_playing(&p->audio, true);
		return false;
	}
	if (need > avail && !finished) {
		p->buffering = true;
		audio_set_playing(&p->audio, false);
		log_printf("[player] buffering at %.1fs (%ld of %ld bytes)",
		           player_position_seconds(p), avail, p->dl->expected);
		return true;
	}
	return false;
}

bool player_step(player_t *p)
{
	if (p->state != PLAYER_PLAYING)
		return p->state == PLAYER_PAUSED || p->state == PLAYER_READY;

	if (buffer_check(p))
		return true;
	long avail = available(p);
	if (p->has_audio)
		audio_fill(&p->audio, avail);

	uint64_t media_now = media_now_us(p);
	for (int n = 0; n < MAX_CATCHUP_SAMPLES; n++) {
		if (p->current_sample >= p->track.video_count) {
			if (p->has_audio && !audio_finished(&p->audio))
				return true;             /* let the sound run out */
			p->state = PLAYER_ENDED;
			audio_set_playing(&p->audio, false);
			log_printf("[player] ended: %d decoded, %d failed, %d late",
			           p->frames_decoded, p->frames_failed, p->frames_late);
			return false;
		}
		const mp4_sample_t *next = &p->track.video_samples[p->current_sample];
		if (ticks_to_us(p, next->time_offset) > media_now)
			break;                       /* not due yet */
		if ((long)(next->offset + next->size) > avail)
			break;                       /* not downloaded yet */
		if (n > 0)
			p->frames_late++;
		decode_next(p);
	}

	/* Too far behind to catch up: move the clock instead of racing. The
	 * audio clock cannot be moved, so with sound the video just runs late
	 * until it catches up. */
	if (!p->has_audio && p->current_sample < p->track.video_count) {
		uint64_t now = now_us();
		uint32_t due = p->track.video_samples[p->current_sample].time_offset;
		uint64_t due_us = p->clock_base_us + ticks_to_us(p, due);
		if (now > due_us + 500000)
			p->clock_base_us += now - due_us;
	}
	return true;
}

int player_seek(player_t *p, int seconds)
{
	if (seconds < 0)
		seconds = 0;
	uint32_t ticks = (uint32_t)((double)seconds * p->track.video_timescale);
	int idx = index_for_time(&p->track, ticks);

	p->current_sample = idx;
	p->current_time = p->track.video_samples[idx].time_offset;
	p->clock_base_us = now_us() - ticks_to_us(p, p->current_time);
	if (video_reset(&p->decoder) != 0) {
		fail(p, "decoder reset failed");
		return -1;
	}
	if (p->has_audio)
		audio_seek(&p->audio, player_position_seconds(p));
	if (p->state == PLAYER_ENDED) {
		p->state = PLAYER_PLAYING;
		audio_set_playing(&p->audio, true);
	}
	log_printf("[player] seek to sample %d (%.1fs)", idx,
	           player_position_seconds(p));
	return 0;
}

void player_play(player_t *p)
{
	if (p->state != PLAYER_READY && p->state != PLAYER_PAUSED)
		return;
	/* Resume so that the next sample is due right now. */
	p->clock_base_us = now_us() - ticks_to_us(p, p->current_time);
	p->state = PLAYER_PLAYING;
	/* While buffering, buffer_check restarts the sound once data is in. */
	audio_set_playing(&p->audio, !p->buffering);
}

void player_pause(player_t *p)
{
	if (p->state == PLAYER_PLAYING) {
		p->state = PLAYER_PAUSED;
		audio_set_playing(&p->audio, false);
	}
}

void player_toggle_pause(player_t *p)
{
	if (p->state == PLAYER_PLAYING)
		player_pause(p);
	else
		player_play(p);
}

double player_position_seconds(const player_t *p)
{
	if (p->track.video_timescale == 0)
		return 0.0;
	return (double)p->current_time / p->track.video_timescale;
}

double player_duration_seconds(const player_t *p)
{
	if (p->track.video_timescale == 0)
		return 0.0;
	return (double)p->track.duration / p->track.video_timescale;
}
