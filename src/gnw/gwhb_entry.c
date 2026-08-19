//
// gwhb_entry.c — stage-1 copier of the CORE RAM overlay.
//
// The firmware copies the payload to __RAM_EMU_START__ and jumps here.
// We unpack ITCM + AXI (.data / .text_axis) segments, then enter doom_start.
//
// AHB is the firmware malloc heap — we never copy into it.
//
// ACTIVE_FILE lives on the firmware AHB heap (calloc in emulator_start) and
// remains valid after stage-1 (we no longer smash AHB). Still snapshot into
// a durable .core_romfile buffer and reinstall: pause/save UX and any later
// AHB churn must keep a stable romPath.
//
#include <stdint.h>

#include "rg_abi.h"

extern uint32_t __itcram_hot_start__[], __itcram_hot_end__[], __itcram_hot_lma[];
extern uint32_t _doom_data_vma_start[], _doom_data_vma_end[], _doom_data_lma[];
extern uint32_t __text_axis_start__[], __text_axis_end__[], __text_axis_lma[];

extern void doom_start(uint8_t load_state, uint8_t start_paused, int8_t save_slot,
                       const char *whd_path);

/* Prefix of retro_emulator_file_t (COVERFLOW/CHEAT fields follow `size` —
 * we zero-pad a generous tail so firmware COVERFLOW=1 builds stay safe). */
typedef struct {
    char name[256];
    const char *ext;
    char path[256];
    uint8_t *address;
    uint32_t size;
    /* COVERFLOW/CHEAT tail — zeroed so COVERFLOW=1 firmware reads NULLs. */
    uint32_t pad[16];
} doom_active_file_t;

/* Lives in the CORE image (BLOB), not AHB / AXIBSS. */
__attribute__((section(".core_romfile"), used))
static doom_active_file_t g_active_file;
/* Non-const: same section as g_active_file (avoids section type conflict). */
__attribute__((section(".core_romfile"), used))
static char g_ext_whd[] = "whd";

static inline void copy_words(uint32_t *dst, const uint32_t *end, const uint32_t *src)
{
    while (dst < end)
        *dst++ = *src++;
}

static void copy_str(char *dst, unsigned dst_sz, const char *src)
{
    unsigned i = 0;
    if (!src)
        src = "";
    while (i + 1 < dst_sz && src[i]) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

static void snapshot_active_file(void)
{
    unsigned i;
    uint8_t *raw = (uint8_t *)&g_active_file;
    for (i = 0; i < sizeof(g_active_file); i++)
        raw[i] = 0;

    if (!gnw_abi_ok() || !gnw_abi()->ACTIVE_FILE_ptr)
        return;

    doom_active_file_t *src = *(doom_active_file_t **)gnw_abi()->ACTIVE_FILE_ptr;
    if (!src || !src->path[0])
        return;

    copy_str(g_active_file.name, sizeof(g_active_file.name), src->name);
    copy_str(g_active_file.path, sizeof(g_active_file.path), src->path);
    g_active_file.address = src->address;
    g_active_file.size = src->size;
    g_active_file.ext = g_ext_whd;
    for (i = 0; g_active_file.path[i]; i++) {
        if (g_active_file.path[i] == '.' && g_active_file.path[i + 1])
            g_active_file.ext = &g_active_file.path[i + 1];
    }
}

static void install_active_file(void)
{
    if (!gnw_abi()->ACTIVE_FILE_ptr || !g_active_file.path[0])
        return;
    *(doom_active_file_t **)gnw_abi()->ACTIVE_FILE_ptr = &g_active_file;
}

/* CORE entry: must be first word of the payload (linker .core_entry). */
__attribute__((section(".gwhb_entry"), used, noreturn))
void app_main_doom(uint8_t load_state, uint8_t start_paused, int8_t save_slot)
{
    snapshot_active_file();

    copy_words(__itcram_hot_start__, __itcram_hot_end__, __itcram_hot_lma);
    copy_words(_doom_data_vma_start, _doom_data_vma_end, _doom_data_lma);
    copy_words(__text_axis_start__, __text_axis_end__, __text_axis_lma);

    __asm__ volatile ("dsb");
    for (uintptr_t a = (uintptr_t)__text_axis_start__ & ~31u;
         a < (uintptr_t)__text_axis_end__; a += 32)
        *(volatile uint32_t *)0xE000EF68 = a;
    __asm__ volatile ("dsb");
    *(volatile uint32_t *)0xE000EF50 = 0;
    __asm__ volatile ("dsb; isb");

    install_active_file();

    doom_start(load_state, start_paused, save_slot,
               g_active_file.path[0] ? g_active_file.path : (const char *)0);
    for (;;) {
        if (gnw_abi_ok() && gnw_abi()->wdog_refresh)
            gnw_abi()->wdog_refresh();
    }
}
