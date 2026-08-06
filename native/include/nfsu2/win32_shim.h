/*
 * win32_shim.h - public control surface of the native Win32 shim.
 *
 * The shim implements the Win32 imports the game actually uses (see
 * analysis/win32_imports.txt for the full import list, and
 * `python3 native/tools/win32_coverage.py` for what is done so far) directly
 * on top of glibc/POSIX. It is not a Wine reimplementation and does not aim
 * to be one: only the ~250 entry points this one game imports matter.
 *
 * Everything here is host-side plumbing, not Win32 API - the Win32 entry
 * points themselves come from Wine's headers.
 */
#ifndef NFSU2_WIN32_SHIM_H
#define NFSU2_WIN32_SHIM_H

#include <nfsu2/win32_compat.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Initialise the shim. `game_root` is the directory the game's DATA/ tree
 * lives in; every Windows-style path the game opens is resolved relative to
 * it (case-insensitively, backslashes translated). Passing NULL uses
 * $NFSU2_ROOT, else the current directory.
 *
 * Returns 0 on success, -errno on failure.
 */
int nfsu2_win32_init(const char *game_root);

/* Release shim state (open handles, path cache). Optional; mainly for tests. */
void nfsu2_win32_shutdown(void);

/*
 * Publish the process arguments so GetCommandLineA() returns something. Call
 * from main() before any game code runs; without it GetCommandLineA returns an
 * empty string (which the game tolerates - it only parses debug switches).
 */
void nfsu2_win32_set_command_line(int argc, char **argv);

/*
 * Translate a Windows path ("DATA\\FRONTEND\\FE_ART.BUN", "C:\\foo\\bar",
 * "..\\save") to a host path under the game root, resolving each component
 * case-insensitively. Returns 0 on success and fills `out`; returns -ENOENT
 * if an intermediate component does not exist. The final component is
 * allowed not to exist (so it works for file creation), in which case its
 * name is used verbatim.
 */
int nfsu2_path_to_host(const char *win_path, char *out, size_t out_size);

/* Diagnostics: set NFSU2_SHIM_TRACE=1 to log unimplemented/stubbed calls. */
int nfsu2_shim_trace_enabled(void);
void nfsu2_shim_trace(const char *fmt, ...);

/*
 * Marks a call site we know is reachable but have not implemented. Logs on
 * every call when tracing is on. Grep for NFSU2_STUB to find the remaining
 * work.
 */
#define NFSU2_STUB(name) nfsu2_shim_trace("STUB %s", name)

#ifdef __cplusplus
}
#endif

#endif /* NFSU2_WIN32_SHIM_H */
