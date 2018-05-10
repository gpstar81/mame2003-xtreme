#ifndef LOG_H
#define LOG_H

#include <libretro.h>

/******************************************************************************

	Shared libretro log interface
    set in mame2003.c 

******************************************************************************/

#define LOGPRE          "[MAME Xtreme] "

extern retro_log_printf_t log_cb;

#endif /* LOG_H */