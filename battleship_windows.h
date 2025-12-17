#ifndef BATTLESHIP_WINDOWS_H
#define BATTLESHIP_WINDOWS_H

/* Cross-platform small helper header.
   This version avoids redefining types already provided by winsock2.h
   and exposes a poll() fallback on Windows when WSAPoll is not available.
   Also includes <signal.h> so SIGINT/SIGTERM and signal() are visible.
*/

#ifdef __INTELLISENSE__
/* Lightweight stubs for IntelliSense (editor only) */
#ifdef _WIN32
typedef unsigned long DWORD;
typedef unsigned long SOCKET;
#define INVALID_SOCKET ((SOCKET)-1)
#define STDIN_FILENO 0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2
#ifndef POLLIN
#define POLLIN 0x0001
#endif
static inline bool init_winsock() { return true; }
static inline void cleanup_winsock() {}
static inline int sock_close(int fd) { (void)fd; return 0; }
static inline void Sleep(unsigned long ms) { (void)ms; }
static inline void usleep(unsigned int microseconds) { (void)microseconds; }
#else
typedef int SOCKET;
#define INVALID_SOCKET ((SOCKET)-1)
static inline bool init_winsock() { return true; }
static inline void cleanup_winsock() {}
static inline int sock_close(int fd) { (void)fd; return 0; }
static inline void Sleep(unsigned long ms) { (void)ms; }
static inline void usleep(unsigned int microseconds) { (void)microseconds; }
#endif
#else /* not __INTELLISENSE__ */

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <signal.h>    /* bring signal(), SIGINT, SIGTERM */
#pragma comment(lib, "ws2_32.lib")
#include <io.h>
#include <stdio.h>
#include <errno.h>
#include <sys/types.h>

/* STD fds */
#ifndef STDIN_FILENO
#define STDIN_FILENO 0
#endif
#ifndef STDOUT_FILENO
#define STDOUT_FILENO 1
#endif
#ifndef STDERR_FILENO
#define STDERR_FILENO 2
#endif

/* Sleep/usleep wrappers */
#ifndef sleep
#define sleep(seconds) Sleep((seconds) * 1000)
#endif
#ifndef usleep
static inline void usleep(unsigned int microseconds) { Sleep((microseconds) / 1000); }
#endif

/* Winsock helpers */
static inline bool init_winsock() {
    WSADATA wsaData;
    return WSAStartup(MAKEWORD(2, 2), &wsaData) == 0;
}
static inline void cleanup_winsock() {
    WSACleanup();
}
static inline int sock_close(int fd) {
    return closesocket((SOCKET)fd);
}

/* Provide a poll() fallback mapped to WSAPoll if available, otherwise emulate
   behavior for simple use-cases using select(). Do NOT redefine struct pollfd —
   winsock2.h already provides it on MSYS2/ucrt64. */
#ifndef HAVE_WSAPOLL_FALLBACK
/* If WSAPoll is available, prefer that. If not, provide an emulation and map
   poll(...) to WSAPoll_emulation. Emulation supports POLLIN testing for sockets. */
#if defined(WSAPoll) || defined(WSAPOLL) || defined(__MINGW32__)
/* WSAPoll should exist on modern toolchains */
#define HAVE_WSAPOLL_FALLBACK 1
#else
#define HAVE_WSAPOLL_FALLBACK 1 /* we'll provide emulation below */
#endif
#endif

#if HAVE_WSAPOLL_FALLBACK && !defined(WSAPoll)
#include <sys/time.h>
/* winsock2.h already typedefs struct pollfd; use that. */
static inline int WSAPoll_emulation(struct pollfd *fds, unsigned long nfds, int timeout) {
    if (nfds == 0) return 0;
    fd_set rfds;
    FD_ZERO(&rfds);
    SOCKET maxfd = 0;
    for (unsigned long i = 0; i < nfds; ++i) {
        FD_SET(fds[i].fd, &rfds);
        if (fds[i].fd > maxfd) maxfd = fds[i].fd;
        fds[i].revents = 0;
    }
    struct timeval tv;
    struct timeval *ptv = NULL;
    if (timeout >= 0) {
        tv.tv_sec = timeout / 1000;
        tv.tv_usec = (timeout % 1000) * 1000;
        ptv = &tv;
    }
    int sel = select((int)(maxfd + 1), &rfds, NULL, NULL, ptv);
    if (sel <= 0) return sel;
    for (unsigned long i = 0; i < nfds; ++i) {
        if (FD_ISSET(fds[i].fd, &rfds)) fds[i].revents = POLLIN;
        else fds[i].revents = 0;
    }
    return sel;
}

/* Map poll(...) calls to our emulation if WSAPoll isn't present */
#ifndef poll
static inline int poll(struct pollfd *fds, unsigned long nfds, int timeout) {
    return WSAPoll_emulation(fds, nfds, timeout);
}
#endif

#endif /* HAVE_WSAPOLL_FALLBACK && !WSAPoll */

#else /* POSIX branch */

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <poll.h>
#include <signal.h>
#include <syslog.h>
#include <fcntl.h>
#include <stdio.h>
#include <errno.h>

static inline bool init_winsock() { (void)0; return true; }
static inline void cleanup_winsock() { (void)0; }
static inline int sock_close(int fd) { return close(fd); }

#endif /* _WIN32 */

#endif /* __INTELLISENSE__ */

#endif /* BATTLESHIP_WINDOWS_H */