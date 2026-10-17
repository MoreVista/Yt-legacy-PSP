#ifndef YT_UI_KEYBOARD_H
#define YT_UI_KEYBOARD_H

#include "../common.h"
#include <psputility.h>
#include <psputility_osk.h>

#define OSK_TEXT_CHARS 64

typedef struct {
	SceUtilityOskParams param;
	SceUtilityOskData   field;
	unsigned short      desc[32];
	unsigned short      intext[OSK_TEXT_CHARS];
	unsigned short      outtext[OSK_TEXT_CHARS];
	bool                running;
	bool                accepted;
} osk_context_t;

void osk_init(osk_context_t *ctx);

/*
 * Runs the on-screen keyboard modally, pumping the display while it is up.
 * Returns true when the user accepted a value (written to utf8_out), false on
 * cancel. Returns false immediately if the OSK refuses to start.
 */
bool osk_prompt(osk_context_t *ctx, const char *prompt_utf8,
                const char *initial_utf8, char *utf8_out, int utf8_out_size);

/* UTF-8 <-> UTF-16LE conversion used by the OSK bridge. */
int  utf8_to_utf16(const char *in, unsigned short *out, int out_units);
int  utf16_to_utf8(const unsigned short *in, char *out, int out_bytes);

#endif
