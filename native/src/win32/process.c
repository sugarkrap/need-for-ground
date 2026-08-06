/*
 * process.c - process-level odds and ends: command line, exit, debug output,
 * error dialogs, standard handles, environment, system info.
 */
#include "shim_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char *g_command_line;

void nfsu2_win32_set_command_line(int argc, char **argv)
{
    size_t len = 1;
    int i;

    free(g_command_line);
    for (i = 0; i < argc; i++)
        len += strlen(argv[i]) + 3; /* quotes + space */
    g_command_line = malloc(len);
    if (!g_command_line)
        return;
    g_command_line[0] = '\0';
    for (i = 0; i < argc; i++) {
        if (i)
            strcat(g_command_line, " ");
        strcat(g_command_line, argv[i]);
    }
}

LPSTR WINAPI GetCommandLineA(void)
{
    return g_command_line ? g_command_line : (LPSTR) "";
}

HANDLE WINAPI GetCurrentProcess(void)
{
    return (HANDLE)-1; /* same pseudo-handle Windows uses */
}

DWORD WINAPI GetCurrentProcessId(void)
{
    return (DWORD)getpid();
}

/*
 * Watch the exact parameter types here: on i386 Wine's DWORD is unsigned long
 * and UINT is unsigned int, which C treats as different types even though both
 * are 32 bits wide, so a mismatch is a hard error at -m32 while compiling
 * silently at -m64. ExitProcess takes DWORD, TerminateProcess takes UINT.
 */
VOID WINAPI ExitProcess(DWORD code)
{
    exit((int)code);
}

BOOL WINAPI TerminateProcess(HANDLE proc, UINT code)
{
    (void)proc;
    _exit((int)code);
}

/* --- debug output ------------------------------------------------------ */

VOID WINAPI OutputDebugStringA(LPCSTR text)
{
    if (!text)
        return;
    fputs("[game] ", stderr);
    fputs(text, stderr);
    if (text[0] && text[strlen(text) - 1] != '\n')
        fputc('\n', stderr);
}

VOID WINAPI DebugBreak(void)
{
    __builtin_trap();
}

BOOL WINAPI IsDebuggerPresent(void)
{
    FILE *f = fopen("/proc/self/status", "r");
    char line[256];
    BOOL present = FALSE;

    if (!f)
        return FALSE;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "TracerPid:", 10) == 0) {
            present = atoi(line + 10) != 0;
            break;
        }
    }
    fclose(f);
    return present;
}

int WINAPI MessageBoxA(HWND owner, LPCSTR text, LPCSTR caption, UINT type)
{
    /*
     * No dialog: this only ever shows fatal-error boxes, and losing them to a
     * silent exit is exactly the failure mode that wastes debugging time.
     * Print and answer "OK".
     */
    (void)owner; (void)type;
    fprintf(stderr, "[MessageBox] %s: %s\n", caption ? caption : "", text ? text : "");
    return IDOK;
}

LPTOP_LEVEL_EXCEPTION_FILTER WINAPI SetUnhandledExceptionFilter(
    LPTOP_LEVEL_EXCEPTION_FILTER filter)
{
    /*
     * Deliberately not wired to a signal handler. SEH-based crash handling is
     * a separate piece of work (a real port needs SIGSEGV -> EXCEPTION_RECORD
     * translation), and installing a half-working one hides crashes we want
     * to see natively during the port.
     */
    (void)filter;
    NFSU2_STUB("SetUnhandledExceptionFilter");
    return NULL;
}

UINT WINAPI SetErrorMode(UINT mode)
{
    (void)mode;
    return 0;
}

/* --- standard handles / environment ------------------------------------ */

HANDLE WINAPI GetStdHandle(DWORD which)
{
    /* Handed straight back to WriteFile in CRT paths, which expects one of our
     * file objects; stdio is a better target for those, so report "no
     * console" instead of a handle that would fail the type check later. */
    (void)which;
    NFSU2_STUB("GetStdHandle");
    return INVALID_HANDLE_VALUE;
}

BOOL WINAPI SetStdHandle(DWORD which, HANDLE h)
{
    (void)which; (void)h;
    return TRUE;
}

DWORD WINAPI GetEnvironmentVariableA(LPCSTR name, LPSTR buf, DWORD size)
{
    const char *v = name ? getenv(name) : NULL;
    DWORD need;

    if (!v) {
        SetLastError(ERROR_ENVVAR_NOT_FOUND);
        return 0;
    }
    need = (DWORD)strlen(v) + 1;
    if (!buf || size < need)
        return need;
    memcpy(buf, v, need);
    return need - 1;
}

BOOL WINAPI SetEnvironmentVariableA(LPCSTR name, LPCSTR value)
{
    if (!name) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (!value)
        return unsetenv(name) == 0;
    return setenv(name, value, 1) == 0;
}

/* --- system info ------------------------------------------------------- */

VOID WINAPI GetSystemInfo(LPSYSTEM_INFO info)
{
    long cpus = sysconf(_SC_NPROCESSORS_ONLN);
    long page = sysconf(_SC_PAGESIZE);

    if (!info)
        return;
    memset(info, 0, sizeof(*info));
    /* Nameless union by default in Wine's headers (NONAMELESSUNION unset). */
    info->wProcessorArchitecture = PROCESSOR_ARCHITECTURE_INTEL;
    info->dwPageSize = (DWORD)page;
    info->lpMinimumApplicationAddress = (LPVOID)0x10000;
    info->lpMaximumApplicationAddress = (LPVOID)0x7ffeffff;
    info->dwActiveProcessorMask = (cpus >= 32) ? 0xffffffffU : ((1u << cpus) - 1u);
    info->dwNumberOfProcessors = (DWORD)(cpus > 0 ? cpus : 1);
    info->dwProcessorType = PROCESSOR_INTEL_PENTIUM;
    info->dwAllocationGranularity = 0x10000;
    info->wProcessorLevel = 6;
}

BOOL WINAPI GetVersionExA(LPOSVERSIONINFOA info)
{
    if (!info || info->dwOSVersionInfoSize < sizeof(OSVERSIONINFOA)) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    /* Report Windows XP SP2: the era this build was shipped for, and what its
     * own version checks are written against. */
    info->dwMajorVersion = 5;
    info->dwMinorVersion = 1;
    info->dwBuildNumber = 2600;
    info->dwPlatformId = VER_PLATFORM_WIN32_NT;
    snprintf(info->szCSDVersion, sizeof(info->szCSDVersion), "Service Pack 2");
    return TRUE;
}

BOOL WINAPI IsProcessorFeaturePresent(DWORD feature)
{
    switch (feature) {
    case PF_MMX_INSTRUCTIONS_AVAILABLE:
    case PF_XMMI_INSTRUCTIONS_AVAILABLE:  /* SSE  */
    case PF_XMMI64_INSTRUCTIONS_AVAILABLE: /* SSE2 */
    case PF_COMPARE_EXCHANGE_DOUBLE:
    case PF_RDTSC_INSTRUCTION_AVAILABLE:
        return TRUE;
    default:
        return FALSE;
    }
}

BOOL WINAPI GetProcessAffinityMask(HANDLE proc, PDWORD_PTR process_mask, PDWORD_PTR system_mask)
{
    long cpus = sysconf(_SC_NPROCESSORS_ONLN);
    DWORD_PTR mask = (cpus >= 32) ? (DWORD_PTR)-1 : (DWORD_PTR)((1u << cpus) - 1u);

    (void)proc;
    if (process_mask)
        *process_mask = mask;
    if (system_mask)
        *system_mask = mask;
    return TRUE;
}

BOOL WINAPI SetProcessAffinityMask(HANDLE proc, DWORD_PTR mask)
{
    /* The game pins itself to one core to work around 2004-era timing bugs on
     * multi-socket machines. Honouring that on modern hardware costs
     * performance for no benefit, so ignore it. */
    (void)proc; (void)mask;
    return TRUE;
}

BOOL WINAPI SetPriorityClass(HANDLE proc, DWORD priority)
{
    (void)proc; (void)priority;
    return TRUE;
}

DWORD WINAPI GetPriorityClass(HANDLE proc)
{
    (void)proc;
    return NORMAL_PRIORITY_CLASS;
}
