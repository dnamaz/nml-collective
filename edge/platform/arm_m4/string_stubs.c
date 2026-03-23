/*
 * string_stubs.c — String / memory functions for ARM Cortex-M4 bare metal.
 *
 * Maps to compiler builtins wherever possible so GCC can inline and optimise
 * them (e.g. the M4 LDRD/STRD path for memcpy).  The remaining functions
 * are implemented as simple byte loops — correct and compact, not fastest.
 *
 * When linking against newlib-nano these stubs are superseded by the
 * toolchain's libc — remove this file.
 */

#include <string.h>
#include <stdlib.h>   /* malloc (from alloc.c) */
#include <stddef.h>

/* ── Memory ──────────────────────────────────────────────────────────────── */

void *memcpy(void *dst, const void *src, size_t n)
{
    return __builtin_memcpy(dst, src, n);
}

void *memmove(void *dst, const void *src, size_t n)
{
    return __builtin_memmove(dst, src, n);
}

void *memset(void *dst, int c, size_t n)
{
    return __builtin_memset(dst, c, n);
}

int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *pa = (const unsigned char *)a;
    const unsigned char *pb = (const unsigned char *)b;
    for (size_t i = 0; i < n; i++) {
        if (pa[i] != pb[i]) return (int)pa[i] - (int)pb[i];
    }
    return 0;
}

void *memchr(const void *s, int c, size_t n)
{
    const unsigned char *p = (const unsigned char *)s;
    for (size_t i = 0; i < n; i++) {
        if (p[i] == (unsigned char)c) return (void *)(p + i);
    }
    return NULL;
}

/* ── String length / copy / compare ─────────────────────────────────────── */

size_t strlen(const char *s)
{
    return __builtin_strlen(s);
}

char *strcpy(char *dst, const char *src)
{
    char *d = dst;
    while ((*d++ = *src++));
    return dst;
}

char *strncpy(char *dst, const char *src, size_t n)
{
    size_t i;
    for (i = 0; i < n && src[i]; i++) dst[i] = src[i];
    for (; i < n; i++) dst[i] = '\0';
    return dst;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i]) return (int)(unsigned char)a[i] - (int)(unsigned char)b[i];
        if (!a[i]) break;
    }
    return 0;
}

/* ── Case-insensitive compare (used by nml.c opcode lookup) ─────────────── */

static int to_upper(int c)
{
    return (c >= 'a' && c <= 'z') ? c - 32 : c;
}

int strcasecmp(const char *a, const char *b)
{
    while (*a && to_upper((unsigned char)*a) == to_upper((unsigned char)*b)) {
        a++; b++;
    }
    return to_upper((unsigned char)*a) - to_upper((unsigned char)*b);
}

int strncasecmp(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        int da = to_upper((unsigned char)a[i]);
        int db = to_upper((unsigned char)b[i]);
        if (da != db) return da - db;
        if (!da) break;
    }
    return 0;
}

char *strcat(char *dst, const char *src)
{
    char *d = dst + strlen(dst);
    while ((*d++ = *src++));
    return dst;
}

char *strncat(char *dst, const char *src, size_t n)
{
    char *d = dst + strlen(dst);
    for (size_t i = 0; i < n && src[i]; i++) *d++ = src[i];
    *d = '\0';
    return dst;
}

/* ── Search ──────────────────────────────────────────────────────────────── */

char *strchr(const char *s, int c)
{
    for (; *s; s++) {
        if ((unsigned char)*s == (unsigned char)c) return (char *)s;
    }
    return (c == '\0') ? (char *)s : NULL;
}

char *strrchr(const char *s, int c)
{
    const char *last = NULL;
    for (; *s; s++) {
        if ((unsigned char)*s == (unsigned char)c) last = s;
    }
    return (c == '\0') ? (char *)s : (char *)last;
}

char *strstr(const char *haystack, const char *needle)
{
    if (!*needle) return (char *)haystack;
    size_t nlen = strlen(needle);
    for (; *haystack; haystack++) {
        if (strncmp(haystack, needle, nlen) == 0) return (char *)haystack;
    }
    return NULL;
}

/* strtok — not reentrant; uses a static pointer */
static char *_strtok_ptr = NULL;

char *strtok(char *str, const char *delim)
{
    if (str) _strtok_ptr = str;
    if (!_strtok_ptr) return NULL;

    /* Skip leading delimiters */
    while (*_strtok_ptr && strchr(delim, *_strtok_ptr)) _strtok_ptr++;
    if (!*_strtok_ptr) { _strtok_ptr = NULL; return NULL; }

    char *token = _strtok_ptr;
    while (*_strtok_ptr && !strchr(delim, *_strtok_ptr)) _strtok_ptr++;
    if (*_strtok_ptr) { *_strtok_ptr = '\0'; _strtok_ptr++; }
    else               { _strtok_ptr = NULL; }
    return token;
}

/* strdup — defined in alloc.c (needs malloc) */

/* strerror */
char *strerror(int errnum)
{
    (void)errnum;
    return (char *)"error";
}
