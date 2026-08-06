/*
 * posix_net.h - the POSIX sockets API, renamed.
 *
 * Wine's <winsock2.h> and glibc's <sys/socket.h> cannot coexist in one
 * translation unit: both declare `bind`, `socket`, `select` and friends, with
 * different calling conventions and different struct types, and both define
 * `fd_set`, `struct sockaddr` and the AF_/SOCK_/SO_ constants. Wine's
 * USE_WS_PREFIX only renames the types, not the functions, so it does not save
 * us either.
 *
 * So the POSIX side lives in its own file (posix_net.c) which includes only
 * system headers and exposes everything under nfsu2_net_* names with plain int /
 * void* types. winsock.c then includes only Wine's headers and does the
 * translation. The split is the whole reason this header exists.
 *
 * Address structures are passed through as raw bytes on purpose:
 * `struct sockaddr_in` has the identical layout on both sides (2-byte family,
 * 2-byte big-endian port, 4-byte address, 8 bytes of padding), so no conversion
 * is needed. Only the *constants* differ, and those are translated explicitly
 * in winsock.c.
 */
#ifndef NFSU2_POSIX_NET_H
#define NFSU2_POSIX_NET_H

#include <stddef.h>

/* Our own address-family / type / protocol values, translated in winsock.c to
 * whatever the host actually uses. Kept separate from both worlds' constants so
 * neither header's values can leak in by accident. */
enum {
    NFSU2_NET_AF_INET = 1,
    NFSU2_NET_AF_INET6 = 2,
    NFSU2_NET_AF_UNSPEC = 3
};

enum {
    NFSU2_NET_SOCK_STREAM = 1,
    NFSU2_NET_SOCK_DGRAM = 2,
    NFSU2_NET_SOCK_RAW = 3
};

enum {
    NFSU2_NET_IPPROTO_IP = 0,
    NFSU2_NET_IPPROTO_TCP = 1,
    NFSU2_NET_IPPROTO_UDP = 2
};

/* Socket-option identifiers we support, host-independent. */
enum {
    NFSU2_NET_OPT_REUSEADDR = 1,
    NFSU2_NET_OPT_BROADCAST,
    NFSU2_NET_OPT_KEEPALIVE,
    NFSU2_NET_OPT_LINGER,
    NFSU2_NET_OPT_SNDBUF,
    NFSU2_NET_OPT_RCVBUF,
    NFSU2_NET_OPT_SNDTIMEO,
    NFSU2_NET_OPT_RCVTIMEO,
    NFSU2_NET_OPT_DONTROUTE,
    NFSU2_NET_OPT_OOBINLINE,
    NFSU2_NET_OPT_ERROR,
    NFSU2_NET_OPT_TYPE,
    NFSU2_NET_OPT_TCP_NODELAY,
    NFSU2_NET_OPT_IP_MULTICAST_TTL,
    NFSU2_NET_OPT_IP_MULTICAST_LOOP,
    NFSU2_NET_OPT_IP_ADD_MEMBERSHIP,
    NFSU2_NET_OPT_IP_DROP_MEMBERSHIP,
    NFSU2_NET_OPT_IP_TTL
};

/* Poll event bits. */
#define NFSU2_NET_POLL_READ  0x01
#define NFSU2_NET_POLL_WRITE 0x02
#define NFSU2_NET_POLL_ERROR 0x04

/* Message flags for send/recv. */
#define NFSU2_NET_MSG_OOB       0x01
#define NFSU2_NET_MSG_PEEK      0x02
#define NFSU2_NET_MSG_DONTROUTE 0x04

/*
 * Each call returns >= 0 on success, or -errno on failure (never -1 with a
 * separate errno), so the caller has the failure reason without a second call
 * and without worrying about errno being clobbered in between.
 */
int nfsu2_net_socket(int family, int type, int protocol);
int nfsu2_net_close(int fd);
int nfsu2_net_bind(int fd, const void *addr, int addr_len);
int nfsu2_net_listen(int fd, int backlog);
int nfsu2_net_accept(int fd, void *addr, int *addr_len);
int nfsu2_net_connect(int fd, const void *addr, int addr_len);
int nfsu2_net_shutdown(int fd, int how); /* 0 = read, 1 = write, 2 = both */
int nfsu2_net_send(int fd, const void *buf, int len, int flags);
int nfsu2_net_recv(int fd, void *buf, int len, int flags);
int nfsu2_net_sendto(int fd, const void *buf, int len, int flags,
                     const void *addr, int addr_len);
int nfsu2_net_recvfrom(int fd, void *buf, int len, int flags, void *addr, int *addr_len);
int nfsu2_net_getsockname(int fd, void *addr, int *addr_len);
int nfsu2_net_getpeername(int fd, void *addr, int *addr_len);
int nfsu2_net_setsockopt(int fd, int option, const void *value, int value_len);
int nfsu2_net_getsockopt(int fd, int option, void *value, int *value_len);
int nfsu2_net_set_nonblocking(int fd, int nonblocking);
int nfsu2_net_bytes_available(int fd, unsigned long *out);

/*
 * poll() rather than select(): the fd sets on the two sides have incompatible
 * representations (Winsock is a count plus an array, POSIX is a bitmask), and
 * going through poll means winsock.c only has to rebuild its own arrays.
 * `timeout_ms` < 0 blocks indefinitely.
 */
int nfsu2_net_poll(const int *fds, const unsigned char *events, unsigned char *revents_out,
                   int count, int timeout_ms);

/*
 * Resolve a host name to IPv4 addresses. Writes up to `max_addrs` addresses in
 * network byte order and returns how many, or -errno. `canonical_name` receives
 * the resolved canonical name if non-NULL.
 */
int nfsu2_net_resolve_ipv4(const char *name, unsigned int *addrs, int max_addrs,
                           char *canonical_name, size_t canonical_size);

/* One IPv4 interface, for the interface-enumeration ioctl. */
struct nfsu2_net_interface {
    unsigned int address;   /* network byte order */
    unsigned int netmask;   /* network byte order */
    unsigned int broadcast; /* network byte order */
    int is_up;
    int is_loopback;
    int supports_broadcast;
    int is_point_to_point;
    int supports_multicast;
};

/* Enumerate IPv4 interfaces. Returns how many were written, or -errno. */
int nfsu2_net_list_interfaces(struct nfsu2_net_interface *out, int max_count);

#endif /* NFSU2_POSIX_NET_H */
