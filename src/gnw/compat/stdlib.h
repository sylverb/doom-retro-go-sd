#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void *malloc(size_t size);
void *calloc(size_t nmemb, size_t size);
void *realloc(void *ptr, size_t size);
void free(void *ptr);

int abs(int j);

int system(const char *command);

int rand(void);
void srand(unsigned int seed);
#define RAND_MAX 0x7fff

void qsort(void *base, size_t nmemb, size_t size,
           int (*compar)(const void *, const void *));

void *memset(void *s, int c, size_t n);
void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);

void exit(int err);

#ifdef __cplusplus
}
#endif
