#pragma once
/* Freestanding stdio shim for gnw-doom (ABI-backed I/O via abi_stubs.c). */
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void putchar_(char c);
void printf(const char *fmt, ...);
void vprintf(const char *fmt, va_list arg);
int snprintf(char *dst, unsigned long size, const char *fmt, ...);
int sprintf(char *dst, const char *fmt, ...);
int vsnprintf(char *dst, unsigned long size, const char *fmt, va_list arg);
int vsprintf(char *dst, const char *fmt, va_list arg);
int sscanf(const char *str, const char *format, ...);

#define fprintf(ignore, fmt, ...) printf(fmt, ##__VA_ARGS__)

#define FILE void
#define stdout NULL
#define stderr NULL
#define SEEK_END 0
#define SEEK_SET 0

FILE *fopen(const char *filename, const char *mode);
int fclose(FILE *fp);
long ftell(FILE *fp);
int fseek(FILE *fp, long offset, int whence);
size_t fread(void *ptr, size_t size, size_t nmemb, FILE *fp);
size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *fp);
int remove(const char *pathname);
int rename(const char *oldpath, const char *newpath);
int vfprintf(FILE *stream, const char *format, va_list ap);
int fflush(FILE *stream);
int puts(const char *s);
int fputc(int c, FILE *stream);
char *fgets(char *s, int size, FILE *stream);
int fputs(const char *s, FILE *stream);

#ifdef __cplusplus
}
#endif
