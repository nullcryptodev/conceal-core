// BoltSocket.hpp — POSIX/Winsock helpers for BoltHttp
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
#pragma once

#include <cstddef>
#include <cstdint>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

namespace BoltHttp
{
namespace socket_detail
{
inline void ensureInit()
{
  static bool initialized = false;
  if (!initialized)
  {
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
    initialized = true;
  }
}

inline SOCKET toHandle(int fd)
{
  return fd < 0 ? INVALID_SOCKET : static_cast<SOCKET>(fd);
}

inline int fromHandle(SOCKET handle)
{
  return handle == INVALID_SOCKET ? -1 : static_cast<int>(handle);
}
} // namespace socket_detail

constexpr int BOLT_INVALID_SOCKET = -1;
// Linux SOCK_NONBLOCK (0x800); internal flag for boltSocket, not passed to ::socket() on Windows.
constexpr int BOLT_SOCK_NONBLOCK = 0x800;
constexpr int BOLT_SHUT_RDWR = SD_BOTH;
constexpr int BOLT_MSG_NOSIGNAL = 0;
constexpr int BOLT_EINPROGRESS = WSAEWOULDBLOCK;
constexpr int BOLT_EAGAIN = WSAEWOULDBLOCK;
constexpr int BOLT_EWOULDBLOCK = WSAEWOULDBLOCK;
constexpr int BOLT_EINTR = WSAEINTR;

inline int boltErrno() { return WSAGetLastError(); }

inline void boltClose(int fd)
{
  if (fd >= 0)
    closesocket(socket_detail::toHandle(fd));
}

inline void boltShutdown(int fd, int how)
{
  if (fd >= 0)
    shutdown(socket_detail::toHandle(fd), how);
}

inline int boltSend(int fd, const char *buf, size_t len, int flags)
{
  return send(socket_detail::toHandle(fd), buf, static_cast<int>(len), flags);
}

inline int boltRecv(int fd, char *buf, size_t len, int flags)
{
  return recv(socket_detail::toHandle(fd), buf, static_cast<int>(len), flags);
}

inline int boltSetNonBlocking(int fd, bool enable)
{
  u_long mode = enable ? 1 : 0;
  return ioctlsocket(socket_detail::toHandle(fd), FIONBIO, &mode);
}

inline int boltSocket(int domain, int type, int protocol)
{
  socket_detail::ensureInit();
  const bool nonBlocking = (type & BOLT_SOCK_NONBLOCK) != 0;
  const int baseType = type & ~BOLT_SOCK_NONBLOCK;
  const SOCKET handle = ::socket(domain, baseType, protocol);
  const int fd = socket_detail::fromHandle(handle);
  if (fd < 0)
    return BOLT_INVALID_SOCKET;
  if (nonBlocking && boltSetNonBlocking(fd, true) != 0)
  {
    boltClose(fd);
    return BOLT_INVALID_SOCKET;
  }
  return fd;
}

inline int boltConnect(int fd, const sockaddr *addr, int addrlen)
{
  return ::connect(socket_detail::toHandle(fd), addr, addrlen);
}

inline int boltInetPton(int af, const char *src, void *dst)
{
  return InetPtonA(af, src, dst);
}

inline const char *boltInetNtop(int af, const void *src, char *dst, size_t size)
{
  return InetNtopA(af, const_cast<PVOID>(src), dst, static_cast<DWORD>(size));
}

inline int boltSetRecvTimeoutMs(int fd, int timeoutMs)
{
  const DWORD timeoutMsValue = static_cast<DWORD>(timeoutMs);
  return setsockopt(socket_detail::toHandle(fd), SOL_SOCKET, SO_RCVTIMEO,
                    reinterpret_cast<const char *>(&timeoutMsValue), sizeof(timeoutMsValue));
}

inline int boltSetReuseAddr(int fd)
{
  BOOL opt = TRUE;
  return setsockopt(socket_detail::toHandle(fd), SOL_SOCKET, SO_REUSEADDR,
                    reinterpret_cast<const char *>(&opt), sizeof(opt));
}

inline int boltBind(int fd, const sockaddr *addr, int addrlen)
{
  return bind(socket_detail::toHandle(fd), addr, addrlen);
}

inline int boltListen(int fd, int backlog)
{
  return listen(socket_detail::toHandle(fd), backlog);
}

inline int boltAccept(int fd, sockaddr *addr, int *addrlen)
{
  const SOCKET client = accept(socket_detail::toHandle(fd), addr, addrlen);
  return socket_detail::fromHandle(client);
}

inline int boltAcceptNonBlocking(int fd, sockaddr *addr, int *addrlen)
{
  const SOCKET client = accept(socket_detail::toHandle(fd), addr, addrlen);
  const int clientFd = socket_detail::fromHandle(client);
  if (clientFd < 0)
    return BOLT_INVALID_SOCKET;
  if (boltSetNonBlocking(clientFd, true) != 0)
  {
    boltClose(clientFd);
    return BOLT_INVALID_SOCKET;
  }
  return clientFd;
}

inline int boltSelectWrite(int fd, int timeoutSec, int timeoutUsec)
{
  fd_set fdset;
  FD_ZERO(&fdset);
  FD_SET(socket_detail::toHandle(fd), &fdset);
  timeval tv;
  tv.tv_sec = timeoutSec;
  tv.tv_usec = timeoutUsec;
  return select(0, nullptr, &fdset, nullptr, &tv);
}

inline int boltGetSocketError(int fd)
{
  int soError = 0;
  int len = sizeof(soError);
  getsockopt(socket_detail::toHandle(fd), SOL_SOCKET, SO_ERROR,
             reinterpret_cast<char *>(&soError), &len);
  return soError;
}
} // namespace BoltHttp

#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace BoltHttp
{
constexpr int BOLT_INVALID_SOCKET = -1;
#if defined(__linux__)
constexpr int BOLT_SOCK_NONBLOCK = SOCK_NONBLOCK;
#else
// Linux SOCK_NONBLOCK (0x800); internal flag for boltSocket, not passed to ::socket() on BSD/macOS.
constexpr int BOLT_SOCK_NONBLOCK = 0x800;
#endif
constexpr int BOLT_SHUT_RDWR = SHUT_RDWR;
#ifndef MSG_NOSIGNAL
constexpr int BOLT_MSG_NOSIGNAL = 0;
#else
constexpr int BOLT_MSG_NOSIGNAL = MSG_NOSIGNAL;
#endif
constexpr int BOLT_EINPROGRESS = EINPROGRESS;
constexpr int BOLT_EAGAIN = EAGAIN;
constexpr int BOLT_EWOULDBLOCK = EWOULDBLOCK;
constexpr int BOLT_EINTR = EINTR;

inline int boltErrno() { return errno; }

inline void boltClose(int fd)
{
  if (fd >= 0)
    ::close(fd);
}

inline void boltShutdown(int fd, int how)
{
  if (fd >= 0)
    ::shutdown(fd, how);
}

inline ssize_t boltSend(int fd, const void *buf, size_t len, int flags)
{
  return ::send(fd, buf, len, flags);
}

inline ssize_t boltRecv(int fd, void *buf, size_t len, int flags)
{
  return ::recv(fd, buf, len, flags);
}

inline int boltSetNonBlocking(int fd, bool enable)
{
  const int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0)
    return -1;
  return fcntl(fd, F_SETFL, enable ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK));
}

inline int boltSocket(int domain, int type, int protocol)
{
#if defined(__linux__)
  return ::socket(domain, type, protocol);
#else
  const bool nonBlocking = (type & BOLT_SOCK_NONBLOCK) != 0;
  const int baseType = type & ~BOLT_SOCK_NONBLOCK;
  const int fd = ::socket(domain, baseType, protocol);
  if (fd < 0)
    return BOLT_INVALID_SOCKET;
  if (nonBlocking && boltSetNonBlocking(fd, true) != 0)
  {
    boltClose(fd);
    return BOLT_INVALID_SOCKET;
  }
  return fd;
#endif
}

inline int boltConnect(int fd, const sockaddr *addr, socklen_t addrlen)
{
  return ::connect(fd, addr, addrlen);
}

inline int boltInetPton(int af, const char *src, void *dst)
{
  return inet_pton(af, src, dst);
}

inline const char *boltInetNtop(int af, const void *src, char *dst, size_t size)
{
  return inet_ntop(af, src, dst, static_cast<socklen_t>(size));
}

inline int boltSetRecvTimeoutMs(int fd, int timeoutMs)
{
  timeval tv;
  tv.tv_sec = timeoutMs / 1000;
  tv.tv_usec = (timeoutMs % 1000) * 1000;
  return setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

inline int boltSetReuseAddr(int fd)
{
  const int opt = 1;
  return setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
}

inline int boltBind(int fd, const sockaddr *addr, socklen_t addrlen)
{
  return bind(fd, addr, addrlen);
}

inline int boltListen(int fd, int backlog)
{
  return listen(fd, backlog);
}

inline int boltAccept(int fd, sockaddr *addr, socklen_t *addrlen)
{
  return accept(fd, addr, addrlen);
}

inline int boltAcceptNonBlocking(int fd, sockaddr *addr, socklen_t *addrlen)
{
#if defined(__linux__)
  return accept4(fd, addr, addrlen, BOLT_SOCK_NONBLOCK);
#else
  const int clientFd = accept(fd, addr, addrlen);
  if (clientFd < 0)
    return BOLT_INVALID_SOCKET;
  if (boltSetNonBlocking(clientFd, true) != 0)
  {
    boltClose(clientFd);
    return BOLT_INVALID_SOCKET;
  }
  return clientFd;
#endif
}

inline int boltSelectWrite(int fd, int timeoutSec, int timeoutUsec)
{
  fd_set fdset;
  FD_ZERO(&fdset);
  FD_SET(fd, &fdset);
  timeval tv;
  tv.tv_sec = timeoutSec;
  tv.tv_usec = timeoutUsec;
  return select(fd + 1, nullptr, &fdset, nullptr, &tv);
}

inline int boltGetSocketError(int fd)
{
  int soError = 0;
  socklen_t len = sizeof(soError);
  getsockopt(fd, SOL_SOCKET, SO_ERROR, &soError, &len);
  return soError;
}
} // namespace BoltHttp
#endif
