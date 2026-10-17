#ifndef YT_UI_GUI_H
#define YT_UI_GUI_H

#include "../common.h"

typedef unsigned int gui_color_t;

/* ABGR8888 as the GE reads it; opaque, since blending uses source alpha. */
#define RGB(r, g, b)  (0xFF000000u | ((b) << 16) | ((g) << 8) | (r))

#define COLOR_BG        RGB(18, 20, 28)
#define COLOR_PANEL     RGB(30, 34, 46)
#define COLOR_PANEL2    RGB(52, 58, 74)
#define COLOR_ACCENT    RGB(64, 132, 244)
#define COLOR_TEXT      RGB(226, 230, 238)
#define COLOR_DIM       RGB(130, 138, 152)
#define COLOR_HIGHLIGHT RGB(38, 62, 110)
#define COLOR_ERROR     RGB(232, 96, 96)
#define COLOR_OK        RGB(96, 208, 128)

void gui_init(void);
void gui_start_frame(void);
void gui_end_frame(void);
void gui_clear(gui_color_t color);

void gui_fill_rect(int x, int y, int w, int h, gui_color_t color);
void gui_draw_rect(int x, int y, int w, int h, gui_color_t color);

/*
 * Draws UTF-8 text. Characters outside the built-in ASCII font are skipped
 * rather than drawn as garbage, so CJK titles degrade to their ASCII subset.
 * Returns the advance width in pixels.
 */
int  gui_draw_text(int x, int y, const char *utf8, gui_color_t color);
int  gui_text_width(const char *utf8);

/* RGBA8888 image in RAM (e.g. a decoded video frame), scaled to fit. */
void gui_draw_rgba(const void *pixels, int pitch, int src_w, int src_h,
                   int dx, int dy, int dw, int dh);

void gui_present_osk_frame(void);

/* UTF-8 helpers shared with the list renderer. */
int  utf8_char_len(unsigned char c);
/* Copies at most max_bytes, never splitting a multi-byte sequence. */
void utf8_copy_trunc(char *dst, int dst_size, const char *src);

#endif
