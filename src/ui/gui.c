#include "gui.h"
#include "../util/log.h"
#include <pspgu.h>
#include <pspgum.h>
#include <pspge.h>
#include <malloc.h>
#include <pspdisplay.h>
#include <psputility.h>

/*
 * 2D renderer on the immediate-mode GU interface, laid out like the PSPSDK
 * samples: through-mode (GU_TRANSFORM_2D) sprites in screen coordinates,
 * double-buffered with sceGuSwapBuffers.
 *
 * Two hardware facts decide the shape of this file:
 *
 *  - The GE reads memory directly, behind the CPU's data cache, and with a
 *    GU_DIRECT list it starts on each draw as soon as sceGuDrawArray returns.
 *    Vertices therefore come from sceGuGetMemory, which carves them out of
 *    the display list (sceGuStart maps that list uncached), and the font
 *    texture is written into VRAM through its uncached alias. Vertices in a
 *    reused malloc buffer were both stale in RAM and overwritten before the
 *    GE read them, which is what garbled the screen.
 *  - A display list is only open between gui_start_frame and the flip in
 *    gui_end_frame. gui_end_frame opens the next one straight away, because
 *    the main loop draws, presents, and draws again without a start call.
 */

#define FONT_FIRST      0x20
#define FONT_LAST       0x7E
#define FONT_GLYPHS     (FONT_LAST - FONT_FIRST + 1)
#define FONT_CELL_W     8
#define FONT_CELL_H     8
#define FONT_COLS       16
#define FONT_TEX_W      (FONT_COLS * FONT_CELL_W)   /* 128 */
#define FONT_TEX_H      64
#define FONT_ADVANCE    8

/*
 * 5x7 glyphs for printable ASCII, column-major: one byte per column, LSB is
 * the top row. Reading them as rows, as this file used to, transposes every
 * glyph.
 */
static const unsigned char g_font[FONT_GLYPHS][FONT_CELL_H] = {
	{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /*   */
	{0x00,0x00,0x5F,0x00,0x00,0x00,0x00,0x00}, /* ! */
	{0x00,0x07,0x00,0x07,0x00,0x00,0x00,0x00}, /* " */
	{0x14,0x7F,0x14,0x7F,0x14,0x00,0x00,0x00}, /* # */
	{0x24,0x2A,0x7F,0x2A,0x12,0x00,0x00,0x00}, /* $ */
	{0x23,0x13,0x08,0x64,0x62,0x00,0x00,0x00}, /* % */
	{0x36,0x49,0x55,0x22,0x50,0x00,0x00,0x00}, /* & */
	{0x00,0x05,0x03,0x00,0x00,0x00,0x00,0x00}, /* ' */
	{0x00,0x1C,0x22,0x41,0x00,0x00,0x00,0x00}, /* ( */
	{0x00,0x41,0x22,0x1C,0x00,0x00,0x00,0x00}, /* ) */
	{0x14,0x08,0x3E,0x08,0x14,0x00,0x00,0x00}, /* * */
	{0x08,0x08,0x3E,0x08,0x08,0x00,0x00,0x00}, /* + */
	{0x00,0x50,0x30,0x00,0x00,0x00,0x00,0x00}, /* , */
	{0x08,0x08,0x08,0x08,0x08,0x00,0x00,0x00}, /* - */
	{0x00,0x60,0x60,0x00,0x00,0x00,0x00,0x00}, /* . */
	{0x20,0x10,0x08,0x04,0x02,0x00,0x00,0x00}, /* / */
	{0x3E,0x51,0x49,0x45,0x3E,0x00,0x00,0x00}, /* 0 */
	{0x00,0x42,0x7F,0x40,0x00,0x00,0x00,0x00}, /* 1 */
	{0x42,0x61,0x51,0x49,0x46,0x00,0x00,0x00}, /* 2 */
	{0x21,0x41,0x45,0x4B,0x31,0x00,0x00,0x00}, /* 3 */
	{0x18,0x14,0x12,0x7F,0x10,0x00,0x00,0x00}, /* 4 */
	{0x27,0x45,0x45,0x45,0x39,0x00,0x00,0x00}, /* 5 */
	{0x3C,0x4A,0x49,0x49,0x30,0x00,0x00,0x00}, /* 6 */
	{0x01,0x71,0x09,0x05,0x03,0x00,0x00,0x00}, /* 7 */
	{0x36,0x49,0x49,0x49,0x36,0x00,0x00,0x00}, /* 8 */
	{0x06,0x49,0x49,0x29,0x1E,0x00,0x00,0x00}, /* 9 */
	{0x00,0x36,0x36,0x00,0x00,0x00,0x00,0x00}, /* : */
	{0x00,0x56,0x36,0x00,0x00,0x00,0x00,0x00}, /* ; */
	{0x00,0x08,0x14,0x22,0x41,0x00,0x00,0x00}, /* < */
	{0x14,0x14,0x14,0x14,0x14,0x00,0x00,0x00}, /* = */
	{0x41,0x22,0x14,0x08,0x00,0x00,0x00,0x00}, /* > */
	{0x02,0x01,0x51,0x09,0x06,0x00,0x00,0x00}, /* ? */
	{0x32,0x49,0x79,0x41,0x3E,0x00,0x00,0x00}, /* @ */
	{0x7E,0x11,0x11,0x11,0x7E,0x00,0x00,0x00}, /* A */
	{0x7F,0x49,0x49,0x49,0x36,0x00,0x00,0x00}, /* B */
	{0x3E,0x41,0x41,0x41,0x22,0x00,0x00,0x00}, /* C */
	{0x7F,0x41,0x41,0x22,0x1C,0x00,0x00,0x00}, /* D */
	{0x7F,0x49,0x49,0x49,0x41,0x00,0x00,0x00}, /* E */
	{0x7F,0x09,0x09,0x09,0x01,0x00,0x00,0x00}, /* F */
	{0x3E,0x41,0x49,0x49,0x7A,0x00,0x00,0x00}, /* G */
	{0x7F,0x08,0x08,0x08,0x7F,0x00,0x00,0x00}, /* H */
	{0x00,0x41,0x7F,0x41,0x00,0x00,0x00,0x00}, /* I */
	{0x20,0x40,0x41,0x3F,0x01,0x00,0x00,0x00}, /* J */
	{0x7F,0x08,0x14,0x22,0x41,0x00,0x00,0x00}, /* K */
	{0x7F,0x40,0x40,0x40,0x40,0x00,0x00,0x00}, /* L */
	{0x7F,0x02,0x0C,0x02,0x7F,0x00,0x00,0x00}, /* M */
	{0x7F,0x04,0x08,0x10,0x7F,0x00,0x00,0x00}, /* N */
	{0x3E,0x41,0x41,0x41,0x3E,0x00,0x00,0x00}, /* O */
	{0x7F,0x09,0x09,0x09,0x06,0x00,0x00,0x00}, /* P */
	{0x3E,0x41,0x51,0x21,0x5E,0x00,0x00,0x00}, /* Q */
	{0x7F,0x09,0x19,0x29,0x46,0x00,0x00,0x00}, /* R */
	{0x46,0x49,0x49,0x49,0x31,0x00,0x00,0x00}, /* S */
	{0x01,0x01,0x7F,0x01,0x01,0x00,0x00,0x00}, /* T */
	{0x3F,0x40,0x40,0x40,0x3F,0x00,0x00,0x00}, /* U */
	{0x1F,0x20,0x40,0x20,0x1F,0x00,0x00,0x00}, /* V */
	{0x3F,0x40,0x38,0x40,0x3F,0x00,0x00,0x00}, /* W */
	{0x63,0x14,0x08,0x14,0x63,0x00,0x00,0x00}, /* X */
	{0x07,0x08,0x70,0x08,0x07,0x00,0x00,0x00}, /* Y */
	{0x61,0x51,0x49,0x45,0x43,0x00,0x00,0x00}, /* Z */
	{0x00,0x7F,0x41,0x41,0x00,0x00,0x00,0x00}, /* [ */
	{0x02,0x04,0x08,0x10,0x20,0x00,0x00,0x00}, /* \ */
	{0x00,0x41,0x41,0x7F,0x00,0x00,0x00,0x00}, /* ] */
	{0x04,0x02,0x01,0x02,0x04,0x00,0x00,0x00}, /* ^ */
	{0x40,0x40,0x40,0x40,0x40,0x00,0x00,0x00}, /* _ */
	{0x00,0x01,0x02,0x04,0x00,0x00,0x00,0x00}, /* ` */
	{0x20,0x54,0x54,0x54,0x78,0x00,0x00,0x00}, /* a */
	{0x7F,0x48,0x44,0x44,0x38,0x00,0x00,0x00}, /* b */
	{0x38,0x44,0x44,0x44,0x20,0x00,0x00,0x00}, /* c */
	{0x38,0x44,0x44,0x48,0x7F,0x00,0x00,0x00}, /* d */
	{0x38,0x54,0x54,0x54,0x18,0x00,0x00,0x00}, /* e */
	{0x08,0x7E,0x09,0x01,0x02,0x00,0x00,0x00}, /* f */
	{0x0C,0x52,0x52,0x52,0x3E,0x00,0x00,0x00}, /* g */
	{0x7F,0x08,0x04,0x04,0x78,0x00,0x00,0x00}, /* h */
	{0x00,0x44,0x7D,0x40,0x00,0x00,0x00,0x00}, /* i */
	{0x20,0x40,0x44,0x3D,0x00,0x00,0x00,0x00}, /* j */
	{0x7F,0x10,0x28,0x44,0x00,0x00,0x00,0x00}, /* k */
	{0x00,0x41,0x7F,0x40,0x00,0x00,0x00,0x00}, /* l */
	{0x7C,0x04,0x18,0x04,0x78,0x00,0x00,0x00}, /* m */
	{0x7C,0x08,0x04,0x04,0x78,0x00,0x00,0x00}, /* n */
	{0x38,0x44,0x44,0x44,0x38,0x00,0x00,0x00}, /* o */
	{0x7C,0x14,0x14,0x14,0x08,0x00,0x00,0x00}, /* p */
	{0x08,0x14,0x14,0x18,0x7C,0x00,0x00,0x00}, /* q */
	{0x7C,0x08,0x04,0x04,0x08,0x00,0x00,0x00}, /* r */
	{0x48,0x54,0x54,0x54,0x20,0x00,0x00,0x00}, /* s */
	{0x04,0x3F,0x44,0x40,0x20,0x00,0x00,0x00}, /* t */
	{0x3C,0x40,0x40,0x20,0x7C,0x00,0x00,0x00}, /* u */
	{0x1C,0x20,0x40,0x20,0x1C,0x00,0x00,0x00}, /* v */
	{0x3C,0x40,0x30,0x40,0x3C,0x00,0x00,0x00}, /* w */
	{0x44,0x28,0x10,0x28,0x44,0x00,0x00,0x00}, /* x */
	{0x0C,0x50,0x50,0x50,0x3C,0x00,0x00,0x00}, /* y */
	{0x44,0x64,0x54,0x4C,0x44,0x00,0x00,0x00}, /* z */
	{0x00,0x08,0x36,0x41,0x00,0x00,0x00,0x00}, /* { */
	{0x00,0x00,0x7F,0x00,0x00,0x00,0x00,0x00}, /* | */
	{0x00,0x41,0x36,0x08,0x00,0x00,0x00,0x00}, /* } */
	{0x10,0x08,0x08,0x10,0x08,0x00,0x00,0x00}, /* ~ */
};

/*
 * Vertex layouts. The GE fetches components in a fixed order -- texture,
 * colour, position -- whatever order the C struct declares them in, so the
 * structs follow that order. Both are padded to 4-byte alignment, which is
 * the stride the GE uses for these formats.
 */
typedef struct {
	unsigned int color;
	short x, y, z;
	short pad;
} rect_vertex_t;

typedef struct {
	unsigned short u, v;
	unsigned int color;
	short x, y, z;
	short pad;
} glyph_vertex_t;

#define RECT_VTYPE  (GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D)
#define GLYPH_VTYPE (GU_TEXTURE_16BIT | GU_COLOR_8888 | GU_VERTEX_16BIT | \
                     GU_TRANSFORM_2D)

/*
 * VRAM layout, offsets from the start of EDRAM. Frame buffers are 512 pixels
 * wide (the pitch must be a power of two) by 272 lines of 32-bit pixels, so
 * each is 512*272*4 = 0x88000 bytes. The depth buffer gets its own region:
 * left to sceGuDrawBuffer it lands directly after the draw buffer, which is
 * the display buffer, and every depth clear then wrote over the picture.
 */
#define FB_PITCH        512
#define FB_SIZE         (FB_PITCH * SCREEN_HEIGHT * 4)
#define VRAM_FB0        0
#define VRAM_FB1        (VRAM_FB0 + FB_SIZE)
#define VRAM_DEPTH      (VRAM_FB1 + FB_SIZE)
#define VRAM_FONT       (VRAM_DEPTH + FB_PITCH * SCREEN_HEIGHT * 2)

/*
 * Glyph vertices come out of the display list, 32 bytes per character, and a
 * results page draws a few thousand characters, so the list is sized well
 * past what a frame needs.
 */
#define GU_LIST_BYTES   (256 * 1024)

static unsigned int *g_dlist;
static void         *g_font_tex;    /* GE address of the font texture */
static bool          g_ready;
static bool          g_in_frame;

int utf8_char_len(unsigned char c)
{
	if (c < 0x80)     return 1;
	if ((c & 0xE0) == 0xC0) return 2;
	if ((c & 0xF0) == 0xE0) return 3;
	if ((c & 0xF8) == 0xF0) return 4;
	return 1;
}

/*
 * Length of the UTF-8 sequence at p, cut short at a NUL. A string truncated
 * by snprintf can end in the middle of a sequence; stepping by the lead
 * byte's length alone then jumps over the terminator and walks on into
 * whatever memory follows.
 */
static int seq_len(const unsigned char *p)
{
	int n = utf8_char_len(*p);
	for (int i = 1; i < n; i++) {
		if (!p[i])
			return i;
	}
	return n;
}

void utf8_copy_trunc(char *dst, int dst_size, const char *src)
{
	const unsigned char *s = (const unsigned char *)src;
	int o = 0;
	while (*s) {
		int n = seq_len(s);
		if (n != utf8_char_len(*s))
			break;                    /* incomplete sequence: drop it */
		if (o + n > dst_size - 1)
			break;
		memmove(dst + o, s, n);       /* dst may alias src */
		o += n;
		s += n;
	}
	dst[o] = 0;
}

static void build_font(void)
{
	/* Uncached alias of VRAM, so nothing is left behind in the data cache. */
	unsigned int *tex = (unsigned int *)(0x40000000u |
		((unsigned int)sceGeEdramGetAddr() + VRAM_FONT));
	g_font_tex = (void *)((unsigned int)sceGeEdramGetAddr() + VRAM_FONT);

	for (int i = 0; i < FONT_TEX_W * FONT_TEX_H; i++)
		tex[i] = 0;

	/* White glyphs on a transparent background; the vertex colour tints them. */
	for (int i = 0; i < FONT_GLYPHS; i++) {
		int cx = (i % FONT_COLS) * FONT_CELL_W;
		int cy = (i / FONT_COLS) * FONT_CELL_H;
		for (int col = 0; col < FONT_CELL_W; col++) {
			unsigned char bits = g_font[i][col];
			for (int row = 0; row < FONT_CELL_H; row++) {
				if (bits & (1 << row))
					tex[(cy + row) * FONT_TEX_W + cx + col] = 0xFFFFFFFFu;
			}
		}
	}
}

/* Render state. GE registers persist across lists, but re-emitting it each
 * frame keeps the state right after the OSK, which draws with its own. */
static void emit_state(void)
{
	sceGuOffset(2048 - (SCREEN_WIDTH / 2), 2048 - (SCREEN_HEIGHT / 2));
	sceGuViewport(2048, 2048, SCREEN_WIDTH, SCREEN_HEIGHT);
	sceGuScissor(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);
	sceGuEnable(GU_SCISSOR_TEST);
	sceGuDisable(GU_DEPTH_TEST);
	sceGuDisable(GU_CULL_FACE);
	sceGuEnable(GU_BLEND);
	sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
	sceGuShadeModel(GU_FLAT);
	sceGuTexMode(GU_PSM_8888, 0, 0, 0);
	sceGuTexImage(0, FONT_TEX_W, FONT_TEX_H, FONT_TEX_W, g_font_tex);
	sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
	sceGuTexFilter(GU_NEAREST, GU_NEAREST);
	sceGuTexWrap(GU_CLAMP, GU_CLAMP);
	sceGuDisable(GU_TEXTURE_2D);
}

void gui_init(void)
{
	sceGuInit();

	/* sceGuStart maps the list uncached itself; it must be 16-byte aligned. */
	g_dlist = memalign(16, GU_LIST_BYTES);
	if (!g_dlist) {
		log_printf("[gui] no memory for display list");
		return;
	}
	build_font();

	/*
	 * Everything except sceGuInit and sceGuDispBuffer records into the
	 * display list, so the buffer setup happens inside one.
	 */
	sceGuStart(GU_DIRECT, g_dlist);
	sceGuDrawBuffer(GU_PSM_8888, (void *)VRAM_FB0, FB_PITCH);
	sceGuDispBuffer(SCREEN_WIDTH, SCREEN_HEIGHT, (void *)VRAM_FB1, FB_PITCH);
	sceGuDepthBuffer((void *)VRAM_DEPTH, FB_PITCH);
	emit_state();
	sceGuClearColor(0xFF000000u);
	sceGuClear(GU_COLOR_BUFFER_BIT);
	sceGuFinish();
	sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);

	sceDisplayWaitVblankStart();
	sceGuDisplay(GU_TRUE);
	g_ready = true;
}

void gui_start_frame(void)
{
	if (!g_ready || g_in_frame)
		return;
	/* sceGuStart re-points the GE at the current draw buffer by itself. */
	sceGuStart(GU_DIRECT, g_dlist);
	emit_state();
	g_in_frame = true;
}

#ifdef GUI_SCREENSHOT
static int g_last_list_bytes;
#endif

static void finish_list(void)
{
#ifdef GUI_SCREENSHOT
	g_last_list_bytes = sceGuCheckList();
#endif
	sceGuFinish();
	sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);
	g_in_frame = false;
}

static unsigned int g_draw_offset = VRAM_FB0;

#ifdef GUI_SCREENSHOT
#include <pspiofilemgr.h>
/*
 * Diagnostic builds only: writes the finished frame to the memory stick as a
 * 32-bit BMP, so the rendering can be checked off-device. GUI_SCREENSHOT is
 * the frame number to capture.
 */
static void dump_frame(void)
{
	static int frame;
	++frame;
	if (frame == GUI_SCREENSHOT - 1 || frame == GUI_SCREENSHOT)
		log_printf("[gui] frame %d list bytes %d", frame, g_last_list_bytes);
	if (frame != GUI_SCREENSHOT)
		return;

	const unsigned int *px = (const unsigned int *)(0x40000000u |
		((unsigned int)sceGeEdramGetAddr() + g_draw_offset));
	SceUID fd = sceIoOpen("ms0:/PSP/GAME/YTPSP/screen.bmp",
	                      PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
	if (fd < 0)
		return;

	unsigned char hdr[54] = { 'B', 'M' };
	unsigned int img = SCREEN_WIDTH * SCREEN_HEIGHT * 4;
	unsigned int fields[] = { 54 + img, 0, 54, 40, SCREEN_WIDTH,
	                          (unsigned int)-SCREEN_HEIGHT, 1 | (32 << 16), 0,
	                          img, 2835, 2835, 0, 0 };
	memcpy(hdr + 2, fields, sizeof(fields));
	sceIoWrite(fd, hdr, sizeof(hdr));

	static unsigned int row[SCREEN_WIDTH];
	for (int y = 0; y < SCREEN_HEIGHT; y++) {
		for (int x = 0; x < SCREEN_WIDTH; x++) {
			unsigned int c = px[y * FB_PITCH + x];      /* ABGR -> ARGB */
			row[x] = (c & 0xFF00FF00u) | ((c & 0xFF) << 16) |
			         ((c >> 16) & 0xFF);
		}
		sceIoWrite(fd, row, sizeof(row));
	}
	sceIoClose(fd);
	log_printf("[gui] frame %d saved to screen.bmp", frame);
}
#endif

static void flip(void)
{
#ifdef GUI_SCREENSHOT
	dump_frame();
#endif
	sceDisplayWaitVblankStart();
	g_draw_offset = (unsigned int)sceGuSwapBuffers();
}

void gui_end_frame(void)
{
	if (!g_ready)
		return;
	if (!g_in_frame)
		gui_start_frame();
	finish_list();
	flip();
	gui_start_frame();
}

/*
 * One frame while the system OSK is up. The OSK draws into the current draw
 * buffer with the GE, so it has to run after our list has finished and
 * before the flip; sceUtilityOskUpdate is what advances it at all.
 */
void gui_present_osk_frame(void)
{
	if (!g_ready)
		return;
	if (!g_in_frame)
		gui_start_frame();
	gui_clear(COLOR_BG);
	finish_list();
	sceUtilityOskUpdate(1);
	flip();
	gui_start_frame();
}

void gui_clear(gui_color_t color)
{
	if (!g_in_frame)
		return;
	sceGuClearColor(color);
	sceGuClear(GU_COLOR_BUFFER_BIT);
}

void gui_fill_rect(int x, int y, int w, int h, gui_color_t color)
{
	if (w <= 0 || h <= 0 || !g_in_frame)
		return;

	rect_vertex_t *v = sceGuGetMemory(2 * sizeof(rect_vertex_t));
	v[0] = (rect_vertex_t){ color, (short)x,       (short)y,       0, 0 };
	v[1] = (rect_vertex_t){ color, (short)(x + w), (short)(y + h), 0, 0 };

	sceGuDisable(GU_TEXTURE_2D);
	sceGuDrawArray(GU_SPRITES, RECT_VTYPE, 2, NULL, v);
}

void gui_draw_rect(int x, int y, int w, int h, gui_color_t color)
{
	gui_fill_rect(x, y, w, 1, color);
	gui_fill_rect(x, y + h - 1, w, 1, color);
	gui_fill_rect(x, y, 1, h, color);
	gui_fill_rect(x + w - 1, y, 1, h, color);
}

int gui_draw_text(int x, int y, const char *utf8, gui_color_t color)
{
	if (!utf8 || !g_in_frame)
		return x;

	/* Count drawable glyphs first so the vertices come from one allocation. */
	int n = 0;
	for (const unsigned char *p = (const unsigned char *)utf8; *p; ) {
		int clen = seq_len(p);
		if (clen == 1 && *p >= FONT_FIRST && *p <= FONT_LAST && *p != ' ')
			n++;
		p += clen;
	}

	int cx = x, cy = y;
	glyph_vertex_t *v = NULL;
	if (n > 0)
		v = sceGuGetMemory(n * 2 * sizeof(glyph_vertex_t));

	int i = 0;
	for (const unsigned char *p = (const unsigned char *)utf8; *p; ) {
		if (*p == '\n') {
			cx = x;
			cy += FONT_CELL_H + 2;
			p++;
			continue;
		}

		int clen = seq_len(p);
		int code = *p;

		if (clen == 1 && code >= FONT_FIRST && code <= FONT_LAST) {
			if (code != ' ' && v && i < n) {
				int idx = code - FONT_FIRST;
				unsigned short u0 = (idx % FONT_COLS) * FONT_CELL_W;
				unsigned short v0 = (idx / FONT_COLS) * FONT_CELL_H;
				v[i * 2] = (glyph_vertex_t){
					u0, v0, color, (short)cx, (short)cy, 0, 0 };
				v[i * 2 + 1] = (glyph_vertex_t){
					u0 + FONT_CELL_W, v0 + FONT_CELL_H, color,
					(short)(cx + FONT_CELL_W), (short)(cy + FONT_CELL_H), 0, 0 };
				i++;
			}
			cx += FONT_ADVANCE;
		} else if (clen == 1) {
			cx += FONT_ADVANCE;
		} else {
			/* No glyph data for non-ASCII; keep the advance so runs of
			 * CJK text still occupy sensible space. */
			cx += (clen == 2) ? FONT_ADVANCE : FONT_ADVANCE * 2;
		}
		p += clen;
	}

	if (i > 0) {
		sceGuEnable(GU_TEXTURE_2D);
		sceGuDrawArray(GU_SPRITES, GLYPH_VTYPE, i * 2, NULL, v);
		sceGuDisable(GU_TEXTURE_2D);
	}
	return cx;
}

/*
 * Draws an RGBA8888 image from RAM, scaled into (dx, dy, dw, dh).
 *
 * The GE samples at most 512 texels per axis, and a 640-wide video frame
 * is wider than that, so the image goes out as vertical strips 64 texels
 * wide, each bound as its own texture at an offset into the same buffer.
 * 64-texel strips also keep the GE's texture cache efficient. The rows past
 * `src_h` are never sampled, so the texture height can round up to 512.
 */
void gui_draw_rgba(const void *pixels, int pitch, int src_w, int src_h,
                   int dx, int dy, int dw, int dh)
{
	if (!g_in_frame || !pixels || src_w <= 0 || src_h <= 0 || src_h > 512)
		return;

	const int strip = 64;
	int strips = (src_w + strip - 1) / strip;
	glyph_vertex_t *v = sceGuGetMemory(strips * 2 * sizeof(glyph_vertex_t));

	sceGuTexFlush();                 /* the frame changed since last time */
	sceGuTexFilter(GU_LINEAR, GU_LINEAR);
	/*
	 * Opaque regardless of the image's alpha: the ME's colour conversion
	 * leaves alpha at 0, which with source-alpha blending drew the video
	 * fully transparent -- a black screen while decoding ran fine.
	 */
	sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGB);
	sceGuEnable(GU_TEXTURE_2D);
	for (int i = 0; i < strips; i++) {
		int sx = i * strip;
		int sw = (sx + strip > src_w) ? src_w - sx : strip;
		int x0 = dx + sx * dw / src_w;
		int x1 = dx + (sx + sw) * dw / src_w;

		sceGuTexImage(0, strip, 512, pitch,
		              (const uint32_t *)pixels + sx);
		v[i * 2] = (glyph_vertex_t){
			0, 0, 0xFFFFFFFFu, (short)x0, (short)dy, 0, 0 };
		v[i * 2 + 1] = (glyph_vertex_t){
			(unsigned short)sw, (unsigned short)src_h, 0xFFFFFFFFu,
			(short)x1, (short)(dy + dh), 0, 0 };
		sceGuDrawArray(GU_SPRITES, GLYPH_VTYPE, 2, NULL, &v[i * 2]);
	}
	sceGuDisable(GU_TEXTURE_2D);

	/* Back to the font texture for any text drawn after this. */
	sceGuTexImage(0, FONT_TEX_W, FONT_TEX_H, FONT_TEX_W, g_font_tex);
	sceGuTexFilter(GU_NEAREST, GU_NEAREST);
	sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
}

int gui_text_width(const char *utf8)
{
	int w = 0;
	if (!utf8)
		return 0;
	for (const unsigned char *p = (const unsigned char *)utf8; *p && *p != '\n'; ) {
		int clen = seq_len(p);
		w += (clen == 1) ? FONT_ADVANCE : (clen == 2 ? FONT_ADVANCE
		                                              : FONT_ADVANCE * 2);
		p += clen;
	}
	return w;
}
