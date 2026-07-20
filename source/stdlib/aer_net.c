#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#endif
#include "aer_stdlib.h"
#include "error.h"

/* Blocking TCP only -- connect/send/recv/close, the common "talk to a server" case. No
   HTTP/TLS layer and no async I/O, matching AER's single-threaded execution model. */

#ifdef _WIN32
typedef SOCKET sock_t;
#define SOCK_INVALID INVALID_SOCKET

/* Lazy, once per process -- WSACleanup is skipped deliberately, same as every other stdlib
   module's lack of explicit process-exit teardown; the OS reclaims it. */
static bool wsa_ready = false;
static void ensure_socket_layer(void) {
    if (wsa_ready) return;
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    wsa_ready = true;
}
static void sock_close(sock_t s) { closesocket(s); }
static const char* sock_errmsg(void) {
    static char buf[64];
    snprintf(buf, sizeof(buf), "socket error %d", WSAGetLastError());
    return buf;
}
#else
typedef int sock_t;
#define SOCK_INVALID (-1)
static void ensure_socket_layer(void) { }
static void sock_close(sock_t s) { close(s); }
static const char* sock_errmsg(void) { return strerror(errno); }
#endif

static AerVal make_error(const char* msg) {
    size_t n   = strlen(msg);
    char*  buf = xmalloc(n + 1);
    memcpy(buf, msg, n + 1);
    return aer_make_string(buf, (unsigned int)n);
}

#ifdef _WIN32
static bool set_nonblocking(sock_t s, bool nonblocking) {
    u_long mode = nonblocking ? 1 : 0;
    return ioctlsocket(s, FIONBIO, &mode) == 0;
}
#else
static bool set_nonblocking(sock_t s, bool nonblocking) {
    int flags = fcntl(s, F_GETFL, 0);
    if (flags < 0) return false;
    flags = nonblocking ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    return fcntl(s, F_SETFL, flags) == 0;
}
#endif

#define NET_CONNECT_TIMEOUT_SECONDS 10

/* connect() has no built-in timeout and can hang far longer than a normal refused connection on
   some host/network combinations -- found via tests/fuzz.py mutating a test's loopback address
   from 127.0.0.1 (a real host, connection refused instantly) to 127.0.0.0 (a network address, not
   a host; this OS's stack just doesn't answer, instead of returning RST), hanging the whole
   process. A bounded non-blocking connect + select() guarantees this can never block a script
   indefinitely, regardless of what the target does. */
static bool connect_with_timeout(sock_t s, const struct sockaddr* addr, socklen_t addrlen) {
    set_nonblocking(s, true);
    if (connect(s, addr, addrlen) == 0) { set_nonblocking(s, false); return true; }
#ifdef _WIN32
    if (WSAGetLastError() != WSAEWOULDBLOCK) return false;
#else
    if (errno != EINPROGRESS) return false;
#endif
    fd_set write_set;
    FD_ZERO(&write_set);
    FD_SET(s, &write_set);
    struct timeval tv;
    tv.tv_sec  = NET_CONNECT_TIMEOUT_SECONDS;
    tv.tv_usec = 0;
    if (select((int)s + 1, NULL, &write_set, NULL, &tv) <= 0) {
        set_nonblocking(s, false);
        return false;   /* timed out or select() itself failed */
    }
    int so_error = 0;
    socklen_t len = sizeof(so_error);
    getsockopt(s, SOL_SOCKET, SO_ERROR, (char*)&so_error, &len);
    set_nonblocking(s, false);
    return so_error == 0;
}

bool aer_net_call(VM* vm, int fn_id, int arg_count) {
    if (fn_id == FN_NET_CONNECT && arg_count == 2) {
        AerVal port_v = vm_stack_pop(vm);
        AerVal host_v = vm_stack_pop(vm);
        if (aer_type(host_v) != TYPE_STRING || aer_type(port_v) != TYPE_INTEGER) {
            error("net.connect() requires a host string and an integer port");
            vm_stack_push(vm, aer_null());
            return true;
        }
        ensure_socket_layer();

        char port_str[16];
        snprintf(port_str, sizeof(port_str), "%lld", (long long)aer_as_int(port_v));

        struct addrinfo hints;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family   = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        struct addrinfo* res = NULL;
        int gai = getaddrinfo(aer_as_string(host_v)->data, port_str, &hints, &res);
        if (gai != 0) {
            vm_stack_push(vm, aer_make_result(aer_null(), make_error(gai_strerror(gai))));
            return true;
        }

        sock_t s = SOCK_INVALID;
        for (struct addrinfo* p = res; p; p = p->ai_next) {
            s = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
            if (s == SOCK_INVALID) continue;
            if (connect_with_timeout(s, p->ai_addr, (socklen_t)p->ai_addrlen)) break;
            sock_close(s);
            s = SOCK_INVALID;
        }
        freeaddrinfo(res);
        if (s == SOCK_INVALID) {
            vm_stack_push(vm, aer_make_result(aer_null(), make_error(sock_errmsg())));
            return true;
        }
        vm_stack_push(vm, aer_make_result(aer_int((int64_t)(intptr_t)s), aer_null()));
        return true;
    }

    if (fn_id == FN_NET_SEND && arg_count == 2) {
        AerVal data_v   = vm_stack_pop(vm);
        AerVal handle_v = vm_stack_pop(vm);
        if (aer_type(handle_v) != TYPE_INTEGER || aer_type(data_v) != TYPE_STRING) {
            error("net.send() requires a connection handle and a string");
            vm_stack_push(vm, aer_null());
            return true;
        }
        sock_t    s   = (sock_t)(intptr_t)aer_as_int(handle_v);
        AerString* str = aer_as_string(data_v);
        long sent = send(s, str->data, (int)str->length, 0);
        if (sent < 0) {
            vm_stack_push(vm, aer_make_result(aer_null(), make_error(sock_errmsg())));
            return true;
        }
        vm_stack_push(vm, aer_make_result(aer_int((int64_t)sent), aer_null()));
        return true;
    }

    if (fn_id == FN_NET_RECV && arg_count == 2) {
        AerVal max_v    = vm_stack_pop(vm);
        AerVal handle_v = vm_stack_pop(vm);
        if (aer_type(handle_v) != TYPE_INTEGER || aer_type(max_v) != TYPE_INTEGER || aer_as_int(max_v) <= 0) {
            error("net.recv() requires a connection handle and a positive max-byte count");
            vm_stack_push(vm, aer_null());
            return true;
        }
        sock_t  s         = (sock_t)(intptr_t)aer_as_int(handle_v);
        int64_t max_bytes = aer_as_int(max_v);
        char*   buf       = xmalloc((size_t)max_bytes);
        long    got       = recv(s, buf, (int)max_bytes, 0);
        if (got < 0) {
            free(buf);
            vm_stack_push(vm, aer_make_result(aer_null(), make_error(sock_errmsg())));
            return true;
        }
        /* got == 0 means the peer closed the connection -- an empty string, not an error,
           matching io.read()'s own "EOF is just an empty read" convention. */
        buf = xrealloc(buf, (size_t)got + 1);
        buf[got] = '\0';
        vm_stack_push(vm, aer_make_result(aer_make_string(buf, (unsigned int)got), aer_null()));
        return true;
    }

    if (fn_id == FN_NET_CLOSE && arg_count == 1) {
        AerVal handle_v = vm_stack_pop(vm);
        if (aer_type(handle_v) != TYPE_INTEGER) {
            error("net.close() requires a connection handle");
            vm_stack_push(vm, aer_null());
            return true;
        }
        sock_close((sock_t)(intptr_t)aer_as_int(handle_v));
        vm_stack_push(vm, aer_null());
        return true;
    }

    return false;
}
