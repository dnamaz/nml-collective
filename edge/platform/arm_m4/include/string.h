/*
 * Minimal string.h for NML Edge Worker — ARM Cortex-M4 bare metal.
 * Implementations come from the compiler's built-in __builtin_* functions
 * (via the thin wrappers in platform/arm_m4/string_stubs.c) or from the
 * application's libc when linking against newlib/newlib-nano.
 */
#ifndef _EDGE_STRING_H
#define _EDGE_STRING_H

#include <stddef.h>

/* Memory */
void  *memcpy(void *dst, const void *src, size_t n);
void  *memmove(void *dst, const void *src, size_t n);
void  *memset(void *dst, int c, size_t n);
int    memcmp(const void *a, const void *b, size_t n);
void  *memchr(const void *s, int c, size_t n);

/* String length / copy / compare */
size_t strlen(const char *s);
char  *strcpy(char *dst, const char *src);
char  *strncpy(char *dst, const char *src, size_t n);
char  *strcat(char *dst, const char *src);
char  *strncat(char *dst, const char *src, size_t n);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);

/* Search */
char  *strchr(const char *s, int c);
char  *strrchr(const char *s, int c);
char  *strstr(const char *haystack, const char *needle);
char  *strtok(char *str, const char *delim);

/* Allocation (uses malloc from stdlib.h) */
char  *strdup(const char *s);

/* Error string — returns static "error" on bare metal */
char  *strerror(int errnum);

/* Locale-independent case conversion (from strings.h family) — used by nml.c */
int    strcasecmp(const char *a, const char *b);
int    strncasecmp(const char *a, const char *b, size_t n);

#endif /* _EDGE_STRING_H */
