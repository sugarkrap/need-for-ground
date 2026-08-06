/*
 * shim_selftest.c - exercises the Win32 shim without needing a GPU, a
 * display, or any game data. Runs in a temporary directory it creates itself.
 *
 * These are the behaviours that are easy to get subtly wrong and painful to
 * debug later from inside game code: case-insensitive path resolution,
 * short-read semantics at EOF, seek/size agreement, and the WINAPI ->
 * pthread convention boundary in CreateThread.
 */
#include <nfsu2/win32_compat.h>
#include <nfsu2/win32_shim.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

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

static char g_root[] = "/tmp/nfsu2-shim-selftest-XXXXXX";

static void build_fixture(void)
{
    char path[512];
    FILE *f;

    if (!mkdtemp(g_root)) {
        perror("mkdtemp");
        exit(1);
    }
    snprintf(path, sizeof(path), "%s/data", g_root);
    mkdir(path, 0755);
    snprintf(path, sizeof(path), "%s/data/frontend", g_root);
    mkdir(path, 0755);

    /* Lower-case on disk; the game will ask for upper-case. */
    snprintf(path, sizeof(path), "%s/data/frontend/fe_art.bun", g_root);
    f = fopen(path, "wb");
    fwrite("HELLO-BUN", 1, 9, f);
    fclose(f);
}

static void cleanup_fixture(void)
{
    char cmd[600];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", g_root);
    if (system(cmd) != 0)
        fprintf(stderr, "warning: could not clean up %s\n", g_root);
}

static DWORD WINAPI thread_body(LPVOID param)
{
    LONG volatile *counter = param;

    InterlockedIncrement(counter);
    return 0x1234;
}

int main(void)
{
    char host[4096];
    HANDLE h, ev, thread;
    WIN32_FIND_DATAA find_data;
    char buf[32];
    DWORD n, tls, exit_code;
    LONG counter = 0;
    CRITICAL_SECTION cs;
    LARGE_INTEGER freq, t0, t1;
    void *mem;

    build_fixture();
    CHECK(nfsu2_win32_init(g_root) == 0, "shim init with root %s", g_root);

    /* --- paths --------------------------------------------------------- */
    CHECK(nfsu2_path_to_host("DATA\\FRONTEND\\FE_ART.BUN", host, sizeof(host)) == 0 &&
          strstr(host, "data/frontend/fe_art.bun") != NULL,
          "case-insensitive translation of DATA\\FRONTEND\\FE_ART.BUN -> %s", host);
    CHECK(nfsu2_path_to_host("C:\\Data\\FrontEnd\\fe_art.BUN", host, sizeof(host)) == 0,
          "drive letter is stripped and mapped to the game root");
    CHECK(nfsu2_path_to_host("DATA\\NOPE\\FILE.BIN", host, sizeof(host)) != 0,
          "missing intermediate directory fails");
    CHECK(nfsu2_path_to_host("DATA\\NEWFILE.BIN", host, sizeof(host)) == 0,
          "missing final component is allowed (file creation)");

    /* --- file I/O ------------------------------------------------------ */
    h = CreateFileA("DATA\\FRONTEND\\FE_ART.BUN", GENERIC_READ, FILE_SHARE_READ,
                    NULL, OPEN_EXISTING, 0, NULL);
    CHECK(h != INVALID_HANDLE_VALUE, "CreateFileA on a wrong-case path");
    CHECK(GetFileSize(h, NULL) == 9, "GetFileSize == 9");
    memset(buf, 0, sizeof(buf));
    CHECK(ReadFile(h, buf, 5, &n, NULL) && n == 5 && memcmp(buf, "HELLO", 5) == 0,
          "ReadFile of 5 bytes");
    CHECK(SetFilePointer(h, -4, NULL, FILE_END) == 5, "SetFilePointer(FILE_END, -4) == 5");
    CHECK(ReadFile(h, buf, sizeof(buf), &n, NULL) && n == 4,
          "ReadFile past EOF succeeds with a short count (%lu)", (unsigned long)n);
    CHECK(CloseHandle(h), "CloseHandle");

    h = CreateFileA("DATA\\WRITTEN.TMP", GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    CHECK(h != INVALID_HANDLE_VALUE, "CreateFileA(CREATE_ALWAYS) creates a new file");
    CHECK(WriteFile(h, "xyz", 3, &n, NULL) && n == 3, "WriteFile");
    CloseHandle(h);
    CHECK(GetFileAttributesA("DATA\\written.tmp") != INVALID_FILE_ATTRIBUTES,
          "GetFileAttributesA finds the new file case-insensitively");
    CHECK(DeleteFileA("DATA\\WRITTEN.TMP"), "DeleteFileA");

    /* --- enumeration --------------------------------------------------- */
    h = FindFirstFileA("DATA\\FRONTEND\\*.BUN", &find_data);
    CHECK(h != INVALID_HANDLE_VALUE && strcasecmp(find_data.cFileName, "fe_art.bun") == 0,
          "FindFirstFileA(*.BUN) -> %s", h != INVALID_HANDLE_VALUE ? find_data.cFileName : "(none)");
    CHECK(!FindNextFileA(h, &find_data) && GetLastError() == ERROR_NO_MORE_FILES,
          "FindNextFileA reports ERROR_NO_MORE_FILES at the end");
    FindClose(h);

    /* --- heap / virtual memory ----------------------------------------- */
    mem = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, 256);
    CHECK(mem != NULL && ((char *)mem)[128] == 0, "HeapAlloc(HEAP_ZERO_MEMORY) zeroes");
    CHECK(HeapFree(GetProcessHeap(), 0, mem), "HeapFree");
    mem = VirtualAlloc(NULL, 0x10000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    CHECK(mem != NULL, "VirtualAlloc(MEM_COMMIT|MEM_RESERVE)");
    if (mem) {
        memset(mem, 0xaa, 0x10000);
        CHECK(VirtualFree(mem, 0x10000, MEM_DECOMMIT), "VirtualFree(MEM_DECOMMIT)");
    }

    /* --- timing -------------------------------------------------------- */
    CHECK(QueryPerformanceFrequency(&freq) && freq.QuadPart > 0, "QueryPerformanceFrequency");
    QueryPerformanceCounter(&t0);
    Sleep(20);
    QueryPerformanceCounter(&t1);
    CHECK(t1.QuadPart > t0.QuadPart, "QueryPerformanceCounter advances across Sleep(20)");
    CHECK(GetTickCount() >= 20, "GetTickCount is at least the time we slept");

    /* --- sync / TLS / threads ------------------------------------------ */
    InitializeCriticalSection(&cs);
    EnterCriticalSection(&cs);
    EnterCriticalSection(&cs); /* must be recursive, like Win32 */
    LeaveCriticalSection(&cs);
    LeaveCriticalSection(&cs);
    DeleteCriticalSection(&cs);
    printf("ok   - critical section is recursive\n");

    tls = TlsAlloc();
    CHECK(tls != TLS_OUT_OF_INDEXES, "TlsAlloc");
    CHECK(TlsSetValue(tls, (LPVOID)0xdeadbeef) && TlsGetValue(tls) == (LPVOID)0xdeadbeef,
          "TLS round-trip");
    TlsFree(tls);

    ev = CreateEventA(NULL, TRUE, FALSE, NULL);
    CHECK(WaitForSingleObject(ev, 0) == WAIT_TIMEOUT, "unsignalled event times out");
    SetEvent(ev);
    CHECK(WaitForSingleObject(ev, 0) == WAIT_OBJECT_0, "signalled manual-reset event");
    CHECK(WaitForSingleObject(ev, 0) == WAIT_OBJECT_0, "manual-reset event stays signalled");
    ResetEvent(ev);
    CHECK(WaitForSingleObject(ev, 0) == WAIT_TIMEOUT, "ResetEvent clears it");
    CloseHandle(ev);

    thread = CreateThread(NULL, 0, thread_body, &counter, 0, NULL);
    CHECK(thread != NULL, "CreateThread with a WINAPI entry point");
    CHECK(WaitForSingleObject(thread, 5000) == WAIT_OBJECT_0, "thread finished");
    CHECK(GetExitCodeThread(thread, &exit_code) && exit_code == 0x1234,
          "thread exit code survives the convention boundary");
    CHECK(counter == 1, "thread ran its body exactly once");
    CloseHandle(thread);

    nfsu2_win32_shutdown();
    cleanup_fixture();

    printf("\n%s (%d failure%s)\n", g_failures ? "FAILED" : "PASSED",
           g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
