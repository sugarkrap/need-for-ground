/*
 * posix_net.c - the POSIX half of the sockets shim.
 *
 * Includes system headers only, never a Wine header - see posix_net.h for why
 * the two cannot meet in one translation unit.
 */
#include "posix_net.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

/*
 * Every libc socket call has to go through dlsym(RTLD_NEXT), not by name.
 *
 * ws2_32/winsock.c defines `socket`, `bind`, `send`, `recv` and ten more,
 * because those *are* the Winsock API names. Those definitions are global
 * symbols in the same program, so the linker resolves this file's reference to
 * `socket` to the Winsock one rather than to libc's - and nfsu2_net_socket()
 * calls the Win32 socket(), which calls nfsu2_net_socket(), until the stack
 * runs out. It presents as an immediate SIGSEGV on the first socket() call with
 * no useful backtrace, which is a memorable afternoon.
 *
 * RTLD_NEXT resolves past the caller's own object, i.e. straight to libc, and
 * documents the intent at the same time. The lookups are cached, so the cost is
 * one pointer indirection per call.
 *
 * Only the fourteen colliding names need this. poll, fcntl, ioctl, close,
 * getaddrinfo and getifaddrs have no Winsock counterpart here and are called
 * normally.
 */
struct libc_net {
    int (*socket)(int, int, int);
    int (*bind)(int, const struct sockaddr *, socklen_t);
    int (*listen)(int, int);
    int (*accept)(int, struct sockaddr *, socklen_t *);
    int (*connect)(int, const struct sockaddr *, socklen_t);
    int (*shutdown)(int, int);
    ssize_t (*send)(int, const void *, size_t, int);
    ssize_t (*recv)(int, void *, size_t, int);
    ssize_t (*sendto)(int, const void *, size_t, int, const struct sockaddr *, socklen_t);
    ssize_t (*recvfrom)(int, void *, size_t, int, struct sockaddr *, socklen_t *);
    int (*getsockname)(int, struct sockaddr *, socklen_t *);
    int (*getpeername)(int, struct sockaddr *, socklen_t *);
    int (*setsockopt)(int, int, int, const void *, socklen_t);
    int (*getsockopt)(int, int, int, void *, socklen_t *);
    int resolved;
};

static struct libc_net g_libc;
static pthread_once_t g_libc_once = PTHREAD_ONCE_INIT;

static void resolve_libc(void)
{
    /* This file must not include Wine headers (see posix_net.h), so it cannot
     * use nfsu2_shim_trace; stderr is the only channel available here. */
#define RESOLVE(name)                                                              \
    do {                                                                           \
        *(void **)&g_libc.name = dlsym(RTLD_NEXT, #name);                          \
        if (!g_libc.name) {                                                        \
            fprintf(stderr, "[nfsu2/net] cannot resolve libc %s: %s\n", #name,     \
                    dlerror() ? dlerror() : "not found");                          \
            return;                                                                \
        }                                                                          \
    } while (0)

    RESOLVE(socket);
    RESOLVE(bind);
    RESOLVE(listen);
    RESOLVE(accept);
    RESOLVE(connect);
    RESOLVE(shutdown);
    RESOLVE(send);
    RESOLVE(recv);
    RESOLVE(sendto);
    RESOLVE(recvfrom);
    RESOLVE(getsockname);
    RESOLVE(getpeername);
    RESOLVE(setsockopt);
    RESOLVE(getsockopt);
#undef RESOLVE

    g_libc.resolved = 1;
}

static const struct libc_net *libc(void)
{
    pthread_once(&g_libc_once, resolve_libc);
    return g_libc.resolved ? &g_libc : NULL;
}

/* Bail out of a helper when the libc lookups failed. */
#define LIBC_OR_FAIL()                          \
    const struct libc_net *sys = libc();        \
    if (!sys)                                   \
        return -ENOSYS

static int fail(void)
{
    /* Never return a bare -1: the caller wants the reason, and errno may not
     * survive whatever it does next. */
    return errno ? -errno : -EIO;
}

static int family_of(int family)
{
    switch (family) {
    case NFSU2_NET_AF_INET:  return AF_INET;
    case NFSU2_NET_AF_INET6: return AF_INET6;
    default:                 return AF_UNSPEC;
    }
}

static int type_of(int type)
{
    switch (type) {
    case NFSU2_NET_SOCK_STREAM: return SOCK_STREAM;
    case NFSU2_NET_SOCK_DGRAM:  return SOCK_DGRAM;
    case NFSU2_NET_SOCK_RAW:    return SOCK_RAW;
    default:                    return -1;
    }
}

static int protocol_of(int protocol)
{
    switch (protocol) {
    case NFSU2_NET_IPPROTO_TCP: return IPPROTO_TCP;
    case NFSU2_NET_IPPROTO_UDP: return IPPROTO_UDP;
    default:                    return 0;
    }
}

int nfsu2_net_socket(int family, int type, int protocol)
{
    LIBC_OR_FAIL();
    int host_type = type_of(type);
    int fd;

    if (host_type < 0)
        return -EPROTOTYPE;

    fd = sys->socket(family_of(family), host_type, protocol_of(protocol));
    if (fd < 0)
        return fail();

    /*
     * Winsock sockets are not inherited by child processes unless asked for,
     * and nothing here forks anyway; close-on-exec avoids leaking listeners
     * into anything the game launches.
     */
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    return fd;
}

int nfsu2_net_close(int fd)
{
    return close(fd) < 0 ? fail() : 0;
}

int nfsu2_net_bind(int fd, const void *addr, int addr_len)
{
    LIBC_OR_FAIL();
    return sys->bind(fd, addr, (socklen_t)addr_len) < 0 ? fail() : 0;
}

int nfsu2_net_listen(int fd, int backlog)
{
    LIBC_OR_FAIL();
    return sys->listen(fd, backlog) < 0 ? fail() : 0;
}

int nfsu2_net_accept(int fd, void *addr, int *addr_len)
{
    LIBC_OR_FAIL();
    socklen_t len = addr_len ? (socklen_t)*addr_len : 0;
    int result = sys->accept(fd, addr, addr ? &len : NULL);

    if (result < 0)
        return fail();
    if (addr_len)
        *addr_len = (int)len;
    fcntl(result, F_SETFD, FD_CLOEXEC);
    return result;
}

int nfsu2_net_connect(int fd, const void *addr, int addr_len)
{
    LIBC_OR_FAIL();
    return sys->connect(fd, addr, (socklen_t)addr_len) < 0 ? fail() : 0;
}

int nfsu2_net_shutdown(int fd, int how)
{
    LIBC_OR_FAIL();
    int host_how = (how == 0) ? SHUT_RD : (how == 1 ? SHUT_WR : SHUT_RDWR);

    return sys->shutdown(fd, host_how) < 0 ? fail() : 0;
}

static int msg_flags_of(int flags)
{
    int host = 0;

    if (flags & NFSU2_NET_MSG_OOB)
        host |= MSG_OOB;
    if (flags & NFSU2_NET_MSG_PEEK)
        host |= MSG_PEEK;
    if (flags & NFSU2_NET_MSG_DONTROUTE)
        host |= MSG_DONTROUTE;
    /*
     * MSG_NOSIGNAL is added unconditionally: on Windows, writing to a socket
     * whose peer has gone away returns WSAECONNRESET, it does not raise a
     * signal. Without this, SIGPIPE would kill the process on a dropped
     * connection - a difference that would only show up under real network
     * failure, which is the worst time to discover it.
     */
    return host | MSG_NOSIGNAL;
}

int nfsu2_net_send(int fd, const void *buf, int len, int flags)
{
    LIBC_OR_FAIL();
    ssize_t sent = sys->send(fd, buf, (size_t)len, msg_flags_of(flags));

    return sent < 0 ? fail() : (int)sent;
}

int nfsu2_net_recv(int fd, void *buf, int len, int flags)
{
    LIBC_OR_FAIL();
    ssize_t got = sys->recv(fd, buf, (size_t)len, msg_flags_of(flags));

    return got < 0 ? fail() : (int)got;
}

int nfsu2_net_sendto(int fd, const void *buf, int len, int flags,
                     const void *addr, int addr_len)
{
    LIBC_OR_FAIL();
    ssize_t sent = sys->sendto(fd, buf, (size_t)len, msg_flags_of(flags), addr,
                               (socklen_t)addr_len);

    return sent < 0 ? fail() : (int)sent;
}

int nfsu2_net_recvfrom(int fd, void *buf, int len, int flags, void *addr, int *addr_len)
{
    LIBC_OR_FAIL();
    socklen_t sock_len = addr_len ? (socklen_t)*addr_len : 0;
    ssize_t got = sys->recvfrom(fd, buf, (size_t)len, msg_flags_of(flags),
                                addr, addr ? &sock_len : NULL);

    if (got < 0)
        return fail();
    if (addr_len)
        *addr_len = (int)sock_len;
    return (int)got;
}

int nfsu2_net_getsockname(int fd, void *addr, int *addr_len)
{
    LIBC_OR_FAIL();
    socklen_t len = addr_len ? (socklen_t)*addr_len : 0;

    if (sys->getsockname(fd, addr, &len) < 0)
        return fail();
    if (addr_len)
        *addr_len = (int)len;
    return 0;
}

int nfsu2_net_getpeername(int fd, void *addr, int *addr_len)
{
    LIBC_OR_FAIL();
    socklen_t len = addr_len ? (socklen_t)*addr_len : 0;

    if (sys->getpeername(fd, addr, &len) < 0)
        return fail();
    if (addr_len)
        *addr_len = (int)len;
    return 0;
}

/* Map our option id onto (level, name). Returns -1 if unsupported. */
static int option_of(int option, int *level, int *name)
{
    *level = SOL_SOCKET;
    switch (option) {
    case NFSU2_NET_OPT_REUSEADDR: *name = SO_REUSEADDR; return 0;
    case NFSU2_NET_OPT_BROADCAST: *name = SO_BROADCAST; return 0;
    case NFSU2_NET_OPT_KEEPALIVE: *name = SO_KEEPALIVE; return 0;
    case NFSU2_NET_OPT_LINGER:    *name = SO_LINGER;    return 0;
    case NFSU2_NET_OPT_SNDBUF:    *name = SO_SNDBUF;    return 0;
    case NFSU2_NET_OPT_RCVBUF:    *name = SO_RCVBUF;    return 0;
    case NFSU2_NET_OPT_SNDTIMEO:  *name = SO_SNDTIMEO;  return 0;
    case NFSU2_NET_OPT_RCVTIMEO:  *name = SO_RCVTIMEO;  return 0;
    case NFSU2_NET_OPT_DONTROUTE: *name = SO_DONTROUTE; return 0;
    case NFSU2_NET_OPT_OOBINLINE: *name = SO_OOBINLINE; return 0;
    case NFSU2_NET_OPT_ERROR:     *name = SO_ERROR;     return 0;
    case NFSU2_NET_OPT_TYPE:      *name = SO_TYPE;      return 0;
    case NFSU2_NET_OPT_TCP_NODELAY:
        *level = IPPROTO_TCP;
        *name = TCP_NODELAY;
        return 0;
    case NFSU2_NET_OPT_IP_MULTICAST_TTL:
        *level = IPPROTO_IP;
        *name = IP_MULTICAST_TTL;
        return 0;
    case NFSU2_NET_OPT_IP_MULTICAST_LOOP:
        *level = IPPROTO_IP;
        *name = IP_MULTICAST_LOOP;
        return 0;
    case NFSU2_NET_OPT_IP_ADD_MEMBERSHIP:
        *level = IPPROTO_IP;
        *name = IP_ADD_MEMBERSHIP;
        return 0;
    case NFSU2_NET_OPT_IP_DROP_MEMBERSHIP:
        *level = IPPROTO_IP;
        *name = IP_DROP_MEMBERSHIP;
        return 0;
    case NFSU2_NET_OPT_IP_TTL:
        *level = IPPROTO_IP;
        *name = IP_TTL;
        return 0;
    default:
        return -1;
    }
}

int nfsu2_net_setsockopt(int fd, int option, const void *value, int value_len)
{
    LIBC_OR_FAIL();
    int level, name;

    if (option_of(option, &level, &name) != 0)
        return -ENOPROTOOPT;

    /*
     * SO_SNDTIMEO/SO_RCVTIMEO take a DWORD of milliseconds on Winsock and a
     * struct timeval here, so they need converting rather than passing through.
     */
    if (option == NFSU2_NET_OPT_SNDTIMEO || option == NFSU2_NET_OPT_RCVTIMEO) {
        struct timeval tv;
        unsigned int ms;

        if (!value || value_len < (int)sizeof(unsigned int))
            return -EINVAL;
        memcpy(&ms, value, sizeof(ms));
        tv.tv_sec = (time_t)(ms / 1000);
        tv.tv_usec = (suseconds_t)(ms % 1000) * 1000;
        return sys->setsockopt(fd, level, name, &tv, sizeof(tv)) < 0 ? fail() : 0;
    }

    return sys->setsockopt(fd, level, name, value, (socklen_t)value_len) < 0 ? fail() : 0;
}

int nfsu2_net_getsockopt(int fd, int option, void *value, int *value_len)
{
    LIBC_OR_FAIL();
    int level, name;
    socklen_t len;

    if (option_of(option, &level, &name) != 0)
        return -ENOPROTOOPT;
    if (!value_len)
        return -EINVAL;

    if (option == NFSU2_NET_OPT_SNDTIMEO || option == NFSU2_NET_OPT_RCVTIMEO) {
        struct timeval tv;
        unsigned int ms;

        len = sizeof(tv);
        if (sys->getsockopt(fd, level, name, &tv, &len) < 0)
            return fail();
        if (*value_len < (int)sizeof(ms))
            return -EINVAL;
        ms = (unsigned int)(tv.tv_sec * 1000 + tv.tv_usec / 1000);
        memcpy(value, &ms, sizeof(ms));
        *value_len = (int)sizeof(ms);
        return 0;
    }

    len = (socklen_t)*value_len;
    if (sys->getsockopt(fd, level, name, value, &len) < 0)
        return fail();
    *value_len = (int)len;
    return 0;
}

int nfsu2_net_set_nonblocking(int fd, int nonblocking)
{
    int flags = fcntl(fd, F_GETFL, 0);

    if (flags < 0)
        return fail();
    flags = nonblocking ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    return fcntl(fd, F_SETFL, flags) < 0 ? fail() : 0;
}

int nfsu2_net_bytes_available(int fd, unsigned long *out)
{
    int available = 0;

    if (ioctl(fd, FIONREAD, &available) < 0)
        return fail();
    if (out)
        *out = (unsigned long)available;
    return 0;
}

int nfsu2_net_poll(const int *fds, const unsigned char *events, unsigned char *revents_out,
                   int count, int timeout_ms)
{
    struct pollfd stack_fds[64];
    struct pollfd *poll_fds = stack_fds;
    int ready;
    int i;

    if (count < 0)
        return -EINVAL;
    if (count == 0)
        return 0;
    if ((size_t)count > sizeof(stack_fds) / sizeof(stack_fds[0]))
        return -EINVAL; /* Winsock's FD_SETSIZE is 64, so this cannot happen */

    for (i = 0; i < count; i++) {
        poll_fds[i].fd = fds[i];
        poll_fds[i].events = 0;
        poll_fds[i].revents = 0;
        if (events[i] & NFSU2_NET_POLL_READ)
            poll_fds[i].events |= POLLIN;
        if (events[i] & NFSU2_NET_POLL_WRITE)
            poll_fds[i].events |= POLLOUT;
        if (events[i] & NFSU2_NET_POLL_ERROR)
            poll_fds[i].events |= POLLPRI;
    }

    do {
        ready = poll(poll_fds, (nfds_t)count, timeout_ms);
    } while (ready < 0 && errno == EINTR);

    if (ready < 0)
        return fail();

    for (i = 0; i < count; i++) {
        unsigned char out = 0;

        /*
         * POLLHUP counts as readable: select() on Windows reports a closed
         * connection as ready-to-read so the caller's recv() returns 0. Mapping
         * it to the error set instead would make a normal disconnect look like
         * a failure.
         */
        if (poll_fds[i].revents & (POLLIN | POLLHUP))
            out |= NFSU2_NET_POLL_READ;
        if (poll_fds[i].revents & POLLOUT)
            out |= NFSU2_NET_POLL_WRITE;
        if (poll_fds[i].revents & (POLLERR | POLLNVAL | POLLPRI))
            out |= NFSU2_NET_POLL_ERROR;
        revents_out[i] = out;
    }
    return ready;
}

int nfsu2_net_resolve_ipv4(const char *name, unsigned int *addrs, int max_addrs,
                           char *canonical_name, size_t canonical_size)
{
    struct addrinfo hints;
    struct addrinfo *results = NULL;
    struct addrinfo *cursor;
    int count = 0;
    int rc;

    if (!name || !addrs || max_addrs <= 0)
        return -EINVAL;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET; /* the game speaks IPv4 only */
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_CANONNAME;

    rc = getaddrinfo(name, NULL, &hints, &results);
    if (rc != 0) {
        /* EAI_* codes are their own space; collapse to the two cases a caller
         * can act on. */
        return (rc == EAI_NONAME || rc == EAI_NODATA) ? -ENOENT : -EAGAIN;
    }

    if (canonical_name && canonical_size) {
        const char *canonical = results->ai_canonname ? results->ai_canonname : name;
        snprintf(canonical_name, canonical_size, "%s", canonical);
    }

    for (cursor = results; cursor && count < max_addrs; cursor = cursor->ai_next) {
        if (cursor->ai_family != AF_INET)
            continue;
        addrs[count++] = ((struct sockaddr_in *)cursor->ai_addr)->sin_addr.s_addr;
    }
    freeaddrinfo(results);
    return count ? count : -ENOENT;
}

int nfsu2_net_list_interfaces(struct nfsu2_net_interface *out, int max_count)
{
    struct ifaddrs *list = NULL;
    struct ifaddrs *cursor;
    int count = 0;

    if (!out || max_count <= 0)
        return -EINVAL;
    if (getifaddrs(&list) != 0)
        return fail();

    for (cursor = list; cursor && count < max_count; cursor = cursor->ifa_next) {
        struct nfsu2_net_interface *entry;
        unsigned int address, netmask;

        if (!cursor->ifa_addr || cursor->ifa_addr->sa_family != AF_INET)
            continue;

        address = ((struct sockaddr_in *)cursor->ifa_addr)->sin_addr.s_addr;
        netmask = cursor->ifa_netmask
                      ? ((struct sockaddr_in *)cursor->ifa_netmask)->sin_addr.s_addr
                      : 0xffffffffu;

        entry = &out[count++];
        entry->address = address;
        entry->netmask = netmask;
        /* Derive the broadcast address when the interface does not report one:
         * a LAN game's discovery ping needs it and an all-ones fallback would
         * go to the wrong subnet. */
        if (cursor->ifa_broadaddr && (cursor->ifa_flags & IFF_BROADCAST))
            entry->broadcast = ((struct sockaddr_in *)cursor->ifa_broadaddr)->sin_addr.s_addr;
        else
            entry->broadcast = address | ~netmask;

        entry->is_up = (cursor->ifa_flags & IFF_UP) ? 1 : 0;
        entry->is_loopback = (cursor->ifa_flags & IFF_LOOPBACK) ? 1 : 0;
        entry->supports_broadcast = (cursor->ifa_flags & IFF_BROADCAST) ? 1 : 0;
        entry->is_point_to_point = (cursor->ifa_flags & IFF_POINTOPOINT) ? 1 : 0;
        entry->supports_multicast = (cursor->ifa_flags & IFF_MULTICAST) ? 1 : 0;
    }
    freeifaddrs(list);
    return count;
}
