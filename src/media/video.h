#ifndef YT_MEDIA_VIDEO_H
#define YT_MEDIA_VIDEO_H

#include "../common.h"
#include "mp4demux.h"

/*
 * H.264 decoding on the PSP's Media Engine, the way PMPlayer Advance does it
 * on firmware 5.00 and later (cooleyes' mp4avcdecoder.c), verified on a
 * 6.61 PRO console:
 *
 *   avcodec.prx       sceUtilityLoadAvModule(AVCODEC); also exports sceMpegbase
 *   mpeg_vsh.prx      the XMB's sceMpeg, which has the MP4 modes; loaded from
 *                     flash0 through the CFW's KUBridge
 *   cooleyesBridge    a small kernel module, built from third_party/, that
 *                     calls sceMeBootStart to put the ME in video mode
 *
 * Nothing Sony-signed is shipped: mpeg_vsh.prx is read from the console's own
 * flash, and the bridge is built from source.
 *
 * Frames come out as RGBA8888 in `rgb`, `pitch` pixels per row. The decoder
 * pads the height to a multiple of 16 (360 -> 368); `width`/`height` are the
 * stream's real size.
 */
typedef struct {
	bool      open;
	int       mode;          /* 4: up to 480x272, 5: larger */
	int       width;
	int       height;
	int       pitch;         /* 512, or 768 for streams wider than 480 */

	int       mpeg[1];       /* SceMpeg handle */
	int       ringbuffer[11];/* zeroed SceMpegRingbuffer; unused for NAL input */
	void     *mpeg_buf;
	int       mpeg_size;
	bool      created;       /* sceMpegCreate succeeded */
	int      *au;            /* 64-byte SceMpegAu */
	uint8_t  *sps_pps;       /* SPS followed by PPS, as GetAvcNalAu wants */
	int       sps_size;
	int       pps_size;
	int       nal_prefix_size;
	bool      need_first;    /* next sample starts a new sequence */

	uint32_t *rgb;           /* pitch * 368 RGBA pixels */
	int       frames;
} video_decoder_t;

/*
 * Loads the decoder modules once. `bridge_path` is cooleyesBridge.prx next to
 * the EBOOT. Returns 0 when hardware decoding is available.
 */
int  video_system_init(const char *bridge_path);
bool video_system_ready(void);

int  video_open(video_decoder_t *vd, const mp4_track_t *track);
void video_close(video_decoder_t *vd);

/*
 * Decodes one MP4 sample (length-prefixed NAL units, as stored in the file).
 * The buffer must be 64-byte aligned. Returns the number of pictures that
 * came out (the newest is in vd->rgb), 0 while the decoder fills its reorder
 * queue, or a negative error.
 */
int  video_decode(video_decoder_t *vd, void *sample, int size);

/*
 * Starts a fresh decoder context for a discontinuity (a seek, or recovery
 * from an error); the next sample must be a keyframe. The output buffer and
 * the picture in it are kept. Returns 0 on success.
 */
int  video_reset(video_decoder_t *vd);

#endif
