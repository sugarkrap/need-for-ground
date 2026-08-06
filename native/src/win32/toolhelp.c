/*
 * toolhelp.c - process enumeration via /proc.
 *
 * Why a 2004 racing game enumerates processes: this trio is a classic
 * anti-debug / anti-cheat pattern - walk the process list looking for known
 * debugger or trainer executable names. It is implemented properly anyway
 * (reading /proc is easy and truthful) rather than stubbed, because a stub that
 * failed would leave the game unable to tell whether the check passed, and a
 * stub that returned an empty list would be a lie about the machine.
 *
 * There is nothing to hide here: a Linux process list contains no Windows
 * debugger names, so the check passes on its own merits.
 */
#include "shim_internal.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tlhelp32.h>

#define MAX_SNAPSHOT_ENTRIES 512

struct nfsu2_snapshot {
    struct nfsu2_object obj;
    int count;
    int cursor;
    struct {
        DWORD pid;
        DWORD parent_pid;
        char name[MAX_PATH];
    } entries[MAX_SNAPSHOT_ENTRIES];
};

/* Snapshots use the FIND object kind: same lifetime shape (created, iterated,
 * closed with CloseHandle) and no separate destructor needed. */
static void snapshot_fill(struct nfsu2_snapshot *snapshot)
{
    DIR *proc = opendir("/proc");
    struct dirent *entry;

    if (!proc)
        return;

    while ((entry = readdir(proc)) != NULL && snapshot->count < MAX_SNAPSHOT_ENTRIES) {
        char path[64];
        char line[256];
        FILE *status;
        long pid;
        char *end;

        pid = strtol(entry->d_name, &end, 10);
        if (*end != '\0' || pid <= 0)
            continue; /* not a process directory */

        snprintf(path, sizeof(path), "/proc/%ld/status", pid);
        status = fopen(path, "r");
        if (!status)
            continue; /* the process exited between readdir and now */

        snapshot->entries[snapshot->count].pid = (DWORD)pid;
        snapshot->entries[snapshot->count].parent_pid = 0;
        snapshot->entries[snapshot->count].name[0] = '\0';

        while (fgets(line, sizeof(line), status)) {
            if (strncmp(line, "Name:", 5) == 0) {
                char *name = line + 5;
                size_t len;

                while (*name == ' ' || *name == '\t')
                    name++;
                len = strcspn(name, "\n");
                if (len >= MAX_PATH)
                    len = MAX_PATH - 1;
                memcpy(snapshot->entries[snapshot->count].name, name, len);
                snapshot->entries[snapshot->count].name[len] = '\0';
            } else if (strncmp(line, "PPid:", 5) == 0) {
                snapshot->entries[snapshot->count].parent_pid = (DWORD)strtol(line + 5, NULL, 10);
            }
        }
        fclose(status);
        snapshot->count++;
    }
    closedir(proc);
}

HANDLE WINAPI CreateToolhelp32Snapshot(DWORD flags, DWORD pid)
{
    struct nfsu2_snapshot *snapshot;

    (void)pid;
    if (!(flags & TH32CS_SNAPPROCESS)) {
        /* Module and thread snapshots would need a different backing (and there
         * are no PE modules to report); processes are all the game asks for. */
        NFSU2_STUB("CreateToolhelp32Snapshot without TH32CS_SNAPPROCESS");
        SetLastError(ERROR_INVALID_PARAMETER);
        return INVALID_HANDLE_VALUE;
    }

    snapshot = nfsu2_obj_alloc(NFSU2_OBJ_FIND, sizeof(*snapshot));
    if (!snapshot)
        return INVALID_HANDLE_VALUE;

    snapshot_fill(snapshot);
    snapshot->cursor = 0;
    return (HANDLE)snapshot;
}

static BOOL fill_entry(struct nfsu2_snapshot *snapshot, LPPROCESSENTRY32 entry)
{
    if (snapshot->cursor >= snapshot->count) {
        SetLastError(ERROR_NO_MORE_FILES);
        return FALSE;
    }
    memset((char *)entry + sizeof(DWORD), 0, entry->dwSize - sizeof(DWORD));
    entry->th32ProcessID = snapshot->entries[snapshot->cursor].pid;
    entry->th32ParentProcessID = snapshot->entries[snapshot->cursor].parent_pid;
    entry->cntThreads = 1;
    entry->pcPriClassBase = 8; /* NORMAL_PRIORITY_CLASS */
    snprintf(entry->szExeFile, sizeof(entry->szExeFile), "%s",
             snapshot->entries[snapshot->cursor].name);
    snapshot->cursor++;
    return TRUE;
}

BOOL WINAPI Process32First(HANDLE handle, LPPROCESSENTRY32 entry)
{
    struct nfsu2_snapshot *snapshot = nfsu2_obj_get(handle, NFSU2_OBJ_FIND);

    if (!snapshot || !entry)
        return FALSE;
    if (entry->dwSize < sizeof(PROCESSENTRY32)) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    snapshot->cursor = 0;
    return fill_entry(snapshot, entry);
}

BOOL WINAPI Process32Next(HANDLE handle, LPPROCESSENTRY32 entry)
{
    struct nfsu2_snapshot *snapshot = nfsu2_obj_get(handle, NFSU2_OBJ_FIND);

    if (!snapshot || !entry)
        return FALSE;
    if (entry->dwSize < sizeof(PROCESSENTRY32)) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    return fill_entry(snapshot, entry);
}
