/*
 * comm.c - the serial-port entry points, as honest failures.
 *
 * These exist because NFSU2 shipped with 2004-era direct-connect multiplayer:
 * a null-modem cable or a dial-up modem between two PCs (the TAPI half lives in
 * tapi32/tapi.c). Nothing in a modern port will drive a serial link, and the
 * replacement multiplayer will be built on sockets - which *are* implemented,
 * in ws2_32/.
 *
 * So these are not implemented, but they are also not absent: the imports have
 * to resolve for the binary to load, and each one has to fail the way a machine
 * with no serial hardware fails, so the game takes its "no modem available"
 * path instead of proceeding on garbage. Every one sets a plausible last error
 * and traces, so if some code path unexpectedly depends on serial I/O it shows
 * up in an NFSU2_SHIM_TRACE=1 log rather than as a mystery.
 *
 * Implementing them for real is not hard if it is ever wanted - a COM port is a
 * tty on Linux and DCB maps onto termios - but it would be dead code today.
 */
#include "shim_internal.h"

#include <string.h>

BOOL WINAPI GetCommState(HANDLE handle, LPDCB dcb)
{
    (void)handle;
    NFSU2_STUB("GetCommState (no serial support)");
    if (dcb)
        memset(dcb, 0, sizeof(*dcb));
    SetLastError(ERROR_INVALID_HANDLE);
    return FALSE;
}

BOOL WINAPI SetCommState(HANDLE handle, LPDCB dcb)
{
    (void)handle; (void)dcb;
    NFSU2_STUB("SetCommState (no serial support)");
    SetLastError(ERROR_INVALID_HANDLE);
    return FALSE;
}

BOOL WINAPI GetCommConfig(HANDLE handle, LPCOMMCONFIG config, LPDWORD size)
{
    (void)handle;
    NFSU2_STUB("GetCommConfig (no serial support)");
    if (size)
        *size = config ? sizeof(*config) : 0;
    SetLastError(ERROR_INVALID_HANDLE);
    return FALSE;
}

BOOL WINAPI SetCommConfig(HANDLE handle, LPCOMMCONFIG config, DWORD size)
{
    (void)handle; (void)config; (void)size;
    NFSU2_STUB("SetCommConfig (no serial support)");
    SetLastError(ERROR_INVALID_HANDLE);
    return FALSE;
}

BOOL WINAPI SetCommTimeouts(HANDLE handle, LPCOMMTIMEOUTS timeouts)
{
    (void)handle; (void)timeouts;
    NFSU2_STUB("SetCommTimeouts (no serial support)");
    SetLastError(ERROR_INVALID_HANDLE);
    return FALSE;
}

BOOL WINAPI SetCommMask(HANDLE handle, DWORD mask)
{
    (void)handle; (void)mask;
    NFSU2_STUB("SetCommMask (no serial support)");
    SetLastError(ERROR_INVALID_HANDLE);
    return FALSE;
}

BOOL WINAPI SetupComm(HANDLE handle, DWORD in_queue, DWORD out_queue)
{
    (void)handle; (void)in_queue; (void)out_queue;
    NFSU2_STUB("SetupComm (no serial support)");
    SetLastError(ERROR_INVALID_HANDLE);
    return FALSE;
}

BOOL WINAPI PurgeComm(HANDLE handle, DWORD flags)
{
    (void)handle; (void)flags;
    NFSU2_STUB("PurgeComm (no serial support)");
    SetLastError(ERROR_INVALID_HANDLE);
    return FALSE;
}

BOOL WINAPI WaitCommEvent(HANDLE handle, LPDWORD mask, LPOVERLAPPED overlapped)
{
    (void)handle; (void)overlapped;
    NFSU2_STUB("WaitCommEvent (no serial support)");
    if (mask)
        *mask = 0;
    SetLastError(ERROR_INVALID_HANDLE);
    return FALSE;
}

/*
 * GetOverlappedResult belongs to kernel32 rather than the comm API, but the
 * only thing in this game that uses overlapped I/O is the serial path (see
 * win32/file.c, which rejects a non-NULL OVERLAPPED outright). Failing here
 * keeps that consistent instead of pretending an operation completed.
 */
BOOL WINAPI GetOverlappedResult(HANDLE handle, LPOVERLAPPED overlapped,
                                LPDWORD transferred, BOOL wait)
{
    (void)handle; (void)overlapped; (void)wait;
    NFSU2_STUB("GetOverlappedResult (no overlapped I/O)");
    if (transferred)
        *transferred = 0;
    SetLastError(ERROR_INVALID_HANDLE);
    return FALSE;
}
