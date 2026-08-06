/*
 * services_selftest.c - registry, sockets, locale and GDI checks.
 *
 * The sockets half runs a real TCP and a real UDP conversation over the
 * loopback interface, plus a non-blocking connect and a select() - because the
 * three things most likely to be wrong in a Winsock shim (the WSAE* error
 * space, the count-plus-array fd_set, and the FIONBIO ioctl) only misbehave once
 * data is actually moving.
 */
#include <winsock2.h>
#include <ws2tcpip.h>

#include <mmsystem.h>

#include <nfsu2/win32_compat.h>
#include <nfsu2/win32_shim.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/*
 * No <unistd.h> here: it typedefs socklen_t, which Wine's winsock2.h also
 * defines, and the two collide. Anything from unistd would have to go in a
 * separate translation unit - the same rule that forced the posix_net.c split.
 */

static int g_failures;

#define CHECK(cond, ...)                                                        \
    do {                                                                        \
        if (cond) {                                                             \
            printf("ok   - " __VA_ARGS__);                                      \
            printf("\n");                                                       \
        } else {                                                                \
            printf("FAIL - " __VA_ARGS__);                                      \
            printf("  (%s:%d, GetLastError=%lu)\n", __FILE__, __LINE__,         \
                   (unsigned long)GetLastError());                              \
            g_failures++;                                                       \
        }                                                                       \
    } while (0)

static char g_root[] = "/tmp/nfsu2-services-selftest-XXXXXX";

/* --- registry ----------------------------------------------------------- */

static void test_registry(void)
{
    HKEY key = NULL;
    DWORD type = 0;
    DWORD size;
    DWORD dword_value;
    char text[128];
    char store[512];
    struct stat st;

    printf("\n# registry\n");

    CHECK(RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\EA GAMES\\Nope", 0, KEY_READ, &key) ==
              ERROR_FILE_NOT_FOUND,
          "RegOpenKeyExA on a missing key returns ERROR_FILE_NOT_FOUND");

    CHECK(RegCreateKeyA(HKEY_LOCAL_MACHINE, "SOFTWARE\\EA GAMES\\NFSU2", &key) == ERROR_SUCCESS &&
              key != NULL,
          "RegCreateKeyA creates a key");

    dword_value = 0x040c;
    CHECK(RegSetValueExA(key, "Language", 0, REG_DWORD, (const BYTE *)&dword_value,
                         sizeof(dword_value)) == ERROR_SUCCESS,
          "RegSetValueExA(REG_DWORD)");
    CHECK(RegSetValueExA(key, "InstallDir", 0, REG_SZ, (const BYTE *)g_root,
                         (DWORD)strlen(g_root) + 1) == ERROR_SUCCESS,
          "RegSetValueExA(REG_SZ)");

    size = sizeof(dword_value);
    dword_value = 0;
    CHECK(RegQueryValueExA(key, "Language", NULL, &type, (LPBYTE)&dword_value, &size) ==
              ERROR_SUCCESS && type == REG_DWORD && dword_value == 0x040c,
          "RegQueryValueExA reads the DWORD back with its type");

    size = sizeof(text);
    CHECK(RegQueryValueExA(key, "installdir", NULL, &type, (LPBYTE)text, &size) ==
              ERROR_SUCCESS && type == REG_SZ && strcmp(text, g_root) == 0,
          "value names are case-insensitive, like Windows");

    size = 0;
    CHECK(RegQueryValueExA(key, "InstallDir", NULL, NULL, NULL, &size) == ERROR_SUCCESS &&
              size == strlen(g_root) + 1,
          "a NULL data pointer returns the required size (%lu)", (unsigned long)size);

    size = 2;
    CHECK(RegQueryValueExA(key, "InstallDir", NULL, NULL, (LPBYTE)text, &size) ==
              ERROR_MORE_DATA && size == strlen(g_root) + 1,
          "a short buffer returns ERROR_MORE_DATA and the needed size");

    size = sizeof(text);
    CHECK(RegQueryValueExA(key, "Missing", NULL, NULL, (LPBYTE)text, &size) ==
              ERROR_FILE_NOT_FOUND,
          "an unset value returns ERROR_FILE_NOT_FOUND");

    CHECK(RegCloseKey(key) == ERROR_SUCCESS, "RegCloseKey");
    CHECK(RegCloseKey(HKEY_LOCAL_MACHINE) == ERROR_SUCCESS,
          "closing a predefined root is a no-op, not an error");

    snprintf(store, sizeof(store), "%s/registry.ini", g_root);
    CHECK(stat(store, &st) == 0 && st.st_size > 0,
          "the store was written to %s", store);

    /* Reopening must find what was written, which is the whole point. */
    CHECK(RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\EA GAMES\\NFSU2", 0, KEY_READ, &key) ==
              ERROR_SUCCESS,
          "RegOpenKeyExA finds the created key");
    size = sizeof(dword_value);
    CHECK(RegQueryValueExA(key, "Language", NULL, NULL, (LPBYTE)&dword_value, &size) ==
              ERROR_SUCCESS && dword_value == 0x040c,
          "the value survives a close/reopen");
    RegCloseKey(key);
}

/* --- locale ------------------------------------------------------------- */

static void test_locale(void)
{
    WCHAR wide[64];
    char narrow[64];
    WORD types[8];
    CPINFO cp_info;
    int count;

    printf("\n# locale / codepages\n");

    CHECK(GetACP() == 1252, "GetACP reports Windows-1252");
    CHECK(IsValidCodePage(1252) && IsValidCodePage(CP_UTF8) && !IsValidCodePage(12345),
          "IsValidCodePage accepts what we support and rejects the rest");
    CHECK(GetCPInfo(CP_ACP, &cp_info) && cp_info.MaxCharSize == 1 &&
              cp_info.DefaultChar[0] == '?',
          "GetCPInfo(CP_ACP)");

    count = MultiByteToWideChar(CP_ACP, 0, "Hi", -1, wide, 64);
    CHECK(count == 3 && wide[0] == 'H' && wide[1] == 'i' && wide[2] == 0,
          "MultiByteToWideChar with -1 length includes the terminator (%d)", count);

    /* 0x93 is a left double quote in CP1252 (U+201C) but a control character in
     * Latin-1 - the classic way to get this wrong. */
    count = MultiByteToWideChar(1252, 0, "\x93", 1, wide, 64);
    CHECK(count == 1 && wide[0] == 0x201C,
          "CP1252 0x93 maps to U+201C, not to a control character");

    count = WideCharToMultiByte(1252, 0, wide, 1, narrow, 64, NULL, NULL);
    CHECK(count == 1 && (unsigned char)narrow[0] == 0x93, "and it round-trips back to 0x93");

    wide[0] = 0x20AC; /* euro sign */
    count = WideCharToMultiByte(CP_UTF8, 0, wide, 1, narrow, 64, NULL, NULL);
    CHECK(count == 3 && (unsigned char)narrow[0] == 0xe2 && (unsigned char)narrow[1] == 0x82 &&
              (unsigned char)narrow[2] == 0xac,
          "UTF-8 encoding of U+20AC is three bytes");

    count = MultiByteToWideChar(CP_ACP, 0, "abc", -1, NULL, 0);
    CHECK(count == 4, "a zero destination size returns the required length");

    CHECK(CompareStringA(0, NORM_IGNORECASE, "Nitro", -1, "NITRO", -1) == CSTR_EQUAL,
          "CompareStringA(NORM_IGNORECASE)");
    CHECK(CompareStringA(0, 0, "a", -1, "b", -1) == CSTR_LESS_THAN, "CompareStringA orders");

    count = LCMapStringA(0, LCMAP_UPPERCASE, "drift", -1, narrow, sizeof(narrow));
    CHECK(count == 6 && strcmp(narrow, "DRIFT") == 0, "LCMapStringA(LCMAP_UPPERCASE)");

    CHECK(GetStringTypeA(0, CT_CTYPE1, "a1 ", 3, types) &&
              (types[0] & C1_LOWER) && (types[1] & C1_DIGIT) && (types[2] & C1_SPACE),
          "GetStringTypeA classifies letters, digits and spaces");

    CHECK(GetLocaleInfoA(0, LOCALE_SDECIMAL, narrow, sizeof(narrow)) == 2 &&
              strcmp(narrow, ".") == 0,
          "GetLocaleInfoA reports '.' as the decimal separator");

    CHECK(lstrcmpiA("Underground", "UNDERGROUND") == 0, "lstrcmpiA");
    count = wsprintfA(narrow, "%s %d", "lap", 3);
    CHECK(count == 5 && strcmp(narrow, "lap 3") == 0, "wsprintfA");
    wsprintfA(narrow, "%04x|%-4d|%u|%c", 0xbeef, 7, 4000000000u, 'X');
    CHECK(strcmp(narrow, "beef|7   |4000000000|X") == 0,
          "wsprintfA handles width, zero-pad, left-align, %%u and %%c (got \"%s\")", narrow);
    wsprintfA(narrow, "%f", 1.5);
    CHECK(strcmp(narrow, "%f") == 0,
          "wsprintfA leaves %%f literal, as Windows does (no float support)");

    {
        LPSTR block = GetEnvironmentStringsA();
        CHECK(block != NULL && block[0] != '\0', "GetEnvironmentStringsA returns a block");
        CHECK(FreeEnvironmentStringsA(block), "FreeEnvironmentStringsA");
    }
}

/* --- gdi ---------------------------------------------------------------- */

static void test_gdi(void)
{
    HDC dc;
    HBITMAP bitmap;
    HFONT font;
    HGDIOBJ previous;

    printf("\n# gdi32\n");

    dc = CreateCompatibleDC(NULL);
    CHECK(dc != NULL, "CreateCompatibleDC");
    bitmap = CreateBitmap(64, 32, 1, 32, NULL);
    CHECK(bitmap != NULL, "CreateBitmap");
    font = CreateFontA(-12, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
                       DEFAULT_PITCH, "Arial");
    CHECK(font != NULL, "CreateFontA");

    previous = SelectObject(dc, bitmap);
    CHECK(previous == NULL, "SelectObject returns NULL when nothing was selected");
    CHECK(SelectObject(dc, font) == NULL, "selecting a font into the same DC");
    CHECK(!DeleteObject(bitmap), "DeleteObject refuses a bitmap that is still selected");

    CHECK(SetTextColor(dc, 0x00ff00ff) == 0x00ffffff, "SetTextColor returns the previous colour");
    CHECK(SetBkMode(dc, TRANSPARENT) == OPAQUE, "SetBkMode returns the previous mode");
    CHECK(BitBlt(dc, 0, 0, 64, 32, NULL, 0, 0, BLACKNESS), "BitBlt(BLACKNESS)");
    CHECK(ExtTextOutA(dc, 0, 0, 0, NULL, "hud", 3, NULL), "ExtTextOutA is accepted");
    CHECK(GetPixel(dc, 1, 1) == CLR_INVALID,
          "GetPixel reports CLR_INVALID rather than inventing a colour");

    CHECK(DeleteDC(dc), "DeleteDC");
    CHECK(DeleteObject(bitmap), "the bitmap can be deleted once its DC is gone");
    CHECK(DeleteObject(font), "DeleteObject(font)");
}

/* --- file mapping and multimedia timers --------------------------------- */

static volatile LONG g_timer_ticks;

static void CALLBACK timer_tick(UINT id, UINT msg, DWORD_PTR user, DWORD_PTR r1, DWORD_PTR r2)
{
    (void)id; (void)msg; (void)r1; (void)r2;
    /* The user value is checked, not just the count: timeSetEvent's callback is
     * WINAPI while the timer thread is a pthread, so this crosses the same
     * convention boundary as CreateThread's entry point. */
    if (user == 0xfeed)
        InterlockedIncrement(&g_timer_ticks);
}

static void test_mapping_and_timers(void)
{
    HANDLE file, mapping;
    void *view;
    DWORD written;
    UINT timer;
    LONG at_kill;

    printf("\n# file mapping / mm timers\n");

    /* Mapping is how the game streams its .BUN archives, so this is a real
     * implementation over mmap rather than a stub. */
    file = CreateFileA("MAPTEST.BIN", GENERIC_READ | GENERIC_WRITE, 0, NULL,
                       CREATE_ALWAYS, 0, NULL);
    CHECK(file != INVALID_HANDLE_VALUE, "CreateFileA for the mapping fixture");
    CHECK(WriteFile(file, "STREAMED-ARCHIVE-DATA", 21, &written, NULL) && written == 21,
          "wrote the fixture payload");

    mapping = CreateFileMappingA(file, NULL, PAGE_READWRITE, 0, 0, NULL);
    CHECK(mapping != NULL, "CreateFileMappingA with a zero size takes it from the file");

    view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
    CHECK(view != NULL, "MapViewOfFile");
    CHECK(view && memcmp(view, "STREAMED-ARCHIVE-DATA", 21) == 0,
          "the payload reads back through the view");
    CHECK(view && UnmapViewOfFile(view), "UnmapViewOfFile");
    CHECK(view && !UnmapViewOfFile(view) && GetLastError() == ERROR_INVALID_ADDRESS,
          "unmapping twice is rejected rather than unmapping something else");

    /* The mapping must outlive CloseHandle on the file, as on Windows. */
    CloseHandle(file);
    view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
    CHECK(view != NULL && memcmp(view, "STREAMED", 8) == 0,
          "the mapping still works after the file handle is closed");
    if (view)
        UnmapViewOfFile(view);
    CHECK(CloseHandle(mapping), "CloseHandle(mapping)");
    DeleteFileA("MAPTEST.BIN");

    timer = timeSetEvent(10, 1, timer_tick, 0xfeed, TIME_PERIODIC);
    CHECK(timer != 0, "timeSetEvent(10ms, TIME_PERIODIC) returns a non-zero id");
    Sleep(120);
    CHECK(g_timer_ticks >= 8 && g_timer_ticks <= 14,
          "the periodic callback fired %ld times in 120ms (expect ~11)",
          (long)g_timer_ticks);
    at_kill = g_timer_ticks;
    CHECK(timeKillEvent(timer) == TIMERR_NOERROR, "timeKillEvent");
    Sleep(80);
    CHECK(g_timer_ticks <= at_kill + 1,
          "no further callbacks after timeKillEvent (%ld -> %ld)",
          (long)at_kill, (long)g_timer_ticks);
    CHECK(timeKillEvent(timer) == MMSYSERR_INVALPARAM,
          "killing an already-dead timer is rejected");
}

/* --- sockets ------------------------------------------------------------ */

static void test_sockets(void)
{
    WSADATA data;
    SOCKET listener, client, server;
    struct sockaddr_in addr;
    int addr_len;
    char buffer[64];
    u_long nonblocking = 1;
    u_long available = 0;
    fd_set readable;
    struct timeval timeout;
    int sent, got, ready;

    printf("\n# sockets\n");

    /* Before WSAStartup, everything must fail - and say why. */
    CHECK(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP) == INVALID_SOCKET &&
              WSAGetLastError() == WSANOTINITIALISED,
          "socket() before WSAStartup fails with WSANOTINITIALISED");

    CHECK(WSAStartup(MAKEWORD(1, 1), &data) == 0 && data.wVersion == MAKEWORD(1, 1),
          "WSAStartup(1.1) reports back the requested version");

    listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    CHECK(listener != INVALID_SOCKET, "socket(SOCK_STREAM)");

    {
        int reuse = 1;
        CHECK(setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse,
                         sizeof(reuse)) == 0,
              "setsockopt(SO_REUSEADDR) maps onto the host's own constant");
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    addr.sin_port = 0; /* let the kernel choose */
    CHECK(bind(listener, (struct sockaddr *)&addr, sizeof(addr)) == 0, "bind to 127.0.0.1:0");
    CHECK(listen(listener, SOMAXCONN) == 0, "listen(SOMAXCONN)");

    addr_len = sizeof(addr);
    CHECK(getsockname(listener, (struct sockaddr *)&addr, &addr_len) == 0 && addr.sin_port != 0,
          "getsockname reports the assigned port (%u)", ntohs(addr.sin_port));

    /* Non-blocking connect: Winsock reports WSAEWOULDBLOCK, not EINPROGRESS. */
    client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    CHECK(ioctlsocket(client, FIONBIO, &nonblocking) == 0, "ioctlsocket(FIONBIO)");
    {
        int rc = connect(client, (struct sockaddr *)&addr, sizeof(addr));
        CHECK(rc == 0 || (rc == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK),
              "a non-blocking connect returns 0 or WSAEWOULDBLOCK (not EINPROGRESS)");
    }

    /* select() must survive the round trip through poll() and back. */
    FD_ZERO(&readable);
    FD_SET(listener, &readable);
    timeout.tv_sec = 2;
    timeout.tv_usec = 0;
    ready = select(0, &readable, NULL, NULL, &timeout);
    CHECK(ready == 1 && FD_ISSET(listener, &readable),
          "select() reports the listener readable and rebuilds the fd_set");

    addr_len = sizeof(addr);
    server = accept(listener, (struct sockaddr *)&addr, &addr_len);
    CHECK(server != INVALID_SOCKET, "accept");

    sent = send(server, "hello", 5, 0);
    CHECK(sent == 5, "send 5 bytes");

    /* FIONREAD, then a read: both have to agree about how much arrived. */
    for (ready = 0; ready < 200; ready++) {
        if (ioctlsocket(client, FIONREAD, &available) == 0 && available >= 5)
            break;
        Sleep(5);
    }
    CHECK(available == 5, "ioctlsocket(FIONREAD) reports 5 bytes waiting");

    memset(buffer, 0, sizeof(buffer));
    got = recv(client, buffer, sizeof(buffer), 0);
    CHECK(got == 5 && memcmp(buffer, "hello", 5) == 0, "recv gets the payload back");

    /* An empty non-blocking read is WSAEWOULDBLOCK, the code games test for. */
    got = recv(client, buffer, sizeof(buffer), 0);
    CHECK(got == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK,
          "an empty non-blocking recv reports WSAEWOULDBLOCK (10035), not EAGAIN (11)");

    CHECK(shutdown(server, SD_BOTH) == 0, "shutdown");
    CHECK(closesocket(server) == 0 && closesocket(client) == 0 && closesocket(listener) == 0,
          "closesocket");

    /* UDP, which is what the game's LAN discovery actually uses. */
    {
        SOCKET udp_a = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        SOCKET udp_b = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        struct sockaddr_in bound;
        struct sockaddr_in from;
        int from_len = sizeof(from);
        int broadcast = 1;

        CHECK(udp_a != INVALID_SOCKET && udp_b != INVALID_SOCKET, "two UDP sockets");
        CHECK(setsockopt(udp_a, SOL_SOCKET, SO_BROADCAST, (const char *)&broadcast,
                         sizeof(broadcast)) == 0,
              "setsockopt(SO_BROADCAST), as LAN discovery needs");

        memset(&bound, 0, sizeof(bound));
        bound.sin_family = AF_INET;
        bound.sin_addr.s_addr = inet_addr("127.0.0.1");
        CHECK(bind(udp_b, (struct sockaddr *)&bound, sizeof(bound)) == 0, "bind the UDP receiver");
        addr_len = sizeof(bound);
        getsockname(udp_b, (struct sockaddr *)&bound, &addr_len);

        CHECK(sendto(udp_a, "ping", 4, 0, (struct sockaddr *)&bound, sizeof(bound)) == 4,
              "sendto");
        memset(buffer, 0, sizeof(buffer));
        got = recvfrom(udp_b, buffer, sizeof(buffer), 0, (struct sockaddr *)&from, &from_len);
        CHECK(got == 4 && memcmp(buffer, "ping", 4) == 0 &&
                  from.sin_addr.s_addr == inet_addr("127.0.0.1"),
              "recvfrom returns the datagram and the sender address");

        closesocket(udp_a);
        closesocket(udp_b);
    }

    /* Byte order and address formatting. */
    CHECK(htons(0x1234) == 0x3412, "htons swaps");
    CHECK(inet_addr("1.2.3.4") == 0x04030201u, "inet_addr builds network byte order");
    CHECK(strcmp(inet_ntoa(*(struct in_addr *)&(u_long){0x04030201u}), "1.2.3.4") == 0,
          "inet_ntoa formats it back");
    CHECK(inet_addr("999.1.1.1") == INADDR_NONE, "inet_addr rejects an out-of-range octet");

    /* Resolution and the interface list the game uses to pick an adapter. */
    {
        struct hostent *host = gethostbyname("localhost");
        CHECK(host != NULL && host->h_addrtype == AF_INET && host->h_length == 4 &&
                  host->h_addr_list[0] != NULL,
              "gethostbyname(\"localhost\")");
        CHECK(gethostbyname("nfsu2.invalid.example") == NULL &&
                  WSAGetLastError() == WSAHOST_NOT_FOUND,
              "an unresolvable name reports WSAHOST_NOT_FOUND");
    }
    {
        INTERFACE_INFO interfaces[16];
        DWORD returned = 0;
        SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

        CHECK(WSAIoctl(s, SIO_GET_INTERFACE_LIST, NULL, 0, interfaces, sizeof(interfaces),
                       &returned, NULL, NULL) == 0 && returned >= sizeof(INTERFACE_INFO),
              "WSAIoctl(SIO_GET_INTERFACE_LIST) enumerates %lu interface(s)",
              (unsigned long)(returned / sizeof(INTERFACE_INFO)));
        closesocket(s);
    }

    CHECK(WSACleanup() == 0, "WSACleanup");
    CHECK(WSACleanup() == SOCKET_ERROR && WSAGetLastError() == WSANOTINITIALISED,
          "an unbalanced WSACleanup reports WSANOTINITIALISED");
}

int main(void)
{
    char cmd[600];

    /* Unbuffered: if a check crashes the process, the last line printed has to
     * be the one that got us there. */
    setvbuf(stdout, NULL, _IONBF, 0);

    if (!mkdtemp(g_root)) {
        perror("mkdtemp");
        return 1;
    }
    if (nfsu2_win32_init(g_root) != 0) {
        printf("FAIL - shim init\n");
        return 1;
    }

    test_registry();
    test_locale();
    test_gdi();
    test_mapping_and_timers();
    test_sockets();

    nfsu2_win32_shutdown();

    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", g_root);
    if (system(cmd) != 0)
        fprintf(stderr, "warning: could not clean up %s\n", g_root);

    printf("\n%s (%d failure%s, %d-bit)\n", g_failures ? "FAILED" : "PASSED",
           g_failures, g_failures == 1 ? "" : "s", (int)(sizeof(void *) * 8));
    return g_failures ? 1 : 0;
}
