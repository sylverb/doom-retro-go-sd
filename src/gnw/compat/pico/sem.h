#pragma once
#include "pico.h"

// Single-core stand-in for pico semaphores. Everything that matters has been
// flattened to direct calls in pd_render.cpp (DOOMX_SINGLE_CORE); this keeps
// any residual uses compiling and behaving sanely on one core.
typedef struct {
    volatile int count;
    int max;
} semaphore_t;

static inline void sem_init(semaphore_t *s, int initial, int max) {
    s->count = initial;
    s->max = max;
}
static inline void sem_release(semaphore_t *s) {
    if (s->count < s->max) s->count++;
}
static inline bool sem_available(semaphore_t *s) {
    return s->count > 0;
}
static inline void sem_acquire_blocking(semaphore_t *s) {
    // Single core: a blocked acquire could never be released. Treat as
    // consume-if-available; flattened code paths never block here.
    if (s->count > 0) s->count--;
}
static inline bool sem_try_acquire(semaphore_t *s) {
    if (s->count > 0) { s->count--; return true; }
    return false;
}
