/*
 * posix_stubs.c — POSIX I/O stubs for ARM Cortex-M4 bare metal.
 *
 * open / read / write / close are referenced by tweetnacl.c's randombytes()
 * (key generation path) and are never called in normal edge-worker operation
 * (which only does signature verification).  Stubs return failure so that
 * randombytes() leaves the buffer zeroed if somehow reached.
 */

#include <stddef.h>

typedef long ssize_t;

int     open(const char *path, int flags, ...) { (void)path; (void)flags; return -1; }
ssize_t read(int fd, void *buf, size_t n)      { (void)fd; (void)buf; (void)n; return 0; }
ssize_t write(int fd, const void *buf, size_t n){ (void)fd; (void)buf; (void)n; return 0; }
int     close(int fd)                          { (void)fd; return -1; }
