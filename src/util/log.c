#include "log.h"
#include <pspiofilemgr.h>
#include <pspdisplay.h>
#include <pspge.h>

#define LOG_MAX_LINES 48
#define LOG_LINE_LEN  200
#define LOG_TRACE_FILE "ms0:/PSP/GAME/YTPSP/boot.log"

/*
 * Deliberately does not use pspDebugScreenPrintf. That lives in the same
 * object as the kprintf handler, and linking the object pulls
 * sceKernelRegisterDefaultExceptionHandler in, which in turn drags a
 * reference to ExceptionManagerForKernel -- a kernel-mode module that a
 * user-mode EBOOT cannot resolve. That reference alone made the console
 * refuse the module with SCE_KERNEL_ERROR_LIBRARY_NOTFOUND (0x8002013C),
 * and it cost a launch before it was found. The log goes to the memory stick
 * and the on-screen buffer is cleared directly instead.
 */
static char g_log[LOG_MAX_LINES][LOG_LINE_LEN];
static int  g_log_head;
static int  g_log_count;

static unsigned int *g_screen;
static int g_screen_width = 480;
static int g_screen_height = 272;

void log_init(void)
{
	g_log_head = 0;
	g_log_count = 0;

	/* The display buffer is already set up by gui_init; grab it so lines
	 * appear on screen without pulling in libpspdebug. */
	void *fb = NULL;
	int w = 0, h = 0;
	(void)fb; (void)w; (void)h;
	g_screen = NULL;
	g_screen_width = SCREEN_WIDTH;
	g_screen_height = SCREEN_HEIGHT;

	SceUID fd = sceIoOpen(LOG_TRACE_FILE,
	                      PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
	if (fd >= 0)
		sceIoClose(fd);
}

void log_printf(const char *fmt, ...)
{
	char line[LOG_LINE_LEN];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);

	int slot;
	if (g_log_count < LOG_MAX_LINES) {
		slot = (g_log_head + g_log_count) % LOG_MAX_LINES;
		g_log_count++;
	} else {
		g_log_head = (g_log_head + 1) % LOG_MAX_LINES;
		slot = (g_log_head + LOG_MAX_LINES - 1) % LOG_MAX_LINES;
	}
	snprintf(g_log[slot], LOG_LINE_LEN, "%s", line);

	SceUID fd = sceIoOpen(LOG_TRACE_FILE,
	                      PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0777);
	if (fd >= 0) {
		sceIoWrite(fd, line, strlen(line));
		sceIoWrite(fd, "\n", 1);
		sceIoClose(fd);
	}
}

void log_clear(void)
{
	g_log_head = 0;
	g_log_count = 0;
}

int log_lines(void)
{
	return g_log_count;
}

const char *log_line(int index)
{
	if (index < 0 || index >= g_log_count)
		return NULL;
	return g_log[(g_log_head + index) % LOG_MAX_LINES];
}
