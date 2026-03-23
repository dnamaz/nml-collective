/*
 * alloc.c — Static block heap for NML Edge Worker on ARM Cortex-M4.
 *
 * Provides malloc / calloc / realloc / free backed by a fixed-size byte array
 * placed in .bss (zero-initialised at startup).  No OS, no sbrk, no newlib.
 *
 * Algorithm: first-fit free-list with 8-byte-aligned headers.
 * Suitable for the NML VM's alloc pattern (many medium allocs, freed at once).
 *
 * Heap size: controlled by EDGE_HEAP_SIZE (default 128 KiB).
 * Override at compile time:  -DEDGE_HEAP_SIZE=65536
 *
 * Thread safety: none — the edge worker is single-threaded on MCU.
 */

#include <stddef.h>
#include <stdint.h>

#ifndef EDGE_HEAP_SIZE
#define EDGE_HEAP_SIZE (128 * 1024)   /* 128 KiB */
#endif

/* ── Block header ────────────────────────────────────────────────────────── */

#define ALIGN 8u

typedef struct Block {
    size_t        size;   /* payload size in bytes (not counting header) */
    int           free;   /* 1 = available, 0 = in use */
    struct Block *next;   /* next block in list (NULL = last) */
} Block;

#define HEADER_SZ  (sizeof(Block))

/* ── Heap storage ────────────────────────────────────────────────────────── */

static uint8_t _heap[EDGE_HEAP_SIZE] __attribute__((aligned(ALIGN)));
static Block  *_head = NULL;

/* ── Internal helpers ────────────────────────────────────────────────────── */

static size_t align_up(size_t n)
{
    return (n + ALIGN - 1u) & ~(ALIGN - 1u);
}

static void heap_init(void)
{
    _head = (Block *)_heap;
    _head->size = EDGE_HEAP_SIZE - HEADER_SZ;
    _head->free = 1;
    _head->next = NULL;
}

/* Split block b so that b->size == need; returns 1 if split happened */
static int block_split(Block *b, size_t need)
{
    size_t remainder = b->size - need - HEADER_SZ;
    if (b->size < need + HEADER_SZ + ALIGN) return 0;   /* not enough room */

    Block *next = (Block *)((uint8_t *)b + HEADER_SZ + need);
    next->size = remainder;
    next->free = 1;
    next->next = b->next;

    b->size = need;
    b->next = next;
    return 1;
}

/* Coalesce consecutive free blocks starting at b */
static void block_coalesce(Block *b)
{
    while (b->next && b->next->free) {
        b->size += HEADER_SZ + b->next->size;
        b->next  = b->next->next;
    }
}

/* ── Public API ──────────────────────────────────────────────────────────── */

void *malloc(size_t size)
{
    if (size == 0) return NULL;
    if (!_head) heap_init();

    size = align_up(size);

    for (Block *b = _head; b; b = b->next) {
        if (!b->free || b->size < size) continue;
        block_split(b, size);
        b->free = 0;
        return (uint8_t *)b + HEADER_SZ;
    }
    return NULL;   /* OOM */
}

void free(void *ptr)
{
    if (!ptr) return;
    Block *b = (Block *)((uint8_t *)ptr - HEADER_SZ);
    b->free = 1;
    block_coalesce(b);
}

void *calloc(size_t nmemb, size_t size)
{
    size_t total = nmemb * size;
    void  *p = malloc(total);
    if (!p) return NULL;
    /* Zero out — __builtin_memset works in freestanding mode */
    uint8_t *bp = (uint8_t *)p;
    for (size_t i = 0; i < total; i++) bp[i] = 0;
    return p;
}

void *realloc(void *ptr, size_t size)
{
    if (!ptr) return malloc(size);
    if (size == 0) { free(ptr); return NULL; }

    Block  *b    = (Block *)((uint8_t *)ptr - HEADER_SZ);
    size_t  need = align_up(size);

    if (b->size >= need) return ptr;   /* already large enough */

    void *np = malloc(size);
    if (!np) return NULL;

    /* Copy old data */
    size_t copy = b->size < need ? b->size : need;
    uint8_t *src = (uint8_t *)ptr;
    uint8_t *dst = (uint8_t *)np;
    for (size_t i = 0; i < copy; i++) dst[i] = src[i];

    free(ptr);
    return np;
}

/* ── stdlib stubs used by nml.c / edge sources ───────────────────────────── */

#include <stdlib.h>   /* our platform stub: atoi, strtod, srand, rand, … */

/* atoi — used by nml.c for shape parsing */
int atoi(const char *s)
{
    int neg = 0, v = 0;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '-') { neg = 1; s++; } else if (*s == '+') s++;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    return neg ? -v : v;
}

long atol(const char *s) { return (long)atoi(s); }

double atof(const char *s) { return strtod(s, NULL); }

/* strtod — minimal implementation sufficient for NML data files */
double strtod(const char *s, char **end)
{
    while (*s == ' ' || *s == '\t') s++;
    int neg = 0;
    if (*s == '-') { neg = 1; s++; } else if (*s == '+') s++;

    double v = 0.0;
    while (*s >= '0' && *s <= '9') { v = v * 10.0 + (*s - '0'); s++; }

    if (*s == '.') {
        s++;
        double frac = 0.1;
        while (*s >= '0' && *s <= '9') { v += (*s - '0') * frac; frac *= 0.1; s++; }
    }

    /* Exponent */
    if (*s == 'e' || *s == 'E') {
        s++;
        int eneg = 0;
        if (*s == '-') { eneg = 1; s++; } else if (*s == '+') s++;
        int exp = 0;
        while (*s >= '0' && *s <= '9') { exp = exp * 10 + (*s - '0'); s++; }
        double base = 10.0;
        double mult = 1.0;
        while (exp-- > 0) mult *= base;
        v = eneg ? v / mult : v * mult;
    }

    if (end) *end = (char *)s;
    return neg ? -v : v;
}

long strtol(const char *s, char **end, int base)
{
    while (*s == ' ' || *s == '\t') s++;
    int neg = 0;
    if (*s == '-') { neg = 1; s++; } else if (*s == '+') s++;
    if (base == 0) {
        if (*s == '0' && (s[1] == 'x' || s[1] == 'X')) { base = 16; s += 2; }
        else if (*s == '0') { base = 8; s++; }
        else base = 10;
    }
    long v = 0;
    while (1) {
        int d;
        if (*s >= '0' && *s <= '9') d = *s - '0';
        else if (*s >= 'a' && *s <= 'f') d = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'F') d = *s - 'A' + 10;
        else break;
        if (d >= base) break;
        v = v * base + d;
        s++;
    }
    if (end) *end = (char *)s;
    return neg ? -v : v;
}

/* Simple LCG — sufficient for the NML VM's optional RAND opcode */
static unsigned int _rand_seed = 1;

void srand(unsigned int seed) { _rand_seed = seed; }

int rand(void)
{
    _rand_seed = _rand_seed * 1664525u + 1013904223u;
    return (int)(_rand_seed >> 1) & 0x7fffffff;
}

void abort(void)
{
    /* Trigger a fault on ARM — loop so the CPU halts in debug mode */
    for (;;) __asm volatile ("bkpt #0");
}

void exit(int status)
{
    (void)status;
    for (;;) __asm volatile ("wfi");   /* wait-for-interrupt — halt */
}

/* strdup — uses our malloc */
#include <string.h>
char *strdup(const char *s)
{
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char  *p = (char *)malloc(n);
    if (p) {
        for (size_t i = 0; i < n; i++) p[i] = s[i];
    }
    return p;
}
