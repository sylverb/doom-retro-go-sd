/* Freestanding ctype — ASCII-only, no newlib _ctype_ table. */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

static inline int isupper(int c) { return (c >= 'A' && c <= 'Z'); }
static inline int islower(int c) { return (c >= 'a' && c <= 'z'); }
static inline int isalpha(int c) { return isupper(c) || islower(c); }
static inline int isdigit(int c) { return (c >= '0' && c <= '9'); }
static inline int isalnum(int c) { return isalpha(c) || isdigit(c); }
static inline int isspace(int c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}
static inline int isprint(int c) { return c >= 0x20 && c <= 0x7e; }
static inline int isgraph(int c) { return c > 0x20 && c <= 0x7e; }
static inline int iscntrl(int c) { return (c >= 0 && c < 0x20) || c == 0x7f; }
static inline int ispunct(int c) { return isgraph(c) && !isalnum(c); }
static inline int isxdigit(int c)
{
    return isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

/* ABI-backed in abi_stubs.c when linked; declare for headers that need them. */
int tolower(int c);
int toupper(int c);

#ifdef __cplusplus
}
#endif
