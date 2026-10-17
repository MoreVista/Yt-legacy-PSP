/*
 * Hardware H.264 probe. Loads the AV modules the official way, sets up
 * sceMpeg, feeds the first video samples of a downloaded MP4 through
 * sceMpegGetAvcNalAu + sceMpegAvcDecode, and writes the first decoded
 * picture to ms0:/PSP/GAME/YTPROBE/frame.bmp. Every step is logged to
 * probe.log so the last line shows how far it got.
 */
#include <pspkernel.h>
#include <pspiofilemgr.h>
#include <psputility.h>
#include <pspnet_apctl.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <malloc.h>
#include "media/mp4demux.h"

PSP_MODULE_INFO("YtAvcProbe", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(PSP_THREAD_ATTR_USER);
PSP_HEAP_SIZE_KB(16384);

#define DIR      "ms0:/PSP/GAME/YTPROBE/"
#define MP4      "ms0:/PSP/GAME/YTPSP/egqGyZr3O3w.mp4"

void log_printf(const char *fmt, ...)
{
	char line[256];
	va_list ap; va_start(ap, fmt); vsnprintf(line, sizeof line, fmt, ap); va_end(ap);
	SceUID fd = sceIoOpen(DIR "probe.log", PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0777);
	if (fd >= 0) { sceIoWrite(fd, line, strlen(line)); sceIoWrite(fd, "\n", 1); sceIoClose(fd); }
}

typedef int SceMpeg;
typedef struct { int iPtsMSB, iPts, iDtsMSB, iDts, iEsBuffer, iAuSize; } SceMpegAu;
typedef struct { int iPackets, iUnk0, iUnk1, iUnk2, iUnk3; void *pData; void *Callback;
                 void *pCBparam; int iUnk4, iUnk5; SceMpeg pSceMpeg; } SceMpegRingbuffer;
typedef struct { int iUnk0; int iPixelFormat; } SceMpegAvcMode;
/* Layout used by MP4 players on firmware 3.x+ for sceMpegGetAvcNalAu. */
typedef struct {
	void *sps; int sps_size;
	void *pps; int pps_size;
	int nal_prefix_size;
	void *nal; int nal_size;
	int mode;
} AvcNal;

int sceMpegInit(void);
void sceMpegFinish(void);
int sceMpegQueryMemSize(int);
int sceMpegCreate(SceMpeg *, void *, int, SceMpegRingbuffer *, int, int, int);
void sceMpegDelete(SceMpeg *);
int sceMpegRingbufferConstruct(SceMpegRingbuffer *, int, void *, int, void *, void *);
void *sceMpegMallocAvcEsBuf(SceMpeg *);
int sceMpegInitAu(SceMpeg *, void *, SceMpegAu *);
int sceMpegGetAvcNalAu(SceMpeg *, AvcNal *, SceMpegAu *);
int sceMpegAvcDecode(SceMpeg *, SceMpegAu *, int, void *, int *);
int sceMpegAvcDecodeMode(SceMpeg *, SceMpegAvcMode *);


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

int main(void)
{
	sceIoMkdir(DIR, 0777);
	SceUID t = sceIoOpen(DIR "probe.log", PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
	if (t >= 0) sceIoClose(t);
	log_printf("[probe] start v3");

	/* Same import as the app, to test the combination. */
	int st = 0;
	log_printf("[probe] apctl state call -> 0x%08x", sceNetApctlGetState(&st));

	log_printf("[probe] LoadAvModule AVCODEC = 0x%08x", sceUtilityLoadAvModule(PSP_AV_MODULE_AVCODEC));
	log_printf("[probe] LoadAvModule MPEGBASE = 0x%08x", sceUtilityLoadAvModule(PSP_AV_MODULE_MPEGBASE));
	log_printf("[probe] sceMpegInit = 0x%08x", sceMpegInit());

	static SceMpegRingbuffer rb;
	log_printf("[probe] RingbufferConstruct = 0x%08x",
	           sceMpegRingbufferConstruct(&rb, 0, NULL, 0, NULL, NULL));


	int size = sceMpegQueryMemSize(0);
	log_printf("[probe] QueryMemSize = %d", size);
	void *data = memalign(64, size > 0 ? size : 64);

	static mp4_track_t tr;
	int r = mp4_open(MP4, &tr);
	log_printf("[probe] mp4_open = %d video=%d %ux%u nal_len=%d sps=%d pps=%d",
	           r, tr.video_count, tr.video_width, tr.video_height,
	           tr.nal_length_size, tr.sps_size, tr.pps_size);
	if (r != 0) goto out;
	unsigned char *buf = memalign(64, 512 * 1024);

	/*
	 * v3: v1/v2 got 0x80628001 on the IDR with a plain sceMpegCreate, at
	 * every output pitch, and feeding more samples after the error froze the
	 * console. MP4 players built on sceMpegGetAvcNalAu pass a 4 MB-aligned
	 * work area ("ddrtop") as the 7th sceMpegCreate argument, a mode as the
	 * 6th, and place the NAL data inside that area. Try those combinations,
	 * and abandon a variant at its first error.
	 */
	unsigned char *ddrtop = memalign(0x400000, 0x400000);
	log_printf("[probe] ddrtop = %p", ddrtop);
	if (!ddrtop) goto out;
	unsigned char *au_buf = ddrtop + 0x10000;

	static const struct { int mode, fw; } variants[] = {
		{ 0, 512 }, { 4, 512 }, { 4, 768 }, { 0, 768 },
	};
	for (unsigned v = 0; v < sizeof(variants) / sizeof(variants[0]); v++) {
		int md = variants[v].mode, fw = variants[v].fw;
		static SceMpeg mpeg;
		memset(data, 0, size);
		r = sceMpegCreate(&mpeg, data, size, &rb, fw, md, (int)ddrtop);
		log_printf("[probe] v%u mode=%d fw=%d sceMpegCreate = 0x%08x", v, md, fw, r);
		if (r < 0)
			continue;
		SceMpegAvcMode mode = { -1, 3 };     /* 3 = GU_PSM_8888 */
		log_printf("[probe] v%u AvcDecodeMode = 0x%08x", v,
		           sceMpegAvcDecodeMode(&mpeg, &mode));
		void *es = sceMpegMallocAvcEsBuf(&mpeg);
		SceMpegAu *au = memalign(64, 64);
		memset(au, 0, 64);
		log_printf("[probe] v%u es=%p InitAu = 0x%08x", v, es,
		           sceMpegInitAu(&mpeg, es, au));

		void *dest = (void *)0x04000000;
		const unsigned int *vram = (const unsigned int *)0x44000000;
		int got = 0;
		for (int i = 0; i < tr.video_count && i < 4; i++) {
			int n = mp4_read_sample(MP4, &tr.video_samples[i], buf, 512 * 1024);
			if (n <= 0 || n > 0x3F0000)
				break;
			memcpy(au_buf, buf, n);
			AvcNal nal = {
				tr.sps, tr.sps_size, tr.pps, tr.pps_size,
				tr.nal_length_size, au_buf, n, (i == 0) ? 3 : 0,
			};
			int g = sceMpegGetAvcNalAu(&mpeg, &nal, au);
			int pics = 0;
			void *out = dest;
			int d = (g < 0) ? g : sceMpegAvcDecode(&mpeg, au, fw, &out, &pics);
			log_printf("[probe] v%u sample %d size %d: NalAu 0x%08x Decode 0x%08x pics %d au=%d",
			           v, i, n, g, d, pics, au->iAuSize);
			if (d < 0)
				break;                  /* never feed a failed decoder */
			if (pics > 0) {
				int w = (int)tr.video_width > fw ? fw : (int)tr.video_width;
				dump_bmp(vram, w, tr.video_height, fw);
				log_printf("[probe] frame.bmp written (%dx%u pitch %d)",
				           w, tr.video_height, fw);
				got = 1;
				break;
			}
		}
		sceMpegDelete(&mpeg);
		log_printf("[probe] v%u deleted", v);
		if (got)
			break;
	}
out:
	log_printf("[probe] done");
	sceKernelDelayThread(500000);
	sceKernelExitGame();
	return 0;
}
