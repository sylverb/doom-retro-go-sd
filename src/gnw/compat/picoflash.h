#pragma once
#include "pico.h"
// Flash savegame backend. The G&W OSPI driver is read-only/XIP today, so the
// implementation (stm32/flash_stub.c) is a no-op that reports no free space;
// g_game.c handles the failure path. Real saves are a retro-go-phase feature.
void picoflash_sector_program(uint32_t flash_offs, const uint8_t *data);

// pico-sdk hardware/flash.h geometry (p_saveg.c sector math)
#ifndef FLASH_SECTOR_SIZE
#define FLASH_SECTOR_SIZE 4096u
#endif
