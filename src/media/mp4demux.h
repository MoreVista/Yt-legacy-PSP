#ifndef YT_MEDIA_MP4DEMUX_H
#define YT_MEDIA_MP4DEMUX_H

#include "../common.h"

/*
 * Just enough MP4 to pull the H.264 and AAC elementary streams out of a
 * progressive itag 18 file. The file is read from the Memory Stick in place
 * rather than loaded whole, because a 10 minute 360p stream is tens of
 * megabytes and the PSP has ~32 MB of heap.
 */

typedef struct {
	uint32_t offset;      /* byte offset of the sample in the file   */
	uint32_t size;        /* sample size in bytes                    */
	uint32_t duration;    /* in timescale units                      */
	uint32_t time_offset; /* in timescale units                      */
	int      is_sync;     /* video keyframe / audio frame boundary  */
} mp4_sample_t;

typedef struct {
	/* video */
	bool     has_video;
	uint32_t video_timescale;
	uint32_t video_width;
	uint32_t video_height;
	mp4_sample_t *video_samples;
	int      video_count;
	int      video_capacity;

	/* audio */
	bool     has_audio;
	uint32_t audio_timescale;
	int      audio_channels;
	int      audio_sample_rate;
	mp4_sample_t *audio_samples;
	int      audio_count;
	int      audio_capacity;

	uint32_t duration;   /* in video timescale units */

	/* Parameter sets from the avcC box, needed to build Annex-B access
	 * units for the Media Engine. */
	uint8_t  sps[64];
	int      sps_size;
	uint8_t  pps[64];
	int      pps_size;
	uint8_t  nal_length_size;  /* 1, 2 or 4 */
	uint8_t  profile_idc;      /* from the SPS, e.g. 0x4d = Main */
	uint8_t  profile_compat;   /* constraint_set flags from the SPS */
	uint8_t  level_idc;
} mp4_track_t;

/* The PSP Media Engine decodes Baseline/CAVLC only. */
#define H264_PROFILE_CONSTRAINED_BASELINE 0x42
#define H264_PROFILE_BASELINE            0x66
#define H264_PROFILE_MAIN                0x4d
#define H264_PROFILE_HIGH                0x64

bool mp4_video_is_baseline(const mp4_track_t *track);

int  mp4_open(const char *path, mp4_track_t *track);
void mp4_close(mp4_track_t *track);

/*
 * For playing a file that is still downloading: the offset where the moov
 * box ends, once the first `avail` bytes contain all of it. Returns 0 while
 * more data is needed, and -1 when mdat comes first, so the file cannot be
 * played before it is complete.
 */
long mp4_header_end(const char *path, long avail);

/*
 * Reads `size` bytes at `offset` through *fd, which stays open between calls.
 * For a file that is still being written: a descriptor only sees the file as
 * long as it was when opened, so a short read reopens it and tries again.
 * Returns the bytes read.
 */
int  mp4_read_at(SceUID *fd, const char *path, uint32_t offset,
                 void *buf, int size);

/* Reads one sample's payload. Returns bytes read, 0 at end of track. */
int  mp4_read_sample(const char *path, const mp4_sample_t *s,
                     void *buf, int buf_size);

#endif
