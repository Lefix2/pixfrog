// lwIP sockets → an in-process datagram fabric (tests/shims/src/net.cpp).
// Host headers supply the types and byte-order helpers; the calls are
// redirected by macro in the including translation unit only, so libc (and
// the sanitizers' interceptors) keep their own socket()/recvfrom().
#pragma once
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

int shim_socket(int domain, int type, int proto);
int shim_bind(int fd, const struct sockaddr* addr, socklen_t len);
int shim_setsockopt(int fd, int level, int opt, const void* val, socklen_t len);
ssize_t shim_recvfrom(int fd, void* buf, size_t len, int flags, struct sockaddr* from,
                      socklen_t* fromlen);
ssize_t shim_sendto(int fd, const void* buf, size_t len, int flags, const struct sockaddr* to,
                    socklen_t tolen);
int shim_shutdown(int fd, int how);
int shim_close(int fd);

#define socket(d, t, p) shim_socket(d, t, p)
#define bind(f, a, l) shim_bind(f, a, l)
#define setsockopt(f, l, o, v, n) shim_setsockopt(f, l, o, v, n)
#define recvfrom(f, b, l, fl, a, al) shim_recvfrom(f, b, l, fl, a, al)
#define sendto(f, b, l, fl, a, al) shim_sendto(f, b, l, fl, a, al)
#define shutdown(f, h) shim_shutdown(f, h)
#define close(f) shim_close(f)
