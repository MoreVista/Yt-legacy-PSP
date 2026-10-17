#ifndef YT_UTIL_LOG_H
#define YT_UTIL_LOG_H

#include "../common.h"

void log_init(void);
void log_printf(const char *fmt, ...);
void log_clear(void);
int  log_lines(void);
const char *log_line(int index);

#endif
