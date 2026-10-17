#include "keyboard.h"
#include "gui.h"
#include "../util/log.h"

int utf8_to_utf16(const char *in, unsigned short *out, int out_units)
{
	const unsigned char *p = (const unsigned char *)in;
	int n = 0;

	while (*p && n < out_units) {
		unsigned int cp;
		if (*p < 0x80) {
			cp = *p++;
		} else if ((*p & 0xE0) == 0xC0) {
			cp = (unsigned int)(*p++ & 0x1F) << 6;
			if ((*p & 0xC0) != 0x80)
				break;
			cp |= *p++ & 0x3F;
		} else if ((*p & 0xF0) == 0xE0) {
			cp = (unsigned int)(*p++ & 0x0F) << 12;
			if ((*p & 0xC0) != 0x80)
				break;
			cp |= (unsigned int)(*p++ & 0x3F) << 6;
			if ((*p & 0xC0) != 0x80)
				break;
			cp |= *p++ & 0x3F;
		} else {
			/* Four-byte sequences (emoji) exceed the BMP; the OSK stores
			 * UTF-16, so substitute rather than truncate mid-sequence. */
			p++;
			cp = 0xFFFD;
		}
		out[n++] = (unsigned short)cp;
	}
	out[n] = 0;
	return n;
}

int utf16_to_utf8(const unsigned short *in, char *out, int out_bytes)
{
	int o = 0;
	for (int i = 0; in[i] && o < out_bytes - 4; i++) {
		unsigned int cp = in[i];
		if (cp >= 0xD800 && cp <= 0xDBFF && in[i + 1] >= 0xDC00 &&
		    in[i + 1] <= 0xDFFF) {
			cp = 0x10000 + ((cp - 0xD800) << 10) + (in[i + 1] - 0xDC00);
			i++;
		}

		if (cp < 0x80) {
			out[o++] = (char)cp;
		} else if (cp < 0x800) {
			out[o++] = (char)(0xC0 | (cp >> 6));
			out[o++] = (char)(0x80 | (cp & 0x3F));
		} else if (cp < 0x10000) {
			out[o++] = (char)(0xE0 | (cp >> 12));
			out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
			out[o++] = (char)(0x80 | (cp & 0x3F));
		} else {
			out[o++] = (char)(0xF0 | (cp >> 18));
			out[o++] = (char)(0x80 | ((cp >> 12) & 0x3F));
			out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
			out[o++] = (char)(0x80 | (cp & 0x3F));
		}
	}
	out[o] = 0;
	return o;
}

void osk_init(osk_context_t *ctx)
{
	memset(ctx, 0, sizeof(*ctx));
	ctx->field.language = PSP_UTILITY_OSK_LANGUAGE_JAPANESE;
	ctx->field.inputtype = PSP_UTILITY_OSK_INPUTTYPE_ALL;
	ctx->field.lines = 1;
	ctx->field.outtextlength = OSK_TEXT_CHARS;
	ctx->field.outtextlimit = OSK_TEXT_CHARS - 1;
	ctx->field.outtext = ctx->outtext;
	ctx->field.unk_00 = 0;
	ctx->field.unk_04 = 0;
	ctx->field.unk_12 = 0;
	ctx->field.unk_24 = 0;

	ctx->param.base.size = sizeof(SceUtilityOskParams);
	ctx->param.base.language = PSP_UTILITY_OSK_LANGUAGE_JAPANESE;
	/* No button swap: the on-screen keyboard draws its own labels. */
	ctx->param.base.buttonSwap = 0;
	ctx->param.base.graphicsThread = 0x1C;
	ctx->param.base.accessThread = 0x18;
	ctx->param.base.fontThread = 0x18;
	ctx->param.base.soundThread = 0x18;
	ctx->param.datacount = 1;
	ctx->param.data = &ctx->field;
	ctx->param.unk_60 = 0;
}

bool osk_prompt(osk_context_t *ctx, const char *prompt_utf8,
                const char *initial_utf8, char *utf8_out, int utf8_out_size)
{
	osk_init(ctx);

	utf8_to_utf16(prompt_utf8, ctx->desc,
	              sizeof(ctx->desc) / sizeof(ctx->desc[0]) - 1);
	utf8_to_utf16(initial_utf8 ? initial_utf8 : "", ctx->intext, OSK_TEXT_CHARS - 1);
	ctx->outtext[0] = 0;
	ctx->field.result = PSP_UTILITY_OSK_RESULT_UNCHANGED;
	ctx->accepted = false;

	int r = sceUtilityOskInitStart(&ctx->param);
	if (r < 0) {
		log_printf("[osk] init failed 0x%08x", r);
		return false;
	}
	ctx->running = true;

	/*
	 * The OSK only advances when sceUtilityOskUpdate is called once per
	 * frame (gui_present_osk_frame does that between our list and the flip).
	 * It reports QUIT when the user confirms or cancels, and only then may
	 * shutdown be requested; it then passes FINISHED on its way to NONE.
	 * Waiting for NONE without either step hangs with the dialog on screen.
	 */
	bool shutting_down = false;
	for (;;) {
		int status = sceUtilityOskGetStatus();
		if (status == PSP_UTILITY_DIALOG_NONE)
			break;

		if (status == PSP_UTILITY_DIALOG_VISIBLE) {
			gui_present_osk_frame();
			continue;            /* the flip already waited for vblank */
		}
		if (status == PSP_UTILITY_DIALOG_QUIT && !shutting_down) {
			sceUtilityOskShutdownStart();
			shutting_down = true;
		}
		gui_end_frame();
	}
	ctx->running = false;

	if (ctx->field.result == PSP_UTILITY_OSK_RESULT_CHANGED) {
		utf16_to_utf8(ctx->outtext, utf8_out, utf8_out_size);
		return true;
	}

	utf8_out[0] = 0;
	return false;
}
