/*
 * winsock.c - the Winsock entry points, over POSIX sockets.
 *
 * Fully implemented, not stubbed: the game's own LAN/direct-connect multiplayer
 * is 2004-vintage and will be replaced, but the replacement will be built on
 * these same calls, so a real implementation is the useful thing to have.
 *
 * Includes Wine headers only - the POSIX side is behind posix_net.h, which
 * explains why the two cannot share a translation unit.
 *
 * Three things genuinely differ between Winsock and POSIX and are translated
 * here rather than hoped over:
 *
 *  - **Error codes.** Winsock has its own WSAE* space (WSAEWOULDBLOCK = 10035,
 *    not EAGAIN = 11), reported through GetLastError rather than errno. A game
 *    doing non-blocking I/O checks for WSAEWOULDBLOCK constantly, so getting
 *    this wrong means a connection that appears to fail at random.
 *  - **fd_set.** Winsock's is a count plus an array of SOCKETs; POSIX's is a
 *    bitmask. select() below rebuilds the sets from what poll() reports.
 *  - **Socket-option and ioctl constants.** SOL_SOCKET is 0xffff on Winsock and
 *    1 on Linux, SO_REUSEADDR is 4 versus 2, FIONBIO is 0x8004667e versus
 *    0x5421. Nothing may be passed through unmapped.
 *
 * A SOCKET here is the file descriptor, so it is directly comparable and
 * INVALID_SOCKET (~0) can never collide with a real one.
 */
/* Before every Wine header, and before winsock2.h in particular. */
#include <nfsu2/win32_dllmacros.h>

/*
 * winsock2.h must come before windows.h (which win32_compat.h pulls in):
 * windows.h includes the *Winsock 1* winsock.h, and the two then collide on
 * struct sockaddr, SOCKADDR_IN, the IPPROTO_* enum and more. winsock2.h defines
 * _WINSOCKAPI_ on the way in, which is what stops windows.h reaching for the
 * older header - the same include order the Windows SDK requires.
 */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <nb30.h>

#include <nfsu2/win32_compat.h>
#include <nfsu2/win32_shim.h>

#include "posix_net.h"

#include <stdio.h>
#include <string.h>

/* --- error translation -------------------------------------------------- */

/* Our POSIX helpers return -errno; these are the errno values we translate.
 * Spelled out numerically because <errno.h> cannot be included here (it is
 * safe, but keeping this file free of system headers is the rule that stops the
 * winsock/POSIX collision from creeping back in). */
#define E_PERM        1
#define E_INTR        4
#define E_IO          5
#define E_BADF        9
#define E_AGAIN       11
#define E_NOMEM       12
#define E_ACCES       13
#define E_FAULT       14
#define E_BUSY        16
#define E_INVAL       22
#define E_MFILE       24
#define E_NOSPC       28
#define E_PIPE        32
#define E_NAMETOOLONG 36
#define E_NOENT       2
#define E_WOULDBLOCK  11
#define E_INPROGRESS  115
#define E_ALREADY     114
#define E_NOTSOCK     88
#define E_DESTADDRREQ 89
#define E_MSGSIZE     90
#define E_PROTOTYPE   91
#define E_NOPROTOOPT  92
#define E_PROTONOSUP  93
#define E_SOCKTNOSUP  94
#define E_OPNOTSUPP   95
#define E_AFNOSUP     97
#define E_ADDRINUSE   98
#define E_ADDRNOTAVAIL 99
#define E_NETDOWN     100
#define E_NETUNREACH  101
#define E_NETRESET    102
#define E_CONNABORTED 103
#define E_CONNRESET   104
#define E_NOBUFS      105
#define E_ISCONN      106
#define E_NOTCONN     107
#define E_TIMEDOUT    110
#define E_CONNREFUSED 111
#define E_HOSTDOWN    112
#define E_HOSTUNREACH 113

static int wsa_error_from_errno(int err)
{
    switch (err) {
    case 0:              return 0;
    case E_INTR:         return WSAEINTR;
    case E_BADF:         return WSAEBADF;
    case E_ACCES:
    case E_PERM:         return WSAEACCES;
    case E_FAULT:        return WSAEFAULT;
    case E_INVAL:        return WSAEINVAL;
    case E_MFILE:        return WSAEMFILE;
    case E_AGAIN:        return WSAEWOULDBLOCK;
    case E_INPROGRESS:   return WSAEINPROGRESS;
    case E_ALREADY:      return WSAEALREADY;
    case E_NOTSOCK:      return WSAENOTSOCK;
    case E_DESTADDRREQ:  return WSAEDESTADDRREQ;
    case E_MSGSIZE:      return WSAEMSGSIZE;
    case E_PROTOTYPE:    return WSAEPROTOTYPE;
    case E_NOPROTOOPT:   return WSAENOPROTOOPT;
    case E_PROTONOSUP:   return WSAEPROTONOSUPPORT;
    case E_SOCKTNOSUP:   return WSAESOCKTNOSUPPORT;
    case E_OPNOTSUPP:    return WSAEOPNOTSUPP;
    case E_AFNOSUP:      return WSAEAFNOSUPPORT;
    case E_ADDRINUSE:    return WSAEADDRINUSE;
    case E_ADDRNOTAVAIL: return WSAEADDRNOTAVAIL;
    case E_NETDOWN:      return WSAENETDOWN;
    case E_NETUNREACH:   return WSAENETUNREACH;
    case E_NETRESET:     return WSAENETRESET;
    case E_CONNABORTED:  return WSAECONNABORTED;
    case E_PIPE:
    case E_CONNRESET:    return WSAECONNRESET;
    case E_NOBUFS:
    case E_NOMEM:        return WSAENOBUFS;
    case E_ISCONN:       return WSAEISCONN;
    case E_NOTCONN:      return WSAENOTCONN;
    case E_TIMEDOUT:     return WSAETIMEDOUT;
    case E_CONNREFUSED:  return WSAECONNREFUSED;
    case E_HOSTDOWN:     return WSAEHOSTDOWN;
    case E_HOSTUNREACH:  return WSAEHOSTUNREACH;
    case E_NAMETOOLONG:  return WSAENAMETOOLONG;
    case E_NOENT:        return WSAHOST_NOT_FOUND;
    case E_BUSY:
    case E_IO:
    case E_NOSPC:
    default:             return WSAEFAULT;
    }
}

/* Record a failed helper result (-errno) as the Winsock last error, then
 * return SOCKET_ERROR so call sites stay one-liners. */
static int fail_with(int negative_errno)
{
    SetLastError((DWORD)wsa_error_from_errno(-negative_errno));
    return SOCKET_ERROR;
}

static int g_startup_count;

int WINAPI WSAGetLastError(void)
{
    /* Winsock stores its error in the same per-thread slot as GetLastError;
     * this is not a separate variable on Windows either. */
    return (int)GetLastError();
}

void WINAPI WSASetLastError(int error)
{
    SetLastError((DWORD)error);
}

int WINAPI WSAStartup(WORD version, LPWSADATA data)
{
    if (!data)
        return WSAEFAULT;

    memset(data, 0, sizeof(*data));
    /*
     * Report back the version the caller asked for when we can support it. The
     * game requests 1.1; anything up to 2.2 is fine here because nothing in the
     * feature set we implement is version-dependent.
     */
    if (LOBYTE(version) > 2 || (LOBYTE(version) == 2 && HIBYTE(version) > 2)) {
        nfsu2_shim_trace("WSAStartup: unsupported version %d.%d",
                         LOBYTE(version), HIBYTE(version));
        return WSAVERNOTSUPPORTED;
    }
    data->wVersion = version;
    data->wHighVersion = MAKEWORD(2, 2);
    snprintf(data->szDescription, sizeof(data->szDescription),
             "nfsu2-unwrap native sockets");
    snprintf(data->szSystemStatus, sizeof(data->szSystemStatus), "Running");
    data->iMaxSockets = 0; /* zero means "no limit" for Winsock 2 */
    data->iMaxUdpDg = 0;

    g_startup_count++;
    return 0;
}

int WINAPI WSACleanup(void)
{
    if (g_startup_count <= 0) {
        SetLastError(WSANOTINITIALISED);
        return SOCKET_ERROR;
    }
    g_startup_count--;
    /* Open sockets are deliberately *not* closed: Winsock leaves that to the
     * process, and closing them here would break a caller that cleans up in the
     * wrong order (which is common, and harmless on Windows). */
    return 0;
}

/* --- constant translation ----------------------------------------------- */

static int net_family(int af)
{
    switch (af) {
    case AF_INET:  return NFSU2_NET_AF_INET;
    case AF_INET6: return NFSU2_NET_AF_INET6;
    default:       return NFSU2_NET_AF_UNSPEC;
    }
}

static int net_type(int type)
{
    switch (type) {
    case SOCK_STREAM: return NFSU2_NET_SOCK_STREAM;
    case SOCK_DGRAM:  return NFSU2_NET_SOCK_DGRAM;
    case SOCK_RAW:    return NFSU2_NET_SOCK_RAW;
    default:          return -1;
    }
}

static int net_protocol(int protocol)
{
    switch (protocol) {
    case IPPROTO_TCP: return NFSU2_NET_IPPROTO_TCP;
    case IPPROTO_UDP: return NFSU2_NET_IPPROTO_UDP;
    default:          return NFSU2_NET_IPPROTO_IP;
    }
}

static int net_msg_flags(int flags)
{
    int out = 0;

    if (flags & MSG_OOB)
        out |= NFSU2_NET_MSG_OOB;
    if (flags & MSG_PEEK)
        out |= NFSU2_NET_MSG_PEEK;
    if (flags & MSG_DONTROUTE)
        out |= NFSU2_NET_MSG_DONTROUTE;
    return out;
}

/* Winsock (level, optname) -> our option id, or -1. */
static int net_option(int level, int optname)
{
    if (level == SOL_SOCKET) {
        switch (optname) {
        case SO_REUSEADDR: return NFSU2_NET_OPT_REUSEADDR;
        case SO_BROADCAST: return NFSU2_NET_OPT_BROADCAST;
        case SO_KEEPALIVE: return NFSU2_NET_OPT_KEEPALIVE;
        case SO_LINGER:    return NFSU2_NET_OPT_LINGER;
        case SO_SNDBUF:    return NFSU2_NET_OPT_SNDBUF;
        case SO_RCVBUF:    return NFSU2_NET_OPT_RCVBUF;
        case SO_SNDTIMEO:  return NFSU2_NET_OPT_SNDTIMEO;
        case SO_RCVTIMEO:  return NFSU2_NET_OPT_RCVTIMEO;
        case SO_DONTROUTE: return NFSU2_NET_OPT_DONTROUTE;
        case SO_OOBINLINE: return NFSU2_NET_OPT_OOBINLINE;
        case SO_ERROR:     return NFSU2_NET_OPT_ERROR;
        case SO_TYPE:      return NFSU2_NET_OPT_TYPE;
        default:           return -1;
        }
    }
    if (level == IPPROTO_TCP && optname == TCP_NODELAY)
        return NFSU2_NET_OPT_TCP_NODELAY;
    if (level == IPPROTO_IP) {
        switch (optname) {
        case IP_MULTICAST_TTL:   return NFSU2_NET_OPT_IP_MULTICAST_TTL;
        case IP_MULTICAST_LOOP:  return NFSU2_NET_OPT_IP_MULTICAST_LOOP;
        case IP_ADD_MEMBERSHIP:  return NFSU2_NET_OPT_IP_ADD_MEMBERSHIP;
        case IP_DROP_MEMBERSHIP: return NFSU2_NET_OPT_IP_DROP_MEMBERSHIP;
        case IP_TTL:             return NFSU2_NET_OPT_IP_TTL;
        default:                 return -1;
        }
    }
    return -1;
}

/* --- socket lifecycle --------------------------------------------------- */

SOCKET WINAPI socket(int af, int type, int protocol)
{
    int mapped_type = net_type(type);
    int fd;

    if (g_startup_count <= 0) {
        /* Winsock refuses everything before WSAStartup; keeping that behaviour
         * means a missing WSAStartup fails here rather than somewhere subtler. */
        SetLastError(WSANOTINITIALISED);
        return INVALID_SOCKET;
    }
    if (mapped_type < 0) {
        SetLastError(WSAESOCKTNOSUPPORT);
        return INVALID_SOCKET;
    }

    fd = nfsu2_net_socket(net_family(af), mapped_type, net_protocol(protocol));
    if (fd < 0) {
        SetLastError((DWORD)wsa_error_from_errno(-fd));
        return INVALID_SOCKET;
    }
    return (SOCKET)fd;
}

int WINAPI closesocket(SOCKET s)
{
    int rc = nfsu2_net_close((int)s);

    return rc < 0 ? fail_with(rc) : 0;
}

int WINAPI bind(SOCKET s, const struct sockaddr *addr, int addr_len)
{
    int rc = nfsu2_net_bind((int)s, addr, addr_len);

    return rc < 0 ? fail_with(rc) : 0;
}

int WINAPI listen(SOCKET s, int backlog)
{
    int rc;

    /* SOMAXCONN is a sentinel on Winsock ("whatever the stack allows"), not a
     * usable backlog value. */
    if (backlog == SOMAXCONN || backlog <= 0)
        backlog = 128;
    rc = nfsu2_net_listen((int)s, backlog);
    return rc < 0 ? fail_with(rc) : 0;
}

SOCKET WINAPI accept(SOCKET s, struct sockaddr *addr, int *addr_len)
{
    int fd = nfsu2_net_accept((int)s, addr, addr_len);

    if (fd < 0) {
        SetLastError((DWORD)wsa_error_from_errno(-fd));
        return INVALID_SOCKET;
    }
    return (SOCKET)fd;
}

int WINAPI connect(SOCKET s, const struct sockaddr *addr, int addr_len)
{
    int rc = nfsu2_net_connect((int)s, addr, addr_len);

    if (rc < 0) {
        /*
         * A non-blocking connect reports EINPROGRESS on POSIX but
         * WSAEWOULDBLOCK on Winsock, and callers poll for writability after
         * seeing exactly that code.
         */
        if (-rc == E_INPROGRESS) {
            SetLastError(WSAEWOULDBLOCK);
            return SOCKET_ERROR;
        }
        return fail_with(rc);
    }
    return 0;
}

int WINAPI shutdown(SOCKET s, int how)
{
    /* SD_RECEIVE/SD_SEND/SD_BOTH are 0/1/2, same order as our helper wants. */
    int rc = nfsu2_net_shutdown((int)s, how);

    return rc < 0 ? fail_with(rc) : 0;
}

/* --- transfer ----------------------------------------------------------- */

int WINAPI send(SOCKET s, const char *buf, int len, int flags)
{
    int rc = nfsu2_net_send((int)s, buf, len, net_msg_flags(flags));

    return rc < 0 ? fail_with(rc) : rc;
}

int WINAPI recv(SOCKET s, char *buf, int len, int flags)
{
    int rc = nfsu2_net_recv((int)s, buf, len, net_msg_flags(flags));

    return rc < 0 ? fail_with(rc) : rc;
}

int WINAPI sendto(SOCKET s, const char *buf, int len, int flags,
                  const struct sockaddr *addr, int addr_len)
{
    int rc = nfsu2_net_sendto((int)s, buf, len, net_msg_flags(flags), addr, addr_len);

    return rc < 0 ? fail_with(rc) : rc;
}

int WINAPI recvfrom(SOCKET s, char *buf, int len, int flags,
                    struct sockaddr *addr, int *addr_len)
{
    int rc = nfsu2_net_recvfrom((int)s, buf, len, net_msg_flags(flags), addr, addr_len);

    return rc < 0 ? fail_with(rc) : rc;
}

/* --- names and options -------------------------------------------------- */

int WINAPI getsockname(SOCKET s, struct sockaddr *addr, int *addr_len)
{
    int rc = nfsu2_net_getsockname((int)s, addr, addr_len);

    return rc < 0 ? fail_with(rc) : 0;
}

int WINAPI getpeername(SOCKET s, struct sockaddr *addr, int *addr_len)
{
    int rc = nfsu2_net_getpeername((int)s, addr, addr_len);

    return rc < 0 ? fail_with(rc) : 0;
}

int WINAPI setsockopt(SOCKET s, int level, int optname, const char *value, int value_len)
{
    int option = net_option(level, optname);
    int rc;

    if (option < 0) {
        nfsu2_shim_trace("setsockopt(level=%d, opt=%d): unsupported", level, optname);
        SetLastError(WSAENOPROTOOPT);
        return SOCKET_ERROR;
    }
    rc = nfsu2_net_setsockopt((int)s, option, value, value_len);
    return rc < 0 ? fail_with(rc) : 0;
}

int WINAPI getsockopt(SOCKET s, int level, int optname, char *value, int *value_len)
{
    int option = net_option(level, optname);
    int rc;

    if (option < 0) {
        nfsu2_shim_trace("getsockopt(level=%d, opt=%d): unsupported", level, optname);
        SetLastError(WSAENOPROTOOPT);
        return SOCKET_ERROR;
    }
    rc = nfsu2_net_getsockopt((int)s, option, value, value_len);
    return rc < 0 ? fail_with(rc) : 0;
}

int WINAPI ioctlsocket(SOCKET s, LONG command, u_long *argp)
{
    int rc;

    switch (command) {
    case FIONBIO:
        if (!argp) {
            SetLastError(WSAEFAULT);
            return SOCKET_ERROR;
        }
        rc = nfsu2_net_set_nonblocking((int)s, *argp != 0);
        return rc < 0 ? fail_with(rc) : 0;
    case FIONREAD: {
        unsigned long available = 0;
        if (!argp) {
            SetLastError(WSAEFAULT);
            return SOCKET_ERROR;
        }
        rc = nfsu2_net_bytes_available((int)s, &available);
        if (rc < 0)
            return fail_with(rc);
        *argp = (u_long)available;
        return 0;
    }
    default:
        nfsu2_shim_trace("ioctlsocket(0x%lx): unsupported command", (unsigned long)command);
        SetLastError(WSAEINVAL);
        return SOCKET_ERROR;
    }
}

/* --- select ------------------------------------------------------------- */

static int set_contains(const fd_set *set, SOCKET s)
{
    u_int i;

    if (!set)
        return 0;
    for (i = 0; i < set->fd_count; i++) {
        if (set->fd_array[i] == s)
            return 1;
    }
    return 0;
}

/*
 * FD_ISSET is a macro over this exported function on Winsock, not a bit test -
 * a consequence of fd_set being a count plus an array. Callers get it purely
 * through the macro, but the symbol has to exist to link.
 */
int WINAPI __WSAFDIsSet(SOCKET s, fd_set *set)
{
    return set_contains(set, s);
}

int WINAPI select(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds,
                  const struct timeval *timeout)
{
    int fds[FD_SETSIZE];
    unsigned char events[FD_SETSIZE];
    unsigned char revents[FD_SETSIZE];
    int count = 0;
    int timeout_ms;
    int ready;
    int i;
    fd_set read_out, write_out, except_out;

    (void)nfds; /* Winsock ignores it; the sets carry their own counts */

    /* Build one poll array from the union of the three sets. */
    for (i = 0; i < 3; i++) {
        const fd_set *set = (i == 0) ? readfds : (i == 1 ? writefds : exceptfds);
        unsigned char bit = (i == 0) ? NFSU2_NET_POLL_READ
                                     : (i == 1 ? NFSU2_NET_POLL_WRITE : NFSU2_NET_POLL_ERROR);
        u_int j;

        if (!set)
            continue;
        for (j = 0; j < set->fd_count; j++) {
            int fd = (int)set->fd_array[j];
            int existing = -1;
            int k;

            for (k = 0; k < count; k++) {
                if (fds[k] == fd) {
                    existing = k;
                    break;
                }
            }
            if (existing >= 0) {
                events[existing] |= bit;
            } else if (count < FD_SETSIZE) {
                fds[count] = fd;
                events[count] = bit;
                count++;
            } else {
                SetLastError(WSAEINVAL);
                return SOCKET_ERROR;
            }
        }
    }

    if (count == 0) {
        /* Winsock rejects a select with no sockets in any set rather than using
         * it as a sleep. */
        SetLastError(WSAEINVAL);
        return SOCKET_ERROR;
    }

    timeout_ms = timeout ? (int)(timeout->tv_sec * 1000 + timeout->tv_usec / 1000) : -1;

    ready = nfsu2_net_poll(fds, events, revents, count, timeout_ms);
    if (ready < 0)
        return fail_with(ready);

    /* Rebuild the sets, keeping only what is ready - which is what select's
     * contract requires and what callers iterate over afterwards. */
    FD_ZERO(&read_out);
    FD_ZERO(&write_out);
    FD_ZERO(&except_out);

    ready = 0;
    for (i = 0; i < count; i++) {
        SOCKET s = (SOCKET)fds[i];

        if ((revents[i] & NFSU2_NET_POLL_READ) && set_contains(readfds, s)) {
            FD_SET(s, &read_out);
            ready++;
        }
        if ((revents[i] & NFSU2_NET_POLL_WRITE) && set_contains(writefds, s)) {
            FD_SET(s, &write_out);
            ready++;
        }
        if ((revents[i] & NFSU2_NET_POLL_ERROR) && set_contains(exceptfds, s)) {
            FD_SET(s, &except_out);
            ready++;
        }
    }

    if (readfds)
        *readfds = read_out;
    if (writefds)
        *writefds = write_out;
    if (exceptfds)
        *exceptfds = except_out;
    return ready;
}

/* --- name resolution ---------------------------------------------------- */

struct hostent *WINAPI gethostbyname(const char *name)
{
    /*
     * gethostbyname's contract is a pointer to storage owned by the library and
     * valid until the next call *on the same thread*, so this is thread-local
     * rather than static-shared.
     */
    static __thread struct hostent entry;
    static __thread char canonical[256];
    static __thread unsigned int addresses[8];
    static __thread char *address_list[9];
    static __thread char *alias_list[1];
    int count;
    int i;

    if (!name) {
        SetLastError(WSAEFAULT);
        return NULL;
    }

    count = nfsu2_net_resolve_ipv4(name, addresses, 8, canonical, sizeof(canonical));
    if (count < 0) {
        SetLastError(-count == E_NOENT ? WSAHOST_NOT_FOUND : WSATRY_AGAIN);
        return NULL;
    }

    for (i = 0; i < count; i++)
        address_list[i] = (char *)&addresses[i];
    address_list[count] = NULL;
    alias_list[0] = NULL;

    entry.h_name = canonical;
    entry.h_aliases = alias_list;
    entry.h_addrtype = AF_INET;
    entry.h_length = 4;
    entry.h_addr_list = address_list;
    return &entry;
}

/* --- WSAIoctl ----------------------------------------------------------- */

int WINAPI WSAIoctl(SOCKET s, DWORD code, LPVOID in_buffer, DWORD in_size,
                    LPVOID out_buffer, DWORD out_size, LPDWORD bytes_returned,
                    LPWSAOVERLAPPED overlapped, LPWSAOVERLAPPED_COMPLETION_ROUTINE completion)
{
    (void)s; (void)in_buffer; (void)in_size;

    if (overlapped || completion) {
        NFSU2_STUB("WSAIoctl with overlapped completion");
        SetLastError(WSAEOPNOTSUPP);
        return SOCKET_ERROR;
    }

    switch (code) {
    case SIO_GET_INTERFACE_LIST: {
        /*
         * This is why the game imports WSAIoctl: enumerating local adapters to
         * pick one to broadcast LAN game announcements on. Worth implementing
         * properly rather than stubbing, since the replacement multiplayer will
         * want exactly this.
         */
        struct nfsu2_net_interface interfaces[16];
        INTERFACE_INFO *out = out_buffer;
        DWORD capacity = out_size / (DWORD)sizeof(INTERFACE_INFO);
        int count;
        int i;

        if (!out_buffer || !bytes_returned) {
            SetLastError(WSAEFAULT);
            return SOCKET_ERROR;
        }
        count = nfsu2_net_list_interfaces(interfaces, 16);
        if (count < 0)
            return fail_with(count);
        if ((DWORD)count > capacity) {
            *bytes_returned = (DWORD)count * (DWORD)sizeof(INTERFACE_INFO);
            SetLastError(WSAEFAULT); /* what Winsock reports for a short buffer */
            return SOCKET_ERROR;
        }

        memset(out, 0, (size_t)count * sizeof(INTERFACE_INFO));
        for (i = 0; i < count; i++) {
            struct sockaddr_in *address = (struct sockaddr_in *)&out[i].iiAddress.AddressIn;
            struct sockaddr_in *netmask = (struct sockaddr_in *)&out[i].iiNetmask.AddressIn;
            struct sockaddr_in *broadcast =
                (struct sockaddr_in *)&out[i].iiBroadcastAddress.AddressIn;

            out[i].iiFlags = 0;
            if (interfaces[i].is_up)
                out[i].iiFlags |= IFF_UP;
            if (interfaces[i].is_loopback)
                out[i].iiFlags |= IFF_LOOPBACK;
            if (interfaces[i].supports_broadcast)
                out[i].iiFlags |= IFF_BROADCAST;
            if (interfaces[i].is_point_to_point)
                out[i].iiFlags |= IFF_POINTTOPOINT;
            if (interfaces[i].supports_multicast)
                out[i].iiFlags |= IFF_MULTICAST;

            address->sin_family = AF_INET;
            address->sin_addr.S_un.S_addr = interfaces[i].address;
            netmask->sin_family = AF_INET;
            netmask->sin_addr.S_un.S_addr = interfaces[i].netmask;
            broadcast->sin_family = AF_INET;
            broadcast->sin_addr.S_un.S_addr = interfaces[i].broadcast;
        }
        *bytes_returned = (DWORD)count * (DWORD)sizeof(INTERFACE_INFO);
        return 0;
    }
    default:
        nfsu2_shim_trace("WSAIoctl(0x%08lx): unsupported code", (unsigned long)code);
        SetLastError(WSAEOPNOTSUPP);
        return SOCKET_ERROR;
    }
}

/* --- byte order and address helpers ------------------------------------- */

/*
 * Not in the game's import list (its CRT inlines them), but a sockets layer
 * without them is not usable from ported code, and they cost nothing.
 */
u_short WINAPI htons(u_short value)
{
    return (u_short)((value << 8) | (value >> 8));
}

u_short WINAPI ntohs(u_short value)
{
    return htons(value);
}

u_long WINAPI htonl(u_long value)
{
    return ((value & 0x000000fful) << 24) | ((value & 0x0000ff00ul) << 8) |
           ((value & 0x00ff0000ul) >> 8) | ((value & 0xff000000ul) >> 24);
}

u_long WINAPI ntohl(u_long value)
{
    return htonl(value);
}

u_long WINAPI inet_addr(const char *text)
{
    unsigned int parts[4];
    int i;

    if (!text)
        return INADDR_NONE;
    if (sscanf(text, "%u.%u.%u.%u", &parts[0], &parts[1], &parts[2], &parts[3]) != 4)
        return INADDR_NONE;
    for (i = 0; i < 4; i++) {
        if (parts[i] > 255)
            return INADDR_NONE;
    }
    /* Network byte order, i.e. first octet in the low byte on a little-endian
     * host. */
    return (u_long)(parts[0] | (parts[1] << 8) | (parts[2] << 16) |
                    ((unsigned long)parts[3] << 24));
}

char *WINAPI inet_ntoa(struct in_addr addr)
{
    static __thread char text[16];
    unsigned long value = addr.S_un.S_addr;

    snprintf(text, sizeof(text), "%lu.%lu.%lu.%lu",
             value & 0xff, (value >> 8) & 0xff, (value >> 16) & 0xff, (value >> 24) & 0xff);
    return text;
}

/* --- NetBIOS ------------------------------------------------------------ */

UCHAR WINAPI Netbios(PNCB ncb)
{
    /*
     * NetBIOS was the other 2004 LAN transport. Nothing implements it on Linux,
     * and the replacement multiplayer will not use it. Reporting "no such name"
     * rather than a generic failure is what makes the game's own code conclude
     * there is no NetBIOS network and move on.
     */
    NFSU2_STUB("Netbios (no NetBIOS transport)");
    if (ncb)
        ncb->ncb_retcode = NRC_NOWILD;
    return NRC_NOWILD;
}
