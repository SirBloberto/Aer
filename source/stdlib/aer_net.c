#include <stdio.h>
#include <stdlib.h>
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
static void sock_close(sock_t s) {
    closesocket(s);
}
static const char* sock_errmsg(void) {
    static char buf[64];
    snprintf(buf, sizeof(buf), "socket error %d", WSAGetLastError());
    return buf;
}
#else
typedef int sock_t;
#define SOCK_INVALID (-1)
static void ensure_socket_layer(void) {}
static void sock_close(sock_t s) {
    close(s);
}
static const char* sock_errmsg(void) {
    return strerror(errno);
}
#endif

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

/* Shared bound for every blocking net operation -- connect/send/recv all found (or could find) the
   same class of indefinite hang, so they all get the same bound rather than each picking its own
   number. */
#define NET_TIMEOUT_SECONDS 10

/* connect() has no built-in timeout and can hang far longer than a normal refused connection on
   some host/network combinations -- found via tests/fuzz.py mutating a test's loopback address
   from 127.0.0.1 (a real host, connection refused instantly) to 127.0.0.0 (a network address, not
   a host; this OS's stack just doesn't answer, instead of returning RST), hanging the whole
   process. A bounded non-blocking connect + select() guarantees this can never block a script
   indefinitely, regardless of what the target does. */
static bool connect_with_timeout(sock_t s, const struct sockaddr* addr, socklen_t addrlen) {
    set_nonblocking(s, true);
    if (connect(s, addr, addrlen) == 0) {
        set_nonblocking(s, false);
        return true;
    }
#ifdef _WIN32
    if (WSAGetLastError() != WSAEWOULDBLOCK) return false;
#else
    if (errno != EINPROGRESS) return false;
#endif
    fd_set write_set;
    FD_ZERO(&write_set);
    FD_SET(s, &write_set);
    struct timeval tv;
    tv.tv_sec = NET_TIMEOUT_SECONDS;
    tv.tv_usec = 0;
    if (select((int)s + 1, NULL, &write_set, NULL, &tv) <= 0) {
        set_nonblocking(s, false);
        return false; /* timed out or select() itself failed */
    }
    int so_error = 0;
    socklen_t len = sizeof(so_error);
    getsockopt(s, SOL_SOCKET, SO_ERROR, (char*)&so_error, &len);
    set_nonblocking(s, false);
    return so_error == 0;
}

/* send()/recv() are plain blocking calls with no bound of their own -- a connection that's open
   but silent (peer never sends, or never drains its receive buffer) hangs exactly like the
   pre-fix connect() did. Rather than switching the socket to non-blocking I/O (unnecessary here --
   there's no partial-progress case to retry), just gate the existing blocking call behind a bounded
   wait: the socket never leaves blocking mode, only how long we wait for it to become ready is bounded. */
static bool wait_ready(sock_t s, bool for_write, int timeout_sec) {
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(s, &fds);
    struct timeval tv;
    tv.tv_sec = timeout_sec;
    tv.tv_usec = 0;
    int ready =
        for_write ? select((int)s + 1, NULL, &fds, NULL, &tv) : select((int)s + 1, &fds, NULL, NULL, &tv);
    return ready > 0;
}

/* Handles given to scripts are opaque integer ids resolved through this registry, never a raw
   socket cast through an int -- a script passing a wrong or stale integer (a typo'd variable, a
   handle reused after close) must get a clean error, not silently operate on whatever OS handle
   that integer happens to collide with (e.g. net.close(0) closing real stdin on POSIX). Mirrors
   aer_actor.c's aer_actor_resolve(). Entries are removed on close() specifically so a reused id can
   never resolve to a socket the OS has since recycled for something unrelated. */
typedef struct SocketEntry {
    unsigned int id;
    sock_t sock;
    struct SocketEntry* next;
} SocketEntry;

static SocketEntry* sockets = NULL;
static unsigned int next_socket_id = 1; /* 0 reserved as "no such handle" */

static unsigned int register_socket(sock_t s) {
    SocketEntry* e = xmalloc(sizeof(SocketEntry));
    e->id = next_socket_id++;
    e->sock = s;
    e->next = sockets;
    sockets = e;
    return e->id;
}

/* *out_sock untouched on failure. */
static bool resolve_socket(AerVal handle_v, sock_t* out_sock) {
    if (aer_type(handle_v) != TYPE_INTEGER) return false;
    unsigned int id = (unsigned int)aer_as_int(handle_v);
    for (SocketEntry* e = sockets; e; e = e->next) {
        if (e->id == id) {
            *out_sock = e->sock;
            return true;
        }
    }
    return false;
}

static void unregister_socket(unsigned int id) {
    SocketEntry** link = &sockets;
    while (*link && (*link)->id != id)
        link = &(*link)->next;
    if (*link) {
        SocketEntry* dead = *link;
        *link = dead->next;
        free(dead);
    }
}

bool aer_net_call(VM* vm, int fn_id, int arg_count) {
    if (!vm->net_enabled) {
        for (int i = 0; i < arg_count; i++)
            vm_stack_pop(vm);
        error("net is disabled for this run (--no-net)");
        vm_stack_push(vm, aer_null());
        return true;
    }
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
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        struct addrinfo* res = NULL;
        int gai = getaddrinfo(aer_as_string(host_v)->data, port_str, &hints, &res);
        if (gai != 0) {
            vm_stack_push(vm, aer_make_result(aer_null(), aer_make_error(gai_strerror(gai))));
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
            vm_stack_push(vm, aer_make_result(aer_null(), aer_make_error(sock_errmsg())));
            return true;
        }
        vm_stack_push(vm, aer_make_result(aer_int((int64_t)register_socket(s)), aer_null()));
        return true;
    }

    if (fn_id == FN_NET_SEND && arg_count == 2) {
        AerVal data_v = vm_stack_pop(vm);
        AerVal handle_v = vm_stack_pop(vm);
        if (aer_type(handle_v) != TYPE_INTEGER || aer_type(data_v) != TYPE_STRING) {
            error("net.send() requires a connection handle and a string");
            vm_stack_push(vm, aer_null());
            return true;
        }
        sock_t s;
        if (!resolve_socket(handle_v, &s)) {
            error("net.send(): no such connection handle");
            vm_stack_push(vm, aer_null());
            return true;
        }
        AerString* str = aer_as_string(data_v);
        if (!wait_ready(s, true, NET_TIMEOUT_SECONDS)) {
            char msg[64];
            snprintf(msg, sizeof(msg), "net.send() timed out after %ds", NET_TIMEOUT_SECONDS);
            vm_stack_push(vm, aer_make_result(aer_null(), aer_make_error(msg)));
            return true;
        }
        long sent = send(s, str->data, (int)str->length, 0);
        if (sent < 0) {
            vm_stack_push(vm, aer_make_result(aer_null(), aer_make_error(sock_errmsg())));
            return true;
        }
        vm_stack_push(vm, aer_make_result(aer_int((int64_t)sent), aer_null()));
        return true;
    }

    if (fn_id == FN_NET_RECV && arg_count == 2) {
        AerVal max_v = vm_stack_pop(vm);
        AerVal handle_v = vm_stack_pop(vm);
        if (aer_type(handle_v) != TYPE_INTEGER || aer_type(max_v) != TYPE_INTEGER || aer_as_int(max_v) <= 0) {
            error("net.recv() requires a connection handle and a positive max-byte count");
            vm_stack_push(vm, aer_null());
            return true;
        }
        sock_t s;
        if (!resolve_socket(handle_v, &s)) {
            error("net.recv(): no such connection handle");
            vm_stack_push(vm, aer_null());
            return true;
        }
        int64_t max_bytes = aer_as_int(max_v);
        if (!wait_ready(s, false, NET_TIMEOUT_SECONDS)) {
            char msg[64];
            snprintf(msg, sizeof(msg), "net.recv() timed out after %ds", NET_TIMEOUT_SECONDS);
            vm_stack_push(vm, aer_make_result(aer_null(), aer_make_error(msg)));
            return true;
        }
        char* buf = xmalloc((size_t)max_bytes);
        long got = recv(s, buf, (int)max_bytes, 0);
        if (got < 0) {
            free(buf);
            vm_stack_push(vm, aer_make_result(aer_null(), aer_make_error(sock_errmsg())));
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
        sock_t s;
        if (!resolve_socket(handle_v, &s)) {
            error("net.close(): no such connection handle");
            vm_stack_push(vm, aer_null());
            return true;
        }
        sock_close(s);
        unregister_socket((unsigned int)aer_as_int(handle_v));
        vm_stack_push(vm, aer_null());
        return true;
    }

    if (fn_id == FN_NET_LISTEN && arg_count == 1) {
        AerVal port_v = vm_stack_pop(vm);
        if (aer_type(port_v) != TYPE_INTEGER) {
            error("net.listen() requires an integer port");
            vm_stack_push(vm, aer_null());
            return true;
        }
        ensure_socket_layer();

        char port_str[16];
        snprintf(port_str, sizeof(port_str), "%lld", (long long)aer_as_int(port_v));

        /* IPv4 only, not AF_UNSPEC -- an AF_UNSPEC+AI_PASSIVE lookup can resolve to the IPv6
           wildcard first, and Windows binds that IPv6-only by default, silently refusing IPv4
           clients (e.g. net.connect("127.0.0.1", ...)). Forcing IPv4 keeps this deterministic and
           matches every other net-facing test in this codebase, which already targets 127.0.0.1. */
        struct addrinfo hints;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_flags = AI_PASSIVE;
        struct addrinfo* res = NULL;
        int gai = getaddrinfo(NULL, port_str, &hints, &res);
        if (gai != 0) {
            vm_stack_push(vm, aer_make_result(aer_null(), aer_make_error(gai_strerror(gai))));
            return true;
        }

        sock_t s = SOCK_INVALID;
        for (struct addrinfo* p = res; p; p = p->ai_next) {
            s = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
            if (s == SOCK_INVALID) continue;
            int yes = 1;
            setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));
            if (bind(s, p->ai_addr, (socklen_t)p->ai_addrlen) == 0 && listen(s, 16) == 0) break;
            sock_close(s);
            s = SOCK_INVALID;
        }
        freeaddrinfo(res);
        if (s == SOCK_INVALID) {
            vm_stack_push(vm, aer_make_result(aer_null(), aer_make_error(sock_errmsg())));
            return true;
        }
        vm_stack_push(vm, aer_make_result(aer_int((int64_t)register_socket(s)), aer_null()));
        return true;
    }

    if (fn_id == FN_NET_ACCEPT && arg_count == 1) {
        AerVal handle_v = vm_stack_pop(vm);
        if (aer_type(handle_v) != TYPE_INTEGER) {
            error("net.accept() requires a listening handle");
            vm_stack_push(vm, aer_null());
            return true;
        }
        sock_t s;
        if (!resolve_socket(handle_v, &s)) {
            error("net.accept(): no such connection handle");
            vm_stack_push(vm, aer_null());
            return true;
        }
        /* Bounded, not indefinite: an unbounded accept() would freeze the whole cooperative
           scheduler, not just this actor -- a server script polls by calling accept() again on a
           timeout, exactly like a client script retries connect(). */
        if (!wait_ready(s, false, NET_TIMEOUT_SECONDS)) {
            char msg[64];
            snprintf(msg, sizeof(msg), "net.accept() timed out after %ds", NET_TIMEOUT_SECONDS);
            vm_stack_push(vm, aer_make_result(aer_null(), aer_make_error(msg)));
            return true;
        }
        sock_t conn = accept(s, NULL, NULL);
        if (conn == SOCK_INVALID) {
            vm_stack_push(vm, aer_make_result(aer_null(), aer_make_error(sock_errmsg())));
            return true;
        }
        vm_stack_push(vm, aer_make_result(aer_int((int64_t)register_socket(conn)), aer_null()));
        return true;
    }

    return false;
}
