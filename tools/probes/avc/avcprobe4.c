/*
 * Hardware H.264 probe v4: the decode path of PMPlayer Advance's
 * mp4avcdecoder.c (cooleyes, GPL), step for step.
 *
 *  1. sceUtilityLoadAvModule(AVCODEC)          -- also provides sceMpegbase
 *  2. kuKernelLoadModule(flash0:/kd/mpeg_vsh.prx) -- the VSH sceMpeg, which
 *     has the MP4 modes (4: <=480x272, 5: larger)
 *  3. cooleyesBridge.prx (built from source)    -- sceMeBootStart in kernel
 *     mode; ME boot type 1 for mode 5
 *  4. sceMpegCreate(..., 512, mode, ddrtop), InitAu on ddrtop + 0x10000
 *  5. per sample: GetAvcNalAu (mode 3 first, then 0), AvcDecode with no
 *     output, DecodeDetail2 for the YUV planes, BaseCscAvc into VRAM
 *
 * Writes ms0:/PSP/GAME/YTPROBE/frame.bmp for the first decoded picture.
 */
#include <pspkernel.h>
#include <pspiofilemgr.h>
#include <psputility.h>
#include <psputils.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <malloc.h>
#include "media/mp4demux.h"

PSP_MODULE_INFO("YtAvcProbe", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(PSP_THREAD_ATTR_USER);
PSP_HEAP_SIZE_KB(16384);

#define DIR "ms0:/PSP/GAME/YTPROBE/"
#define MP4 "ms0:/PSP/GAME/YTPSP/egqGyZr3O3w.mp4"

void log_printf(const char *fmt, ...)
{
	char line[256];
	va_list ap; va_start(ap, fmt); vsnprintf(line, sizeof line, fmt, ap); va_end(ap);
	SceUID fd = sceIoOpen(DIR "probe.log", PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0777);
	if (fd >= 0) { sceIoWrite(fd, line, strlen(line)); sceIoWrite(fd, "\n", 1); sceIoClose(fd); }
}

typedef int SceMpeg;
typedef struct { int iPtsMSB, iPts, iDtsMSB, iDts, iEsBuffer, iAuSize; } SceMpegAu;
typedef struct { int w[11]; } SceMpegRingbuffer;
typedef struct {
	void *sps_buffer; int sps_size;
	void *pps_buffer; int pps_size;
	int nal_prefix_size;
	void *nal_buffer; int nal_size;
	int mode;
} Mp4AvcNalStruct;
typedef struct { int unknown0, unknown1, width, height, u4, u5, u6, u7, u8, u9; } Mp4AvcInfoStruct;
typedef struct { void *buffer0, *buffer1, *buffer2, *buffer3, *buffer4, *buffer5,
                 *buffer6, *buffer7; int u0, u1, u2; } Mp4AvcYuvStruct;
typedef struct { int u0, u1, u2, u3; Mp4AvcInfoStruct *info_buffer;
                 int u5, u6, u7, u8, u9, u10; Mp4AvcYuvStruct *yuv_buffer;
                 int u12[12]; } Mp4AvcDetail2Struct;
typedef struct { int height, width, mode0, mode1;
                 void *buffer0, *buffer1, *buffer2, *buffer3, *buffer4, *buffer5,
                      *buffer6, *buffer7; } Mp4AvcCscStruct;

int sceMpegInit(void);
void sceMpegFinish(void);
int sceMpegQueryMemSize(int mode);
int sceMpegCreate(SceMpeg *, void *, int, SceMpegRingbuffer *, int, int, int);
void sceMpegDelete(SceMpeg *);
int sceMpegInitAu(SceMpeg *, void *, SceMpegAu *);
int sceMpegGetAvcNalAu(SceMpeg *, Mp4AvcNalStruct *, SceMpegAu *);
int sceMpegAvcDecode(SceMpeg *, SceMpegAu *, int, void *, int *);
int sceMpegAvcDecodeDetail2(SceMpeg *, Mp4AvcDetail2Struct **);
int sceMpegBaseCscAvc(void *dest, int unknown, int width, Mp4AvcCscStruct *csc);
SceUID kuKernelLoadModule(const char *path, int flags, SceKernelLMOption *option);
unsigned int cooleyesBridgeGetVersion(void);
int cooleyesMeBootStart(int devkit, int type);

static void dump_bmp(const unsigned int *px, int w, int h, int pitch)
{
	SceUID fd = sceIoOpen(DIR "frame.bmp", PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
	if (fd < 0) return;
	unsigned char hdr[54] = { 'B', 'M' };
	unsigned int img = w * h * 4;
	unsigned int f[] = { 54 + img, 0, 54, 40, w, (unsigned)-h, 1 | (32 << 16), 0, img, 2835, 2835, 0, 0 };
	memcpy(hdr + 2, f, sizeof f);
	sceIoWrite(fd, hdr, 54);
	static unsigned int row[1024];
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			unsigned int c = px[y * pitch + x];
			row[x] = (c & 0xFF00FF00u) | ((c & 0xFF) << 16) | ((c >> 16) & 0xFF);
		}
		sceIoWrite(fd, row, w * 4);
	}
	sceIoClose(fd);
}

static SceUID load_start(const char *path, int kernel)
{
	SceUID uid = kernel ? kuKernelLoadModule(path, 0, NULL)
	                    : sceKernelLoadModule(path, 0, NULL);
	log_printf("[probe] load %s = 0x%08x", path, uid);
	if (uid < 0)
		return uid;
	int status = 0;
	int r = sceKernelStartModule(uid, 0, NULL, &status, NULL);
	log_printf("[probe] start %s = 0x%08x status 0x%08x", path, r, status);
	return r < 0 ? r : uid;
}

int main(void)
{
	sceIoMkdir(DIR, 0777);
	SceUID t = sceIoOpen(DIR "probe.log", PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
	if (t >= 0) sceIoClose(t);
	log_printf("[probe] start v4, devkit 0x%08x", sceKernelDevkitVersion());

	int r;
	log_printf("[probe] LoadAvModule AVCODEC = 0x%08x",
	           sceUtilityLoadAvModule(PSP_AV_MODULE_AVCODEC));
	if (load_start("flash0:/kd/mpeg_vsh.prx", 1) < 0) goto out;
	if (load_start(DIR "cooleyesBridge.prx", 1) < 0) goto out;
	log_printf("[probe] bridge version 0x%08x", cooleyesBridgeGetVersion());

	static mp4_track_t tr;
	r = mp4_open(MP4, &tr);
	log_printf("[probe] mp4_open = %d %ux%u profile 0x%02x nal_len=%d sps=%d pps=%d",
	           r, tr.video_width, tr.video_height, tr.profile_idc,
	           tr.nal_length_size, tr.sps_size, tr.pps_size);
	if (r != 0) goto out;

	int big = tr.video_width > 480 || tr.video_height > 272;
	int mode = big ? 5 : 4;
	int boot = big ? 1 : (tr.profile_idc == 0x42 ? 4 : 3);
	r = cooleyesMeBootStart(0, boot);
	log_printf("[probe] MeBootStart(type %d) = 0x%08x", boot, r);
	if (r != 0) goto out;

	r = sceMpegInit();
	log_printf("[probe] sceMpegInit = 0x%08x", r);
	if (r != 0) goto out;
	int size = sceMpegQueryMemSize(mode);
	log_printf("[probe] QueryMemSize(%d) = %d", mode, size);
	if (size <= 0) goto out;
	void *mpeg_buf = memalign(64, size);
	unsigned char *ddrtop = memalign(0x400000, 0x200000);
	log_printf("[probe] mpeg_buf %p ddrtop %p", mpeg_buf, ddrtop);
	if (!mpeg_buf || !ddrtop) goto out;
	unsigned char *au_buffer = ddrtop + 0x10000;

	static SceMpeg mpeg;
	static SceMpegRingbuffer rb;          /* zeroed, as in the reference */
	r = sceMpegCreate(&mpeg, mpeg_buf, size, &rb, 512, mode, (int)ddrtop);
	log_printf("[probe] sceMpegCreate(mode %d) = 0x%08x", mode, r);
	if (r != 0) goto out;

	SceMpegAu *au = memalign(64, 64);
	memset(au, 0xFF, 64);
	r = sceMpegInitAu(&mpeg, au_buffer, au);
	log_printf("[probe] InitAu = 0x%08x", r);
	if (r != 0) goto del;

	unsigned char *sps_pps = memalign(64, tr.sps_size + tr.pps_size);
	memcpy(sps_pps, tr.sps, tr.sps_size);
	memcpy(sps_pps + tr.sps_size, tr.pps, tr.pps_size);

	unsigned char *buf = memalign(64, 512 * 1024);
	void *dest = (void *)0x04000000;                   /* VRAM */
	const unsigned int *vram = (const unsigned int *)0x44000000;
	int csc_width = tr.video_width > 480 ? 768 : 512;

	for (int i = 0; i < tr.video_count && i < 30; i++) {
		int n = mp4_read_sample(MP4, &tr.video_samples[i], buf, 512 * 1024);
		if (n <= 0) break;
		Mp4AvcNalStruct nal = {
			sps_pps, tr.sps_size, sps_pps + tr.sps_size, tr.pps_size,
			tr.nal_length_size, buf, n, (i == 0) ? 3 : 0,
		};
		sceKernelDcacheWritebackInvalidateAll();
		int g = sceMpegGetAvcNalAu(&mpeg, &nal, au);
		if (g != 0) {
			log_printf("[probe] sample %d: GetAvcNalAu 0x%08x", i, g);
			break;
		}
		int pics = 0;
		int d = sceMpegAvcDecode(&mpeg, au, 512, 0, &pics);
		Mp4AvcDetail2Struct *det = NULL;
		int dd = (d == 0) ? sceMpegAvcDecodeDetail2(&mpeg, &det) : d;
		log_printf("[probe] sample %d size %d: au %d Decode 0x%08x pics %d Detail2 0x%08x",
		           i, n, au->iAuSize, d, pics, dd);
		if (d != 0 || dd != 0)
			break;                           /* never feed a failed decoder */
		if (pics > 0 && det && det->info_buffer && det->yuv_buffer) {
			Mp4AvcInfoStruct *info = det->info_buffer;
			Mp4AvcYuvStruct *yuv = det->yuv_buffer;
			Mp4AvcCscStruct csc = {
				(info->height + 15) >> 4, (info->width + 15) >> 4, 0, 0,
				yuv->buffer0, yuv->buffer1, yuv->buffer2, yuv->buffer3,
				yuv->buffer4, yuv->buffer5, yuv->buffer6, yuv->buffer7,
			};
			log_printf("[probe] picture %dx%d, csc pitch %d", info->width,
			           info->height, csc_width);
			int c = sceMpegBaseCscAvc(dest, 0, csc_width, &csc);
			log_printf("[probe] BaseCscAvc = 0x%08x", c);
			if (c == 0) {
				sceKernelDcacheWritebackInvalidateAll();
				dump_bmp(vram, info->width, info->height, csc_width);
				log_printf("[probe] frame.bmp written");
			}
			break;
		}
	}
del:
	sceMpegDelete(&mpeg);
	sceMpegFinish();
	log_printf("[probe] deleted");
out:
	log_printf("[probe] done");
	sceKernelDelayThread(500000);
	sceKernelExitGame();
	return 0;
}
