//
// gnw_libc.c — libc that the SDK bridge must NOT provide.
//
// malloc/calloc/free/realloc → engine zone (not firmware heap).
// objcopy --redefine-syms renames them to core_*; the bridge is built with
// GW_CORE_BRIDGE_DISABLE_SDK_MALLOC so it does not define core_malloc/….
// Everything else (stdio, strcmp, lcd_*, fopen, …) comes from gw_core_bridge.c.
//
#include <stddef.h>
#include <string.h>

#include "z_zone.h"     // Z_MallocNoUser / Z_Free + PU_STATIC

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

int abs(int x) { return x < 0 ? -x : x; }
