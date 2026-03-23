/*
 * Minimal stdlib.h for NML Edge Worker — ARM Cortex-M4 bare metal.
 * malloc/calloc/realloc/free are provided by platform/arm_m4/alloc.c.
 * When using gcc-arm-embedded + newlib-nano, remove this file and let
 * the toolchain's stdlib.h take over.
 */
#ifndef _EDGE_STDLIB_H
#define _EDGE_STDLIB_H

#include <stddef.h>

void  *malloc(size_t size);
void  *calloc(size_t nmemb, size_t size);
void  *realloc(void *ptr, size_t size);
void   free(void *ptr);

int    atoi(const char *str);
long   atol(const char *str);
double atof(const char *str);
double strtod(const char *str, char **endptr);
long   strtol(const char *str, char **endptr, int base);

#define RAND_MAX 0x7fffffff

void   srand(unsigned int seed);
int    rand(void);

void   abort(void) __attribute__((noreturn));
void   exit(int status) __attribute__((noreturn));

char  *strdup(const char *s);  /* uses malloc */

#endif /* _EDGE_STDLIB_H */
