// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Velin contributors
#pragma once

// Winsock names over BSD sockets, so socket code reads the same on Windows
// and POSIX systems. Include this instead of <winsock2.h> or <sys/socket.h>.

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

namespace velin::net {
using socklen = int;
inline constexpr int kAccessDenied = WSAEACCES;
inline bool startup() { WSADATA data{}; return WSAStartup(MAKEWORD(2, 2), &data) == 0; }
inline void cleanup() { WSACleanup(); }
inline int lastError() { return WSAGetLastError(); }
inline bool wouldBlock(int error) { return error == WSAEWOULDBLOCK; }
inline void setNonBlocking(SOCKET socket) { u_long value = 1; ioctlsocket(socket, FIONBIO, &value); }
inline void setReceiveTimeout(SOCKET socket, int milliseconds) {
    DWORD value = static_cast<DWORD>(milliseconds);
    setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&value), sizeof(value));
}
// Fail instead of sharing a port another process already bound.
inline void setExclusiveAddress(SOCKET socket) {
    BOOL value = TRUE;
    setsockopt(socket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&value), sizeof(value));
}
inline void suppressSigpipe(SOCKET) {}
// The first argument to select(): ignored by Winsock.
inline int selectWidth(SOCKET) { return 0; }
} // namespace velin::net

#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

using SOCKET = int;
inline constexpr SOCKET INVALID_SOCKET = -1;
inline constexpr int SOCKET_ERROR = -1;
inline int closesocket(SOCKET socket) { return ::close(socket); }

namespace velin::net {
using socklen = socklen_t;
inline constexpr int kAccessDenied = EACCES;
inline bool startup() { return true; }
inline void cleanup() {}
inline int lastError() { return errno; }
inline bool wouldBlock(int error) { return error == EWOULDBLOCK || error == EAGAIN || error == EINTR; }
inline void setNonBlocking(SOCKET socket) { ::fcntl(socket, F_SETFL, ::fcntl(socket, F_GETFL) | O_NONBLOCK); }
inline void setReceiveTimeout(SOCKET socket, int milliseconds) {
    timeval value{milliseconds / 1000, (milliseconds % 1000) * 1000};
    setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &value, sizeof(value));
}
// POSIX already refuses a second bind without SO_REUSEADDR.
inline void setExclusiveAddress(SOCKET) {}
// A peer that disconnects must produce EPIPE, not kill the process.
inline void suppressSigpipe(SOCKET socket) {
#ifdef SO_NOSIGPIPE
    int value = 1;
    setsockopt(socket, SOL_SOCKET, SO_NOSIGPIPE, &value, sizeof(value));
#else
    (void)socket;
#endif
}
inline int selectWidth(SOCKET highest) { return highest + 1; }
} // namespace velin::net
#endif
