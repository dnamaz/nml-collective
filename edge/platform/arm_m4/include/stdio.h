/*
 * Minimal stdio.h for NML Edge Worker — ARM Cortex-M4 bare metal.
 *
 * printf / fprintf / snprintf / sprintf are implemented in platform/arm_m4/printf.c.
 * printf() and fprintf(stdout/stderr, ...) both route through edge_putchar(),
 * a weak symbol the application overrides to route to UART / RTT / semihosting.
 *
 * File I/O (fopen, fread, …) is stubbed out — there is no filesystem on the
 * target.  nml.c calls vm_load_data_from_string(), not vm_load_data_from_file(),
 * so the stubs are never reached in normal use.
 *
 * When using gcc-arm-embedded + newlib-nano, remove this file.
 */
#ifndef _EDGE_STDIO_H
#define _EDGE_STDIO_H

#include <stddef.h>
#include <stdarg.h>

/* ── Output hook ─────────────────────────────────────────────────────────── */

/*
 * Override this in your BSP / HAL to route characters to UART, RTT, etc.
 *   void edge_putchar(char c) { USART1->DR = (uint8_t)c; }
 */
void edge_putchar(char c);

/* ── FILE stub ───────────────────────────────────────────────────────────── */

typedef struct { int _dummy; } FILE;

extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

/* All file ops return failure — no filesystem on bare metal */
FILE  *fopen(const char *path, const char *mode);
int    fclose(FILE *f);
size_t fread(void *buf, size_t sz, size_t n, FILE *f);
size_t fwrite(const void *buf, size_t sz, size_t n, FILE *f);
int    fseek(FILE *f, long offset, int whence);
long   ftell(FILE *f);
void   rewind(FILE *f);
int    feof(FILE *f);
char  *fgets(char *s, int n, FILE *f);
int    fputs(const char *s, FILE *f);
int    fputc(int c, FILE *f);
int    fflush(FILE *f);

/* ── Formatted I/O ───────────────────────────────────────────────────────── */

int printf(const char *fmt, ...)
    __attribute__((format(printf, 1, 2)));

int fprintf(FILE *f, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

int sprintf(char *buf, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

int snprintf(char *buf, size_t sz, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

int vprintf(const char *fmt, va_list ap);
int vfprintf(FILE *f, const char *fmt, va_list ap);
int vsprintf(char *buf, const char *fmt, va_list ap);
int vsnprintf(char *buf, size_t sz, const char *fmt, va_list ap);

int sscanf(const char *buf, const char *fmt, ...)
    __attribute__((format(scanf, 2, 3)));

int scanf(const char *fmt, ...)
    __attribute__((format(scanf, 1, 2)));

/* ── Character I/O ───────────────────────────────────────────────────────── */

int putchar(int c);
int getchar(void);    /* always returns EOF on bare metal */
int puts(const char *s);

#define EOF (-1)

#endif /* _EDGE_STDIO_H */
