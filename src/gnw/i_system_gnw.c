//
// GNW system layer: replaces src/pico/i_system.c.
// Zone memory comes from linker symbols (one contiguous AXISRAM span);
// quit enters STM32 Standby via power_off() (ported from doomgeneric).
//

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "doomtype.h"
#include "i_system.h"
#include "i_sound.h"
#include "i_timer.h"
#include "i_video.h"
#include "m_misc.h"

// Zone span placed by linker.ld in AXISRAM after .bss.
extern unsigned long _zone_start, _zone_end;

void I_Tactile(int on, int off, int total)
{
}

byte *I_ZoneBase(int *size)
{
    byte *zonemem = (byte *)&_zone_start;
    *size = (int)((uintptr_t)&_zone_end - (uintptr_t)&_zone_start);
    printf("zone memory: %p, %x allocated for zone\n", zonemem, *size);
    return zonemem;
}

// I_AtExit / exit handling: NO_USE_EXIT is set; keep the registry so
// I_Quit can run cleanups that opt in.
typedef struct atexit_listentry_s atexit_listentry_t;
struct atexit_listentry_s
{
    atexit_func_t func;
    boolean run_on_error;
    atexit_listentry_t *next;
};

static atexit_listentry_t *exit_funcs = NULL;
#define MAX_ATEXIT 16
static atexit_listentry_t exit_entries[MAX_ATEXIT];
static int num_exit_funcs;

void I_AtExit(atexit_func_t func, boolean run_on_error)
{
    if (num_exit_funcs >= MAX_ATEXIT)
        return;
    atexit_listentry_t *entry = &exit_entries[num_exit_funcs++];
    entry->func = func;
    entry->run_on_error = run_on_error;
    entry->next = exit_funcs;
    exit_funcs = entry;
}

extern void I_InputInit(void);
extern void doom_persist_init(void);

void I_Init(void)
{
    I_InputInit();
    doom_persist_init();   // register retro-go persistence callbacks, load SRAM
}

void I_BindVariables(void)
{
}

void I_PrintBanner(const char *msg)
{
    printf("%s\n", msg);
}

void I_PrintDivider(void)
{
    printf("===========================================================================\n");
}

void I_PrintStartupBanner(const char *gamedescription)
{
    I_PrintDivider();
    printf("%s\n", gamedescription);
    I_PrintDivider();
}

// Quit is firmware-owned (retro-go model): the core leaves via
// odroid_system_switch_app (-> launcher on real retro-go; -> Standby on this
// launcher-less toolkit). The raw PWR/SCB Standby sequence + the POWER+PAUSE
// trigger live in the firmware (input.c power_off).
extern void odroid_system_switch_app(int app);   // "quit"; does not return

extern void I_StopSong(void);

void I_Quit(void)
{
    atexit_listentry_t *entry = exit_funcs;
    while (entry != NULL)
    {
        entry->func();
        entry = entry->next;
    }
    I_StopSong();
    odroid_system_switch_app(0);
    for (;;) {} // unreachable
}

void I_Error(const char *error, ...)
{
    va_list argptr;
    va_start(argptr, error);
    printf("\nError: ");
    vprintf(error, argptr);
    printf("\n");
    va_end(argptr);
    __asm__ volatile ("bkpt #0");
    for (;;) {}
}

void *I_Realloc(void *ptr, size_t size)
{
    void *new_ptr = realloc(ptr, size);   // firmware libc heap

    if (size != 0 && new_ptr == NULL)
    {
        I_Error("I_Realloc: failed on reallocation of %d bytes", (int)size);
    }
    return new_ptr;
}

boolean I_GetMemoryValue(unsigned int offset, void *value, int size)
{
    // Emulates reading DOS conventional memory for vanilla quirks; zeros do.
    memset(value, 0, size);
    return true;
}

// newlib assert() lands here; asserts stay enabled for bring-up (PD_SANITY).
void __assert_func(const char *file, int line, const char *func, const char *expr)
{
    printf("\nASSERT %s:%d (%s): %s\n", file, line, func ? func : "?", expr);
    __asm__ volatile ("bkpt #0");
    for (;;) {}
}
