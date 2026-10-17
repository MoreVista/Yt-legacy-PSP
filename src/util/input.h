#ifndef YT_UTIL_INPUT_H
#define YT_UTIL_INPUT_H

#include "../common.h"
#include <pspctrl.h>

/*
 * Button masks, taken from the SDK's PSP_CTRL_* values. The table that used
 * to be spelled out here by hand matched none of them -- the up key read as
 * TRIANGLE, which quit the app on the first press.
 */
#define BTN_SELECT      PSP_CTRL_SELECT
#define BTN_START       PSP_CTRL_START
#define BTN_UP          PSP_CTRL_UP
#define BTN_RIGHT       PSP_CTRL_RIGHT
#define BTN_DOWN        PSP_CTRL_DOWN
#define BTN_LEFT        PSP_CTRL_LEFT
#define BTN_L           PSP_CTRL_LTRIGGER
#define BTN_R           PSP_CTRL_RTRIGGER
#define BTN_TRIANGLE    PSP_CTRL_TRIANGLE
#define BTN_CIRCLE      PSP_CTRL_CIRCLE
#define BTN_CROSS       PSP_CTRL_CROSS
#define BTN_SQUARE      PSP_CTRL_SQUARE
#define BTN_SELECT_MASK (BTN_SELECT)

void input_init(void);
/* Refreshes the internal state; returns buttons newly pressed since the
 * previous call. */
uint32_t input_poll(void);
bool     input_held(uint32_t mask);

#endif
