//
// gnw_libc.c — the slice of libc that retro-go's ABI does NOT provide, so gnw-doom
// carries its own. Everything retro-go DOES provide (memcpy/memset/strlen/strcmp/
// strncmp/strncpy/tolower/toupper/strstr/free/realloc-of-its-own-heap/...) still
// comes through the ABI (abi_stubs.c); this file is only the gaps the compat gate
// flags:  printf/snprintf formatting, a few string/math helpers, sscanf, and the
// allocator — routed to the engine's own zone, never the firmware heap.
//
#include <stddef.h>
#include <stdarg.h>
#include <string.h>     // memcpy / memset / strlen (these ARE retro-go-provided)

#include "z_zone.h"     // Z_MallocNoUser / Z_Free + PU_STATIC

// --- allocator: gnw-doom's zone, not the firmware heap ----------------------
// Doom's malloc users (WAD directory, OPL track data, m_misc string buffers) all
// run after Z_Init and free explicitly, which the zone handles. realloc copies the
// new size; doom only ever grows, and the tail it over-reads is immediately
// overwritten by the caller (and stays inside the zone span).
void *malloc(size_t n) { return Z_MallocNoUser((int)n, PU_STATIC); }

void free(void *p) { if (p) Z_Free(p); }

void *calloc(size_t n, size_t s)
{
    size_t t = n * s;
    void *p = Z_MallocNoUser((int)t, PU_STATIC);
    if (p) memset(p, 0, t);
    return p;
}

void *realloc(void *p, size_t n)
{
    if (!p) return malloc(n);
    if (!n) { Z_Free(p); return NULL; }
    void *q = Z_MallocNoUser((int)n, PU_STATIC);
    if (q) { memcpy(q, p, n); Z_Free(p); }
    return q;
}

// --- string / math ----------------------------------------------------------
int abs(int x) { return x < 0 ? -x : x; }

char *strcpy(char *d, const char *s) { char *r = d; while ((*d++ = *s++)); return r; }

size_t strcspn(const char *s, const char *reject)
{
    size_t n = 0;
    for (; s[n]; n++)
        for (const char *r = reject; *r; r++)
            if (s[n] == *r) return n;
    return n;
}

char *strdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

// --- stdio formatting -------------------------------------------------------
// vendored from the toolkit firmware's libc (proven on doom's exact format strings:
// %s %c %d %i %u %x %p, with width/precision/zero-pad). A core has no output sink,
// so printf/vprintf format nothing (retro-go logs its own way); snprintf does.
int vsnprintf(char *dst, unsigned long size, const char *fmt, va_list arg)
{
    size_t cnt = 0;
    for (; *fmt; fmt++) {
        if (cnt == size) goto DONE;
        if (*fmt != '%') { dst[cnt++] = *fmt; continue; }
        fmt++;

        int zeropad = 0, width = 0, prec = -1;
        if (*fmt == '0') { zeropad = 1; fmt++; }
        while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
        if (*fmt == '.') {
            fmt++; prec = 0;
            while (*fmt >= '0' && *fmt <= '9') prec = prec * 10 + (*fmt++ - '0');
        }

        switch (*fmt) {
        case '\0': goto DONE;
        case 's': {
            const char *s = va_arg(arg, const char *);
            while (*s && cnt < size) dst[cnt++] = *s++;
            if (cnt == size) goto DONE;
            break;
        }
        case 'c': { int c = va_arg(arg, int); dst[cnt++] = c; break; }
        case 'i': case 'd': case 'u': {
            unsigned int x;
            if (*fmt == 'u') x = va_arg(arg, unsigned int);
            else {
                int sx = va_arg(arg, int);
                if (sx < 0) { if (cnt < size) dst[cnt++] = '-'; sx = -sx; }
                x = (unsigned int)sx;
            }
            char buf[12]; int i = 0;
            do { buf[i++] = (x % 10) + '0'; x /= 10; } while (x > 0);
            int mindigits = prec >= 0 ? prec : (zeropad ? width : 0);
            while (i < mindigits && i < (int)sizeof(buf)) buf[i++] = '0';
            while (i && cnt < size) dst[cnt++] = buf[--i];
            break;
        }
        case 'p': case 'x': {
            unsigned int z = va_arg(arg, unsigned int);
            for (int i = 7; i >= 0 && cnt < size; i--)
                dst[cnt++] = "0123456789abcdef"[(z >> (i << 2)) & 0xf];
            break;
        }
        }
    }
DONE:
    if (size) dst[(cnt < size) ? cnt : size - 1] = 0;
    return (int)cnt;
}

// snprintf returns the character count (standard C; the engine relies on it,
// e.g. `n = snprintf(...)`). printf/vprintf stay void to match the ABI's
// stdio.h, which never exposed a return value for them.
int snprintf(char *dst, unsigned long size, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(dst, size, fmt, ap);
    va_end(ap);
    return n;
}

/* Route to the firmware's vprintf (retro-go: persistent logbuf, shown on the
 * BSOD screen — an assert message that vanishes helps nobody). */
#include "rg_abi.h"
void vprintf(const char *fmt, va_list ap)
{
    if (gnw_abi_ok() && gnw_abi()->vprintf)
        gnw_abi()->vprintf(fmt, ap);
}
void printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
}

// retro-go (and the toolkit firmware) never implemented sscanf; keep it a no-op so
// callers fall back to defaults exactly as before.
int sscanf(const char *str, const char *format, ...) { (void)str; (void)format; return 0; }
