/*
 * compat.h — Cross-platform abstraction layer.
 *
 * Provides a unified API for sockets, filesystem, and signals across:
 *   - Linux / macOS  (POSIX)
 *   - Windows MSVC   (_MSC_VER)
 *   - MinGW / MSYS2  (__MINGW32__)
 *
 * Include this instead of <sys/socket.h>, <arpa/inet.h>, <unistd.h>, etc.
 */

#ifndef EDGE_COMPAT_H
#define EDGE_COMPAT_H

/* ── Platform detection ─────────────────────────────────────────────────── */

#if defined(_MSC_VER) || defined(__MINGW32__) || defined(__MINGW64__)
  #define COMPAT_WINDOWS 1
#else
  #define COMPAT_POSIX 1
#endif

/* ── Socket headers ─────────────────────────────────────────────────────── */

#ifdef COMPAT_WINDOWS
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <windows.h>

  /* Winsock must be initialised before any socket call.
     Call compat_winsock_init() once in main(). */
  static inline int compat_winsock_init(void) {
      WSADATA wsa;
      return WSAStartup(MAKEWORD(2, 2), &wsa);
  }
  static inline void compat_winsock_cleanup(void) {
      WSACleanup();
  }

  /* Winsock uses SOCKET (unsigned), not int.  We alias for the common case. */
  typedef SOCKET compat_socket_t;
  #define COMPAT_INVALID_SOCKET INVALID_SOCKET

  /* close() → closesocket() */
  #define compat_close_socket(s) closesocket(s)

  /* fcntl non-blocking → ioctlsocket */
  static inline int compat_set_nonblocking(compat_socket_t fd) {
      u_long mode = 1;
      return ioctlsocket(fd, FIONBIO, &mode);
  }

  /* setsockopt on Windows expects (const char *) for the value pointer */
  #define COMPAT_SOCKOPT_CAST(ptr) ((const char *)(ptr))

  /* select() on Windows ignores the first argument (nfds) */
  #define COMPAT_SELECT_NFDS(fd) (0)

  /* inet_ntop available via ws2tcpip.h on MinGW-w64 / MSVC */

#else /* POSIX */
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <sys/select.h>
  #include <unistd.h>
  #include <fcntl.h>

  static inline int  compat_winsock_init(void)    { return 0; }
  static inline void compat_winsock_cleanup(void)  { /* no-op */ }

  typedef int compat_socket_t;
  #define COMPAT_INVALID_SOCKET (-1)

  #define compat_close_socket(s) close(s)

  static inline int compat_set_nonblocking(compat_socket_t fd) {
      int flags = fcntl(fd, F_GETFL, 0);
      if (flags < 0) return -1;
      return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  }

  #define COMPAT_SOCKOPT_CAST(ptr) (ptr)

  #define COMPAT_SELECT_NFDS(fd) ((fd) + 1)

#endif

/* ── Filesystem ─────────────────────────────────────────────────────────── */

#include <sys/stat.h>

#ifdef COMPAT_WINDOWS
  #include <direct.h>
  #include <process.h>
  /* MinGW and MSVC mkdir() has no mode parameter */
  #define compat_mkdir(path, mode) _mkdir(path)
  #define compat_getpid() _getpid()
#else
  #define compat_mkdir(path, mode) mkdir(path, mode)
  #define compat_getpid() getpid()
#endif

/* ── Signals ────────────────────────────────────────────────────────────── */

#include <signal.h>

/*
 * SIGTERM is not reliably delivered on Windows console apps.
 * SIGINT (Ctrl-C) works everywhere.  We define a no-op fallback
 * so callers can unconditionally register for SIGTERM.
 */
#ifdef COMPAT_WINDOWS
  #ifndef SIGTERM
    #define SIGTERM 15
  #endif
#endif

#endif /* EDGE_COMPAT_H */
