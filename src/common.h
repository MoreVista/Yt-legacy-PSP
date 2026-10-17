#ifndef YT_COMMON_H
#define YT_COMMON_H

#include <pspkernel.h>
#include <pspctrl.h>
#include <pspgu.h>
#include <pspgum.h>
#include <pspge.h>
#include <pspdisplay.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>

/* ---- Screen ---- */
#define SCREEN_WIDTH  480
#define SCREEN_HEIGHT 272

/* ---- Color format (GU_PSM_8888 is provided by pspgu.h) ---- */

/* ---- Paths ---- */
#define APP_TITLE     "YT Legacy PSP"
#define SAVE_DIR      "ms0:/PSP/GAME/YTPSP"
#define CACHE_DIR     "ms0:/PSP/GAME/YTPSP/cache"

/* ---- Menu / state ---- */
typedef enum {
	STATE_BOOT = 0,
	STATE_WIFI_CONNECTING,
	STATE_INSTANCE_SETUP,
	STATE_SERVER_TEST,
	STATE_SEARCH_MENU,
	STATE_SEARCH_INPUT,
	STATE_SEARCH_RESULTS,
	STATE_VIDEO_DETAIL,
	STATE_DOWNLOADING,
	STATE_PLAYER,
	STATE_ERROR,
	STATE_EXIT
} app_state_t;

#endif /* YT_COMMON_H */
