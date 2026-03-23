/*
 * Minimal unistd.h stub for NML Edge Worker — ARM Cortex-M4 bare metal.
 *
 * tweetnacl.c uses read() and close() only in randombytes(), which is only
 * called for key generation.  The edge worker performs only signature
 * verification (crypto_sign_open), not key generation.
 *
 * Stubs return failure values; they are never reached in normal use.
 */
#ifndef _EDGE_UNISTD_H
#define _EDGE_UNISTD_H

#include <stddef.h>

typedef long ssize_t;
typedef int  pid_t;

ssize_t read(int fd, void *buf, size_t count);
ssize_t write(int fd, const void *buf, size_t count);
int     close(int fd);

#endif /* _EDGE_UNISTD_H */
