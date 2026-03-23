/*
 * Minimal fcntl.h stub for NML Edge Worker — ARM Cortex-M4 bare metal.
 *
 * tweetnacl.c uses open("/dev/urandom", O_RDONLY) only inside randombytes(),
 * which is only called for key generation (crypto_sign_keypair).  The edge
 * worker only calls crypto_sign_open() (verify), so randombytes is never
 * reached at runtime.  These stubs satisfy the compiler; the functions
 * return failure values if somehow called.
 */
#ifndef _EDGE_FCNTL_H
#define _EDGE_FCNTL_H

#define O_RDONLY   0
#define O_WRONLY   1
#define O_RDWR     2
#define O_CREAT  0100
#define O_TRUNC  0200

int open(const char *path, int flags, ...);

#endif /* _EDGE_FCNTL_H */
