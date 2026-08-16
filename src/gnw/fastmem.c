//
// Word/unrolled memcpy + memset for the payload. The firmware's libc
// (src/libc.c) implements these as byte loops; with -ffreestanding gcc
// emits real calls for every large copy, so compose (141 KB/frame), the
// per-frame render memsets and the wipes all paid ~8-9 cycles per byte.
// Payload-defined symbols win the link: -Wl,--just-symbols only resolves
// what is still undefined.
//
// Functions live in ITCM (hottest code in the system by call volume).
//

#include <stddef.h>
#include <stdint.h>

#include "pico.h"

void *__not_in_flash_func(memcpy)(void *dst, const void *src, size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;

    // align destination to 4 (most callers are already aligned)
    while (n && ((uintptr_t)d & 3u)) {
        *d++ = *s++;
        n--;
    }

    if (((uintptr_t)s & 3u) == 0) {
        uint32_t *dw = (uint32_t *)d;
        const uint32_t *sw = (const uint32_t *)s;
        while (n >= 32) {
            dw[0] = sw[0]; dw[1] = sw[1]; dw[2] = sw[2]; dw[3] = sw[3];
            dw[4] = sw[4]; dw[5] = sw[5]; dw[6] = sw[6]; dw[7] = sw[7];
            dw += 8; sw += 8; n -= 32;
        }
        while (n >= 4) {
            *dw++ = *sw++;
            n -= 4;
        }
        d = (unsigned char *)dw;
        s = (const unsigned char *)sw;
    }
    while (n--)
        *d++ = *s++;
    return dst;
}

void *__not_in_flash_func(memset)(void *dst, int c, size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    uint32_t v = (uint8_t)c;
    v |= v << 8;
    v |= v << 16;

    while (n && ((uintptr_t)d & 3u)) {
        *d++ = (unsigned char)c;
        n--;
    }
    uint32_t *dw = (uint32_t *)d;
    while (n >= 32) {
        dw[0] = v; dw[1] = v; dw[2] = v; dw[3] = v;
        dw[4] = v; dw[5] = v; dw[6] = v; dw[7] = v;
        dw += 8; n -= 32;
    }
    while (n >= 4) {
        *dw++ = v;
        n -= 4;
    }
    d = (unsigned char *)dw;
    while (n--)
        *d++ = (unsigned char)c;
    return dst;
}
