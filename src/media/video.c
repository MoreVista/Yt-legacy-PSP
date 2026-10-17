/*
 * Hardware H.264 decoding through sceMpeg's MP4 path. See video.h for the
 * module arrangement; the call sequence below mirrors PMPlayer Advance's
 * mp4avcdecoder.c, which is the only known-good reference for these
 * undocumented calls. Each departure from it cost a device round trip:
 * the plain sceMpeg modes, MallocAvcEsBuf as the ES buffer and a decode
 * straight to RGB all fail with 0x80628001 on MP4 input.
 */
#include "video.h"
#include "../util/log.h"
#include <malloc.h>
#include <psputility.h>
#include <psputils.h>
#include <pspmodulemgr.h>

/* Imported in compat/av_imports.S. */
int  sceMpegInit(void);
void sceMpegFinish(void);
int  sceMpegQueryMemSize(int mode);
int  sceMpegCreate(void *mpeg, void *buf, int size, void *ringbuffer,
                   int frame_width, int mode, int ddrtop);
void sceMpegDelete(void *mpeg);
int  sceMpegInitAu(void *mpeg, void *es_buffer, void *au);
int  sceMpegGetAvcNalAu(void *mpeg, void *nal, void *au);
int  sceMpegAvcDecode(void *mpeg, void *au, int frame_width, void *out,
                      int *pics);
int  sceMpegAvcDecodeDetail2(void *mpeg, void *detail2);
int  sceMpegBaseCscAvc(void *dest, int unknown, int width, void *csc);
SceUID kuKernelLoadModule(const char *path, int flags,
                          SceKernelLMOption *option);
unsigned int cooleyesBridgeGetVersion(void);
int  cooleyesMeBootStart(int devkit, int type);

typedef struct {
	void *sps_buffer; int sps_size;
	void *pps_buffer; int pps_size;
	int   nal_prefix_size;
	void *nal_buffer; int nal_size;
	int   mode;               /* 3 starts a sequence, 0 continues it */
} avc_nal_t;

typedef struct {
	int unknown0, unknown1, width, height;
	int unknown4, unknown5, unknown6, unknown7, unknown8, unknown9;
} avc_info_t;

typedef struct {
	void *buffer[8];
	int   unknown[3];
} avc_yuv_t;

typedef struct {
	int         unknown0[4];
	avc_info_t *info;
	int         unknown5[6];
	avc_yuv_t  *yuv;
	int         unknown12[12];
} avc_detail2_t;

typedef struct {
	int   height_mb, width_mb;   /* in 16-pixel macroblocks */
	int   mode0, mode1;
	void *buffer[8];
} avc_csc_t;

#define DECODED_HEIGHT_MAX 368   /* 360 padded to a macroblock */

static bool     g_ready;
static uint8_t *g_ddrtop;        /* 4 MB-aligned work area, kept for life */
static int      g_me_boot = -1;  /* ME booter type currently loaded */

static int load_start(const char *path, bool kernel)
{
	SceUID uid = kernel ? kuKernelLoadModule(path, 0, NULL)
	                    : sceKernelLoadModule(path, 0, NULL);
	if (uid < 0) {
		log_printf("[video] load %s failed 0x%08x", path, uid);
		return uid;
	}
	int status = 0;
	int r = sceKernelStartModule(uid, 0, NULL, &status, NULL);
	if (r < 0) {
		log_printf("[video] start %s failed 0x%08x", path, r);
		return r;
	}
	return 0;
}

int video_system_init(const char *bridge_path)
{
	if (g_ready)
		return 0;

	int devkit = sceKernelDevkitVersion();
	if (devkit < 0x05000000) {
		log_printf("[video] firmware 0x%08x: mpeg_vsh path needs 5.00+", devkit);
		return -1;
	}

	/*
	 * The work area must be 4 MB aligned. Take it first, before the heap
	 * fragments; it is reused by every video.
	 */
	g_ddrtop = memalign(0x400000, 0x200000);
	if (!g_ddrtop) {
		log_printf("[video] no memory for the ME work area");
		return -1;
	}

	int r = sceUtilityLoadAvModule(PSP_AV_MODULE_AVCODEC);
	if (r < 0) {
		log_printf("[video] LoadAvModule(AVCODEC) failed 0x%08x", r);
		return r;
	}
	if ((r = load_start("flash0:/kd/mpeg_vsh.prx", true)) < 0)
		return r;
	if ((r = load_start(bridge_path, true)) < 0)
		return r;

	log_printf("[video] hardware decoder ready (bridge 0x%08x)",
	           cooleyesBridgeGetVersion());
	g_ready = true;
	return 0;
}

bool video_system_ready(void)
{
	return g_ready;
}

/* sceMpegCreate + InitAu on the decoder's buffers. */
static int create_context(video_decoder_t *vd)
{
	memset(vd->mpeg, 0, sizeof(vd->mpeg));
	memset(vd->ringbuffer, 0, sizeof(vd->ringbuffer));
	int r = sceMpegCreate(vd->mpeg, vd->mpeg_buf, vd->mpeg_size,
	                      vd->ringbuffer, 512, vd->mode, (int)g_ddrtop);
	if (r != 0) {
		log_printf("[video] sceMpegCreate(mode %d) failed 0x%08x", vd->mode, r);
		return -1;
	}
	vd->created = true;

	memset(vd->au, 0xFF, 64);
	r = sceMpegInitAu(vd->mpeg, g_ddrtop + 0x10000, vd->au);
	if (r != 0) {
		log_printf("[video] sceMpegInitAu failed 0x%08x", r);
		return -1;
	}
	vd->need_first = true;
	return 0;
}

int video_open(video_decoder_t *vd, const mp4_track_t *track)
{
	memset(vd, 0, sizeof(*vd));
	if (!g_ready)
		return -1;
	if (track->sps_size <= 0 || track->pps_size <= 0) {
		log_printf("[video] no SPS/PPS in the file");
		return -1;
	}

	vd->width = (int)track->video_width;
	vd->height = (int)track->video_height;
	if (vd->width <= 0 || vd->width > 768 || vd->height <= 0 ||
	    vd->height > DECODED_HEIGHT_MAX) {
		log_printf("[video] %dx%d is outside what the ME decodes",
		           vd->width, vd->height);
		return -1;
	}
	bool big = vd->width > 480 || vd->height > 272;
	vd->mode = big ? 5 : 4;
	vd->pitch = vd->width > 480 ? 768 : 512;

	/* ME booter: 1 for the large mode, else 4 for Baseline, 3 for Main. */
	int boot = big ? 1 : (track->profile_idc == 0x42 ? 4 : 3);
	if (boot != g_me_boot) {
		int r = cooleyesMeBootStart(0, boot);
		if (r != 0) {
			log_printf("[video] MeBootStart(%d) failed 0x%08x", boot, r);
			g_me_boot = -1;
			return -1;
		}
		g_me_boot = boot;
	}

	int r = sceMpegInit();
	if (r != 0) {
		log_printf("[video] sceMpegInit failed 0x%08x", r);
		return -1;
	}
	vd->open = true;           /* from here on video_close undoes the init */

	int size = sceMpegQueryMemSize(vd->mode);
	vd->mpeg_buf = (size > 0) ? memalign(64, size) : NULL;
	vd->au = memalign(64, 64);
	vd->sps_size = track->sps_size;
	vd->pps_size = track->pps_size;
	vd->sps_pps = memalign(64, vd->sps_size + vd->pps_size);
	vd->rgb = memalign(64, vd->pitch * DECODED_HEIGHT_MAX * 4);
	if (!vd->mpeg_buf || !vd->au || !vd->sps_pps || !vd->rgb) {
		log_printf("[video] out of memory (mpeg %d bytes)", size);
		video_close(vd);
		return -1;
	}
	memcpy(vd->sps_pps, track->sps, vd->sps_size);
	memcpy(vd->sps_pps + vd->sps_size, track->pps, vd->pps_size);
	vd->nal_prefix_size = track->nal_length_size;

	/* The ME writes the picture by DMA: no dirty CPU lines may cover it. */
	memset(vd->rgb, 0, vd->pitch * DECODED_HEIGHT_MAX * 4);
	sceKernelDcacheWritebackInvalidateAll();

	vd->mpeg_size = size;
	if (create_context(vd) != 0) {
		video_close(vd);
		return -1;
	}
	log_printf("[video] open %dx%d mode %d pitch %d", vd->width, vd->height,
	           vd->mode, vd->pitch);
	return 0;
}

void video_close(video_decoder_t *vd)
{
	if (vd->open) {
		if (vd->created)
			sceMpegDelete(vd->mpeg);
		sceMpegFinish();
	}
	free(vd->mpeg_buf);
	free(vd->au);
	free(vd->sps_pps);
	free(vd->rgb);
	memset(vd, 0, sizeof(*vd));
}

/*
 * Sony's AVC context keeps reference pictures and CABAC state that a new
 * keyframe does not clear: seeking with the same context left later frames
 * predicting from pre-seek pictures, and playback jumped at every GOP.
 * PMPlayer Advance tears the context down on every discontinuity; so does
 * this.
 */
int video_reset(video_decoder_t *vd)
{
	if (!vd->open)
		return -1;
	if (vd->created) {
		sceMpegDelete(vd->mpeg);
		vd->created = false;
	}
	sceMpegFinish();
	vd->open = false;

	int r = sceMpegInit();
	if (r != 0) {
		log_printf("[video] reset: sceMpegInit failed 0x%08x", r);
		return -1;
	}
	vd->open = true;
	return create_context(vd);
}

int video_decode(video_decoder_t *vd, void *sample, int size)
{
	if (!vd->open || !sample || size <= 0)
		return -1;

	avc_nal_t nal = {
		vd->sps_pps, vd->sps_size,
		vd->sps_pps + vd->sps_size, vd->pps_size,
		vd->nal_prefix_size,
		sample, size,
		vd->need_first ? 3 : 0,
	};
	/* The ME reads the sample from RAM, behind the data cache. */
	sceKernelDcacheWritebackRange(sample, size);

	int r = sceMpegGetAvcNalAu(vd->mpeg, &nal, vd->au);
	if (r != 0)
		return r;
	vd->need_first = false;

	int pics = 0;
	r = sceMpegAvcDecode(vd->mpeg, vd->au, 512, NULL, &pics);
	if (r != 0)
		return r;

	avc_detail2_t *detail = NULL;
	r = sceMpegAvcDecodeDetail2(vd->mpeg, &detail);
	if (r != 0)
		return r;
	if (pics <= 0)
		return 0;
	if (!detail || !detail->info || !detail->yuv)
		return -1;

	/*
	 * Entry 0 is the picture due first, as in the reference's
	 * mp4_avc_get. When more than one comes out at once, the rest are
	 * dropped; at 30 fps that is a skipped frame, not a stall.
	 */
	avc_info_t *info = detail->info;
	avc_yuv_t *yuv = detail->yuv;
	avc_csc_t csc = {
		(info->height + 15) >> 4, (info->width + 15) >> 4, 0, 0,
		{ yuv->buffer[0], yuv->buffer[1], yuv->buffer[2], yuv->buffer[3],
		  yuv->buffer[4], yuv->buffer[5], yuv->buffer[6], yuv->buffer[7] },
	};
	r = sceMpegBaseCscAvc(vd->rgb, 0, vd->pitch, &csc);
	if (r != 0)
		return r;

	vd->frames++;
	if (vd->frames == 1) {
		/* One-off check of what the conversion wrote, read uncached. */
		const uint32_t *px = (const uint32_t *)(0x40000000u | (uint32_t)vd->rgb);
		int mid = (vd->height / 2) * vd->pitch + vd->width / 2;
		log_printf("[video] first picture %dx%d: px[0]=%08x px[mid]=%08x",
		           info->width, info->height, px[0], px[mid]);
	}
	return pics;
}
