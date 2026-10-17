#include "audio.h"
#include "../util/log.h"
#include <malloc.h>
#include <pspaudio.h>
#include <pspaudiocodec.h>
#include <pspiofilemgr.h>
#include <pspthreadman.h>
#include <psputils.h>

#define IN_BUF_SIZE   8192             /* one AAC sample is ~400-800 bytes */
#define PREFILL       4                /* blocks buffered before output starts */

static int ring_used(const audio_player_t *ap)
{
	return (ap->write_idx - ap->read_idx + AUDIO_RING) % AUDIO_RING;
}

static int output_thread(SceSize args, void *argp)
{
	audio_player_t *ap = *(audio_player_t **)argp;
	(void)args;

	while (!ap->quit) {
		if (ap->flush) {
			ap->read_idx = ap->write_idx;
			ap->started = false;
			ap->flush = false;
			ap->flushed = true;
			continue;
		}
		int used = ring_used(ap);
		/* Ran dry mid-stream (the download fell behind): prefill again
		 * before resuming, rather than playing block by block. */
		if (used == 0 && ap->started && !ap->decode_done)
			ap->started = false;
		bool can_start = ap->started || used >= PREFILL ||
		                 (ap->decode_done && used > 0);
		if (!ap->playing || used == 0 || !can_start) {
			sceKernelDelayThread(5000);
			continue;
		}

		audio_block_t *b = &ap->ring[ap->read_idx];
		ap->clock_pts = b->pts;
		ap->started = true;
		sceAudioSRCOutputBlocking(PSP_AUDIO_VOLUME_MAX, b->pcm);
		ap->read_idx = (ap->read_idx + 1) % AUDIO_RING;
	}
	return 0;
}

/*
 * The SRC channel cannot be released while its last block is still playing:
 * the call fails and the channel stays reserved, and the next video's
 * sceAudioSRCChReserve then fails with 0x80268002 -- only the first video
 * after boot had sound. Retry until the hardware has drained.
 */
static int release_src(void)
{
	int r = -1;
	for (int i = 0; i < 50; i++) {
		r = sceAudioSRCChRelease();
		if (r >= 0)
			return r;
		sceKernelDelayThread(10000);
	}
	log_printf("[audio] SRCChRelease failed 0x%08x", r);
	return r;
}

int audio_open(audio_player_t *ap, const mp4_track_t *track, const char *path)
{
	memset(ap, 0, sizeof(*ap));
	ap->fd = -1;
	ap->thread = -1;
	ap->track = track;
	if (!track->has_audio || track->audio_count == 0 ||
	    track->audio_timescale == 0)
		return -1;
	ap->sample_rate = track->audio_sample_rate;
	snprintf(ap->path, sizeof(ap->path), "%s", path);

	ap->codec = memalign(64, 65 * sizeof(unsigned long));
	ap->in_buf = memalign(64, IN_BUF_SIZE);
	ap->ring = memalign(64, AUDIO_RING * sizeof(audio_block_t));
	ap->fd = sceIoOpen(path, PSP_O_RDONLY, 0);
	if (!ap->codec || !ap->in_buf || !ap->ring || ap->fd < 0) {
		log_printf("[audio] setup failed");
		audio_close(ap);
		return -1;
	}
	memset(ap->codec, 0, 65 * sizeof(unsigned long));

	int r = sceAudiocodecCheckNeedMem(ap->codec, PSP_CODEC_AAC);
	if (r < 0) {
		log_printf("[audio] CheckNeedMem failed 0x%08x", r);
		audio_close(ap);
		return -1;
	}
	/* PPSSPP reports success without a size; real firmware always sets it. */
	if (ap->codec[4] == 0)
		ap->codec[4] = 0x10000;
	ap->codec_work = memalign(64, ap->codec[4]);
	if (!ap->codec_work) {
		audio_close(ap);
		return -1;
	}
	ap->codec[3] = (unsigned long)ap->codec_work;
	ap->codec[10] = (unsigned long)ap->sample_rate;   /* AAC: output rate */
	r = sceAudiocodecInit(ap->codec, PSP_CODEC_AAC);
	if (r < 0) {
		log_printf("[audio] Init failed 0x%08x", r);
		audio_close(ap);
		return -1;
	}

	r = sceAudioSRCChReserve(AUDIO_FRAMES, ap->sample_rate, 2);
	if (r < 0) {
		/* A channel left reserved by an earlier session: free it and retry. */
		release_src();
		r = sceAudioSRCChReserve(AUDIO_FRAMES, ap->sample_rate, 2);
	}
	if (r < 0) {
		log_printf("[audio] SRCChReserve(%d Hz) failed 0x%08x",
		           ap->sample_rate, r);
		audio_close(ap);
		return -1;
	}
	ap->open = true;

	/* Above the main thread (32), so output never waits on drawing. */
	ap->thread = sceKernelCreateThread("audio_out", output_thread, 16,
	                                   0x4000, PSP_THREAD_ATTR_USER, NULL);
	if (ap->thread < 0) {
		log_printf("[audio] thread failed 0x%08x", ap->thread);
		audio_close(ap);
		return -1;
	}
	audio_player_t *self = ap;
	sceKernelStartThread(ap->thread, sizeof(self), &self);

	log_printf("[audio] AAC %d Hz %d ch, %d samples, work %lu bytes",
	           ap->sample_rate, track->audio_channels, track->audio_count,
	           ap->codec[4]);
	return 0;
}

void audio_close(audio_player_t *ap)
{
	if (ap->thread >= 0) {
		ap->quit = true;
		sceKernelWaitThreadEnd(ap->thread, NULL);
		sceKernelDeleteThread(ap->thread);
	}
	if (ap->open)
		release_src();
	if (ap->fd >= 0)
		sceIoClose(ap->fd);
	free(ap->codec_work);
	free(ap->codec);
	free(ap->in_buf);
	free(ap->ring);
	memset(ap, 0, sizeof(*ap));
	ap->fd = -1;
	ap->thread = -1;
}

static bool decode_one(audio_player_t *ap, audio_block_t *b)
{
	const mp4_sample_t *s = &ap->track->audio_samples[ap->next_sample++];
	b->pts = s->time_offset;
	if (s->size == 0 || s->size > IN_BUF_SIZE)
		goto silence;
	int n = mp4_read_at(&ap->fd, ap->path, s->offset, ap->in_buf, s->size);
	if (n != (int)s->size) {
		if (++ap->errors <= 5)
			log_printf("[audio] sample %d read %d of %u", ap->next_sample - 1,
			           n, s->size);
		goto silence;
	}

	sceKernelDcacheWritebackRange(ap->in_buf, s->size);
	ap->codec[6] = (unsigned long)ap->in_buf;
	ap->codec[7] = s->size;
	ap->codec[8] = (unsigned long)b->pcm;
	ap->codec[9] = sizeof(b->pcm);
	sceKernelDcacheWritebackInvalidateRange(b->pcm, sizeof(b->pcm));
	int r = sceAudiocodecDecode(ap->codec, PSP_CODEC_AAC);
	if (r >= 0)
		return true;
	if (++ap->errors <= 5)
		log_printf("[audio] sample %d decode 0x%08x", ap->next_sample - 1, r);
silence:
	memset(b->pcm, 0, sizeof(b->pcm));
	return false;
}

long audio_next_end(const audio_player_t *ap)
{
	if (!ap->open || ap->next_sample >= ap->track->audio_count)
		return 0;
	const mp4_sample_t *s = &ap->track->audio_samples[ap->next_sample];
	return (long)(s->offset + s->size);
}

void audio_fill(audio_player_t *ap, long avail)
{
	if (!ap->open || ap->flush)
		return;
	/* One slot stays empty so full and empty are distinguishable. */
	while (ring_used(ap) < AUDIO_RING - 1) {
		if (ap->next_sample >= ap->track->audio_count) {
			ap->decode_done = true;
			return;
		}
		if (audio_next_end(ap) > avail)
			return;                      /* not downloaded yet */
		decode_one(ap, &ap->ring[ap->write_idx]);
		ap->write_idx = (ap->write_idx + 1) % AUDIO_RING;
	}
}

void audio_set_playing(audio_player_t *ap, bool playing)
{
	ap->playing = playing;
}

void audio_seek(audio_player_t *ap, double seconds)
{
	if (!ap->open)
		return;

	/* Have the output thread drop what it holds, then refill from there. */
	ap->flushed = false;
	ap->flush = true;
	while (!ap->flushed)
		sceKernelDelayThread(1000);

	uint32_t target = (uint32_t)(seconds * ap->track->audio_timescale);
	int i = 0;
	while (i + 1 < ap->track->audio_count &&
	       ap->track->audio_samples[i + 1].time_offset <= target)
		i++;
	ap->next_sample = i;
	ap->decode_done = false;
	ap->clock_pts = ap->track->audio_samples[i].time_offset;
}

bool audio_clock(const audio_player_t *ap, double *seconds)
{
	if (!ap->open)
		return false;
	*seconds = (double)ap->clock_pts / ap->track->audio_timescale;
	return true;
}

bool audio_finished(const audio_player_t *ap)
{
	return !ap->open || (ap->decode_done && ring_used(ap) == 0);
}
