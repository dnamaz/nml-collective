/*
 * printf.c — Minimal formatted output for NML Edge Worker on ARM Cortex-M4.
 *
 * Supported format specifiers:
 *   %d / %i   — signed decimal int
 *   %u        — unsigned decimal
 *   %x / %X  — lowercase / uppercase hex
 *   %o        — octal
 *   %f / %F  — double, fixed notation  (6 decimal places by default)
 *   %e / %E  — double, exponential notation
 *   %g / %G  — %f or %e, whichever is shorter (naive: always %f here)
 *   %s        — null-terminated string  ("%s" of NULL → "(null)")
 *   %c        — single character
 *   %p        — pointer as hex
 *   %%        — literal percent
 *
 * Width and precision modifiers are honoured.
 * Long modifier 'l' is accepted and ignored (int == long on M4 with -mthumb).
 *
 * All output routes through edge_putchar() — a weak symbol that the
 * application overrides:
 *
 *   void edge_putchar(char c) { USART1->DR = (uint8_t)c; }
 *
 * FILE* is ignored in fprintf() — all output goes to the same sink.
 * sscanf() supports %d, %u, %f, %s (space-separated tokens).
 */

#include <stdio.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

/* ── Output hook ─────────────────────────────────────────────────────────── */

__attribute__((weak)) void edge_putchar(char c) { (void)c; }

/* Stub FILE objects — address equality is used by fprintf */
static FILE _stdout_obj = {0};
static FILE _stderr_obj = {0};
FILE *stdout = &_stdout_obj;
FILE *stderr = &_stderr_obj;
FILE *stdin  = NULL;

/* ── Internal: write one char to a buffer or to the output hook ──────────── */

typedef struct {
    char   *buf;    /* NULL = stream mode (edge_putchar) */
    size_t  pos;
    size_t  cap;    /* 0 = unlimited (vsprintf) */
} Sink;

static void sink_put(Sink *s, char c)
{
    if (s->buf) {
        if (s->cap == 0 || s->pos < s->cap - 1) {
            s->buf[s->pos] = c;
        }
        s->pos++;
    } else {
        edge_putchar(c);
        s->pos++;
    }
}

static void sink_str(Sink *s, const char *p, int width, int left, char pad)
{
    int len = 0;
    const char *q = p ? p : "(null)";
    const char *t = q;
    while (*t++) len++;

    if (!left) {
        for (int i = len; i < width; i++) sink_put(s, pad);
    }
    while (*q) sink_put(s, *q++);
    if (left) {
        for (int i = len; i < width; i++) sink_put(s, ' ');
    }
}

/* Integer to string (reversed, then flipped) */
static int int_to_str(char *buf, unsigned long long val, int base, int upper)
{
    static const char *lo = "0123456789abcdef";
    static const char *hi = "0123456789ABCDEF";
    const char *digits = upper ? hi : lo;
    int len = 0;
    if (val == 0) { buf[len++] = '0'; }
    else {
        while (val) {
            buf[len++] = digits[val % (unsigned)base];
            val /= (unsigned)base;
        }
    }
    /* Reverse */
    for (int i = 0, j = len - 1; i < j; i++, j--) {
        char tmp = buf[i]; buf[i] = buf[j]; buf[j] = tmp;
    }
    buf[len] = '\0';
    return len;
}

/* Very small dtoa for %f: writes sign + digits to buf (no exp notation) */
static int double_to_str(char *buf, double v, int prec)
{
    int pos = 0;
    if (v < 0.0) { buf[pos++] = '-'; v = -v; }

    /* Round to prec decimal places */
    double round_add = 0.5;
    for (int i = 0; i < prec; i++) round_add *= 0.1;
    v += round_add;

    /* Integer part */
    unsigned long long ipart = (unsigned long long)v;
    char ibuf[24];
    int ilen = int_to_str(ibuf, ipart, 10, 0);
    for (int i = 0; i < ilen; i++) buf[pos++] = ibuf[i];

    if (prec > 0) {
        buf[pos++] = '.';
        double frac = v - (double)ipart;
        for (int i = 0; i < prec; i++) {
            frac *= 10.0;
            int d = (int)frac;
            buf[pos++] = (char)('0' + d);
            frac -= d;
        }
    }
    buf[pos] = '\0';
    return pos;
}

/* Exponential notation for %e */
static int double_to_exp_str(char *buf, double v, int prec, int upper)
{
    int pos = 0;
    if (v < 0.0) { buf[pos++] = '-'; v = -v; }

    int exp = 0;
    if (v != 0.0) {
        /* Normalise to [1, 10) */
        while (v >= 10.0)  { v /= 10.0; exp++; }
        while (v <  1.0)   { v *= 10.0; exp--; }
    }

    /* Mantissa as fixed with prec digits */
    double round_add = 0.5;
    for (int i = 0; i < prec; i++) round_add *= 0.1;
    v += round_add;
    if (v >= 10.0) { v /= 10.0; exp++; }

    /* Integer digit */
    int d = (int)v;
    buf[pos++] = (char)('0' + d);
    v -= d;
    if (prec > 0) {
        buf[pos++] = '.';
        for (int i = 0; i < prec; i++) {
            v *= 10.0;
            d = (int)v;
            buf[pos++] = (char)('0' + d);
            v -= d;
        }
    }

    buf[pos++] = upper ? 'E' : 'e';
    buf[pos++] = exp >= 0 ? '+' : '-';
    if (exp < 0) exp = -exp;
    if (exp >= 100) { buf[pos++] = (char)('0' + exp / 100); exp %= 100; }
    buf[pos++] = (char)('0' + exp / 10);
    buf[pos++] = (char)('0' + exp % 10);
    buf[pos] = '\0';
    return pos;
}

/* ── Core vsnprintf ──────────────────────────────────────────────────────── */

static int vsink(Sink *s, const char *fmt, va_list ap)
{
    char tmp[64];

    for (; *fmt; fmt++) {
        if (*fmt != '%') { sink_put(s, *fmt); continue; }
        fmt++;

        /* Flags */
        int left = 0, zero_pad = 0, plus = 0;
        while (*fmt == '-' || *fmt == '0' || *fmt == '+') {
            if (*fmt == '-') left = 1;
            if (*fmt == '0') zero_pad = 1;
            if (*fmt == '+') plus = 1;
            fmt++;
        }

        /* Width */
        int width = 0;
        while (*fmt >= '0' && *fmt <= '9') { width = width * 10 + (*fmt - '0'); fmt++; }

        /* Precision */
        int prec = 6;  /* default for %f */
        int has_prec = 0;
        if (*fmt == '.') {
            fmt++; prec = 0; has_prec = 1;
            while (*fmt >= '0' && *fmt <= '9') { prec = prec * 10 + (*fmt - '0'); fmt++; }
        }

        /* Length modifier */
        int is_long = 0, is_long_long = 0;
        if (*fmt == 'l') {
            is_long = 1; fmt++;
            if (*fmt == 'l') { is_long_long = 1; fmt++; }
        } else if (*fmt == 'z' || *fmt == 'h') {
            fmt++;   /* ignore size_t / short modifiers */
        }

        char pad = (zero_pad && !left) ? '0' : ' ';

        switch (*fmt) {
        case 'd': case 'i': {
            long long v = is_long_long ? (long long)va_arg(ap, long long)
                        : is_long      ? (long long)va_arg(ap, long)
                                       : (long long)va_arg(ap, int);
            int neg = (v < 0);
            unsigned long long uv = neg ? (unsigned long long)(-v) : (unsigned long long)v;
            int len = int_to_str(tmp, uv, 10, 0);
            int total = len + (neg || plus ? 1 : 0);
            if (!left && pad == ' ') for (int i = total; i < width; i++) sink_put(s, ' ');
            if (neg) sink_put(s, '-');
            else if (plus) sink_put(s, '+');
            if (!left && pad == '0') for (int i = total; i < width; i++) sink_put(s, '0');
            for (int i = 0; i < len; i++) sink_put(s, tmp[i]);
            if (left) for (int i = total; i < width; i++) sink_put(s, ' ');
            break;
        }
        case 'u': case 'o': case 'x': case 'X': case 'p': {
            int base = (*fmt == 'o') ? 8 : (*fmt == 'u') ? 10 : 16;
            int upper = (*fmt == 'X');
            unsigned long long uv;
            if (*fmt == 'p') {
                uv = (unsigned long long)(uintptr_t)va_arg(ap, void *);
            } else {
                uv = is_long_long ? (unsigned long long)va_arg(ap, unsigned long long)
                   : is_long      ? (unsigned long long)va_arg(ap, unsigned long)
                                  : (unsigned long long)va_arg(ap, unsigned int);
            }
            int len = int_to_str(tmp, uv, base, upper);
            if (!left) for (int i = len; i < width; i++) sink_put(s, pad);
            for (int i = 0; i < len; i++) sink_put(s, tmp[i]);
            if (left) for (int i = len; i < width; i++) sink_put(s, ' ');
            break;
        }
        case 'f': case 'F': case 'g': case 'G': {
            double v = va_arg(ap, double);
            int p = has_prec ? prec : 6;
            int len = double_to_str(tmp, v, p);
            if (!left) for (int i = len; i < width; i++) sink_put(s, pad);
            for (int i = 0; i < len; i++) sink_put(s, tmp[i]);
            if (left) for (int i = len; i < width; i++) sink_put(s, ' ');
            break;
        }
        case 'e': case 'E': {
            double v = va_arg(ap, double);
            int p = has_prec ? prec : 6;
            int len = double_to_exp_str(tmp, v, p, (*fmt == 'E'));
            if (!left) for (int i = len; i < width; i++) sink_put(s, pad);
            for (int i = 0; i < len; i++) sink_put(s, tmp[i]);
            if (left) for (int i = len; i < width; i++) sink_put(s, ' ');
            break;
        }
        case 's': {
            const char *str = va_arg(ap, const char *);
            if (!has_prec) {
                sink_str(s, str, width, left, pad);
            } else {
                /* Precision limits string length */
                const char *p2 = str ? str : "(null)";
                int slen = 0;
                while (p2[slen] && slen < prec) slen++;
                if (!left) for (int i = slen; i < width; i++) sink_put(s, pad);
                for (int i = 0; i < slen; i++) sink_put(s, p2[i]);
                if (left) for (int i = slen; i < width; i++) sink_put(s, ' ');
            }
            break;
        }
        case 'c':
            sink_put(s, (char)va_arg(ap, int));
            break;
        case '%':
            sink_put(s, '%');
            break;
        case '\0':
            goto done;
        default:
            sink_put(s, '%');
            sink_put(s, *fmt);
            break;
        }
    }
done:
    if (s->buf && (s->cap == 0 || s->pos < s->cap)) {
        s->buf[s->pos] = '\0';
    } else if (s->buf && s->cap > 0) {
        s->buf[s->cap - 1] = '\0';
    }
    return (int)s->pos;
}

/* ── Public API ──────────────────────────────────────────────────────────── */

int vsnprintf(char *buf, size_t sz, const char *fmt, va_list ap)
{
    Sink s = { buf, 0, sz };
    return vsink(&s, fmt, ap);
}

int vsprintf(char *buf, const char *fmt, va_list ap)
{
    Sink s = { buf, 0, 0 };
    return vsink(&s, fmt, ap);
}

int vprintf(const char *fmt, va_list ap)
{
    Sink s = { NULL, 0, 0 };
    return vsink(&s, fmt, ap);
}

int vfprintf(FILE *f, const char *fmt, va_list ap)
{
    (void)f;   /* all streams go to edge_putchar */
    Sink s = { NULL, 0, 0 };
    return vsink(&s, fmt, ap);
}

int snprintf(char *buf, size_t sz, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    int r = vsnprintf(buf, sz, fmt, ap);
    va_end(ap);
    return r;
}

int sprintf(char *buf, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    int r = vsprintf(buf, fmt, ap);
    va_end(ap);
    return r;
}

int printf(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    int r = vprintf(fmt, ap);
    va_end(ap);
    return r;
}

int fprintf(FILE *f, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    int r = vfprintf(f, fmt, ap);
    va_end(ap);
    return r;
}

int putchar(int c) { edge_putchar((char)c); return c; }
int puts(const char *s) { while (*s) edge_putchar(*s++); edge_putchar('\n'); return 0; }

/* ── sscanf — minimal (%d %u %f %s) ─────────────────────────────────────── */

#include <stdlib.h>   /* strtod, strtol */

int sscanf(const char *buf, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int matched = 0;
    const char *s = buf;

    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            if (*fmt == ' ') { while (*s == ' ' || *s == '\t') s++; }
            continue;
        }
        fmt++;
        while (*s == ' ' || *s == '\t') s++;  /* skip whitespace before arg */

        switch (*fmt) {
        case 'd': case 'i': {
            char *e;
            long v = strtol(s, &e, 10);
            if (e == s) goto done;
            *va_arg(ap, int *) = (int)v;
            s = e; matched++;
            break;
        }
        case 'u': {
            char *e;
            unsigned long v = (unsigned long)strtol(s, &e, 10);
            if (e == s) goto done;
            *va_arg(ap, unsigned int *) = (unsigned int)v;
            s = e; matched++;
            break;
        }
        case 'f': case 'g': case 'e': {
            char *e;
            double v = strtod(s, &e);
            if (e == s) goto done;
            *va_arg(ap, float *) = (float)v;
            s = e; matched++;
            break;
        }
        case 'l': {
            fmt++;  /* skip 'l' */
            if (*fmt == 'f' || *fmt == 'g' || *fmt == 'e') {
                char *e;
                double v = strtod(s, &e);
                if (e == s) goto done;
                *va_arg(ap, double *) = v;
                s = e; matched++;
            }
            break;
        }
        case 's': {
            char *out = va_arg(ap, char *);
            while (*s && *s != ' ' && *s != '\t' && *s != '\n') *out++ = *s++;
            *out = '\0';
            matched++;
            break;
        }
        default: break;
        }
    }
done:
    va_end(ap);
    return matched;
}

/* ── File stubs (always fail — no filesystem) ────────────────────────────── */

FILE *fopen(const char *path, const char *mode) { (void)path; (void)mode; return NULL; }
int   fclose(FILE *f)                            { (void)f; return 0; }
size_t fread(void *buf, size_t sz, size_t n, FILE *f) { (void)buf;(void)sz;(void)n;(void)f; return 0; }
size_t fwrite(const void *b, size_t sz, size_t n, FILE *f){ (void)b;(void)sz;(void)n;(void)f; return 0; }
int   fseek(FILE *f, long o, int w)             { (void)f;(void)o;(void)w; return -1; }
long  ftell(FILE *f)                            { (void)f; return -1L; }
void  rewind(FILE *f)                           { (void)f; }
int   feof(FILE *f)                             { (void)f; return 1; }
char *fgets(char *s, int n, FILE *f)            { (void)s;(void)n;(void)f; return NULL; }
int   fputs(const char *s, FILE *f)             { (void)s;(void)f; return -1; }
int   fputc(int c, FILE *f)                     { (void)c;(void)f; return -1; }
int   fflush(FILE *f)                           { (void)f; return 0; }
int   getchar(void)                             { return -1; /* EOF */ }

/* scanf — always returns 0 on bare metal (no stdin) */
int scanf(const char *fmt, ...)
{
    (void)fmt;
    return 0;
}
