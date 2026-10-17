#include "input.h"
#include <pspctrl.h>

static SceCtrlData g_pad;
static uint32_t     g_now;
static uint32_t     g_prev;
static bool         g_ready;

void input_init(void)
{
	memset(&g_pad, 0, sizeof(g_pad));
	sceCtrlSetSamplingMode(PSP_CTRL_MODE_DIGITAL);
	sceCtrlReadBufferPositive(&g_pad, 1);
	g_now = 0;
	g_prev = 0;
	g_ready = true;
}

uint32_t input_poll(void)
{
	if (!g_ready)
		return 0;

	if (sceCtrlReadBufferPositive(&g_pad, 1) <= 0) {
		/* No pad connected: report nothing rather than stale state. */
		g_now = 0;
	} else {
		g_now = g_pad.Buttons;
	}

	uint32_t hit = g_now & ~g_prev;
	g_prev = g_now;
	return hit;
}

bool input_held(uint32_t mask)
{
	return (g_now & mask) != 0;
}
