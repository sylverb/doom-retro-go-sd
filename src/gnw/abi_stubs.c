//
// abi_stubs.c — resolve the firmware surface through the ABI table.
//
// Thin wrappers call through gw_firmware_abi() (VTOR+0x400). Historical
// names stay for engine code; audio/LCD/input use the direct ABI slots
// (ctl folding was reverted in firmware).
//
#include "rg_abi.h"
#include "gw_lcd.h"
#include "gw_audio.h"
#include "odroid_overlay.h"
#include "common.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

#define A (gw_firmware_abi())

/* --- string / memory (engine freestanding; may also satisfy newlib decls) --- */
int    memcmp(const void *a, const void *b, size_t n)        { return A->memcmp(a, b, n); }
void  *memmove(void *d, const void *s, size_t n)             { return A->memmove(d, s, n); }
size_t strlen(const char *s)                                 { return A->strlen(s); }
char  *strncpy(char *d, const char *s, size_t n)             { return A->strncpy(d, s, n); }
int    strcmp(const char *a, const char *b)                  { return A->strcmp(a, b); }
int    strncmp(const char *a, const char *b, size_t n)       { return A->strncmp(a, b, n); }
char  *strstr(const char *h, const char *n)                  { return A->strstr(h, n); }

int    tolower(int c)                                        { return A->tolower(c); }
int    toupper(int c)                                        { return A->toupper(c); }

/* --- audio ------------------------------------------------------------------- */
void audio_start_playing(uint16_t len)
{
    A->audio_start_playing(len);
}
int16_t *audio_get_active_buffer(void)
{
    return A->audio_get_active_buffer();
}
void audio_clear_active_buffer(void)
{
    A->audio_clear_active_buffer();
}
void audio_clear_inactive_buffer(void)
{
    A->audio_clear_inactive_buffer();
}
void odroid_audio_mute(bool mute)
{
    A->odroid_audio_mute(mute);
}

/* --- DTCM bump: doom still spells dtcm_*; ABI pool is GW_MEM_DTC ------------- */
void dtcm_init(void)
{
    if (!A->mem_ctl)
        return;
    (void)A->mem_ctl(GW_MEM_OP_INIT, GW_MEM_DTC, 0, 0);
}

void *dtcm_malloc(unsigned long size)
{
    if (!A->mem_ctl)
        return (void *)0;
    return (void *)A->mem_ctl(GW_MEM_OP_ALLOC, GW_MEM_DTC, 1, (size_t)size);
}

uint8_t common_emu_sound_get_volume(void)
{
    return A->common_emu_sound_get_volume ? A->common_emu_sound_get_volume() : 255;
}

void common_ingame_overlay(void)
{
    A->common_ingame_overlay();
}

void common_emu_input_loop(odroid_gamepad_state_t *js,
                           odroid_dialog_choice_t *game_options,
                           void_callback_t repaint)
{
    A->common_emu_input_loop(js, game_options, repaint);
}

void odroid_input_read_gamepad(odroid_gamepad_state_t *out)
{
    A->wdog_refresh();
    A->odroid_input_read_gamepad(out);
}

void odroid_system_switch_app(int app)
{
    A->odroid_system_switch_app(app);
}

FILE *fopen(const char *path, const char *mode)
{
    return A->fopen(path, mode);
}
int fclose(FILE *stream)
{
    return A->fclose(stream);
}
size_t fread(void *ptr, size_t size, size_t n, FILE *stream)
{
    return A->fread(ptr, size, n, stream);
}
size_t fwrite(const void *ptr, size_t size, size_t n, FILE *stream)
{
    return A->fwrite(ptr, size, n, stream);
}
int remove(const char *path)
{
    return A->remove(path);
}

/* --- LCD --------------------------------------------------------------------- */
void lcd_set_clut(const uint32_t *clut, uint16_t count)
{
    A->lcd_set_clut(clut, count);
}
void lcd_setup_framebuffers(lcd_mode_t lcd_mode)
{
    A->lcd_setup_framebuffers((int)lcd_mode);
}
void *lcd_get_active_buffer(void)
{
    return A->lcd_get_active_buffer();
}
void *lcd_get_inactive_buffer(void)
{
    return A->lcd_get_inactive_buffer();
}
void lcd_swap(void)
{
    A->wdog_refresh();
    A->lcd_swap();
}

void odroid_system_emu_init(state_handler_t load_cb, state_handler_t save_cb,
                            screenshot_handler_t screenshot_cb,
                            shutdown_handler_t shutdown_cb,
                            sleep_post_wakeup_handler_t sleep_post_wakeup_cb,
                            sram_save_handler_t sram_save_cb,
                            cheat_update_handler_t cheat_update_cb)
{
    A->odroid_system_emu_init(load_cb, save_cb, screenshot_cb, shutdown_cb,
                              sleep_post_wakeup_cb, sram_save_cb, cheat_update_cb);
}

uint8_t *odroid_overlay_cache_file_in_flash(const char *path, uint32_t *size_p,
                                            bool byte_swap)
{
    return A->odroid_overlay_cache_file_in_flash_relocate(
        path, size_p, byte_swap, (gw_flash_relocate_cb_t)0);
}
