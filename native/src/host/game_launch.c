/*
 * game_launch.c - call the game's entry point and see how far it gets.
 *
 * Everything else in native/ tests a piece. This runs the *program*: map
 * speed2.exe at its own base, resolve its imports onto our shim, give the thread a
 * TEB and the process a fault handler, then jump to AddressOfEntryPoint and let it
 * go. There is no more scaffolding to add before trying this - and trying it is the
 * only way to find out what the next thing to fix is, in the order the game
 * actually needs it.
 *
 * The entry point is MSVC's CRT startup, which matters for one specific reason:
 * `_heap_init` and `_mtinit` run there. Those fill `_crtheap` and the CRT lock
 * table, which is what the piecemeal experiments could not do for themselves (see
 * the SEH section in ../../README.md) - so calling code *through* the entry point
 * can get places that calling it directly cannot.
 *
 * Failure is the expected outcome for a while, so the point is the quality of the
 * report:
 *
 *   - it runs in a forked child, because the game will call ExitProcess and take
 *     the process with it either way; the parent survives to say what happened
 *   - our SIGSEGV handler turns a fault into an exception with a code and an
 *     address, and prints where it was rather than dying silently
 *   - NFSU2_SHIM_TRACE=1 logs every Win32 call, so "how far did it get" has an
 *     answer in terms of what the game asked for last
 *
 * Nothing here embeds game data: the exe and its data root are supplied at runtime.
 */
#include <nfsu2/win32_compat.h>

#include <nfsu2/d3d9_native.h>
#include <nfsu2/pe_loader.h>
#include <nfsu2/seh.h>
#include <nfsu2/teb.h>
#include <nfsu2/win32_shim.h>

#include <libgen.h>
#include <stdint.h>
#include <sys/prctl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static void report_unresolved(const char *library, const char *symbol)
{
    printf("             unresolved: %s!%s\n", library, symbol);
}

/* The image is mapped for the child's lifetime; the entry point never returns
 * normally (it ends in ExitProcess), so there is nothing to unwind here. */
static void run_entry_point(unsigned int entry)
{
    void (*start)(void) = (void (*)(void))(uintptr_t)entry;

    printf("\n--- entering the game at 0x%08x ---\n\n", entry);
    fflush(stdout);
    start();
    printf("\n--- the entry point RETURNED, which a CRT startup does not do ---\n");
    fflush(stdout);
}

int main(int argc, char **argv)
{
    const char *exe = getenv("NFSU2_EXE");
    const char *root = getenv("NFSU2_ROOT");
    int use_fork = 1;
    struct nfsu2_pe_image image;
    struct nfsu2_pe_import_stats stats;
    char error[256] = "";
    char *exe_copy;
    pid_t child;
    int status = 0;
    int i;

    setvbuf(stdout, NULL, _IONBF, 0);

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--exe") && i + 1 < argc) exe = argv[++i];
        else if (!strcmp(argv[i], "--root") && i + 1 < argc) root = argv[++i];
        /* Vulkan and fork() are not friends: a driver that has already touched
         * the parent may not survive being inherited. --no-fork runs the game in
         * this process instead, at the cost of the parent's exit report. */
        else if (!strcmp(argv[i], "--no-fork")) use_fork = 0;
        else {
            fprintf(stderr, "usage: %s [--exe PATH] [--root DIR]\n"
                            "  --no-fork runs in this process (Vulkan dislikes fork)\n"
                            "  NFSU2_SHIM_TRACE=1 logs every Win32 call\n", argv[0]);
            return 2;
        }
    }
    if (!exe || !*exe) {
        fprintf(stderr, "no exe: pass --exe PATH or set NFSU2_EXE\n");
        return 2;
    }

    /*
     * The game reads its data with relative paths, so the root has to be the
     * directory the exe lives in unless told otherwise - that is where a real
     * process would have been started from.
     */
    exe_copy = strdup(exe);
    if (!root || !*root)
        root = exe_copy ? dirname(exe_copy) : ".";
    if (chdir(root) != 0)
        fprintf(stderr, "warning: cannot chdir to %s\n", root);
    setenv("NFSU2_ROOT", root, 1);
    printf("root       : %s\n", root);

    /*
     * The shim's own root, which is not the same thing as the working directory:
     * every path the game opens is resolved against it. This host used to rely on
     * the chdir alone, which worked by accident - path.c falls back to "." - but
     * left nfsu2_path_root() reporting "." to everything that asks, including
     * GetCurrentDirectoryA, whose answer the game uses as its file-system search
     * path. Both smoke hosts have always called this.
     */
    if (nfsu2_win32_init(root) != 0)
        fprintf(stderr, "warning: cannot set the shim root to %s\n", root);

    if (nfsu2_pe_load(exe, &image, error, sizeof(error)) != 0) {
        fprintf(stderr, "%s: %s\n", exe, error);
        return 1;
    }
    printf("mapped     : base 0x%x, %u KiB, entry 0x%x\n",
           image.image_base, image.image_size / 1024, image.entry_point);

    /*
     * Take the address of Direct3DCreate9 before resolving imports, and this is
     * load-bearing rather than diagnostic: the IAT is filled through
     * dlsym(RTLD_DEFAULT, ...), which only finds what is actually loaded, and the
     * linker drops a library nothing references (--as-needed). Without this the
     * game called an unresolved slot - the raw hint-table RVA, 0x3e616c - and
     * faulted there, having got all the way through its disc check first.
     */
    printf("d3d9       : Direct3DCreate9 at %p\n", (void *)(uintptr_t)Direct3DCreate9);

    nfsu2_pe_set_import_reporter(report_unresolved);
    nfsu2_pe_resolve_imports(&image, &stats);
    nfsu2_pe_set_import_reporter(NULL);
    printf("imports    : %d of %d resolved (%d by ordinal)\n",
           stats.resolved, stats.total, stats.by_ordinal);

    fflush(stdout);
    if (!use_fork) {
        if (nfsu2_teb_install(error, sizeof(error)) != 0) {
            fprintf(stderr, "TEB: %s\n", error);
            return 1;
        }
        if (nfsu2_seh_install(error, sizeof(error)) != 0)
            fprintf(stderr, "SEH unavailable: %s\n", error);
        run_entry_point(image.entry_point);
        return 0;
    }
    child = fork();
    if (child < 0) {
        perror("fork");
        return 1;
    }
    if (child == 0) {
        /*
         * Die with the parent, and this is not belt-and-braces: without it, a
         * parent killed by a timeout (or Ctrl-C, or a supervisor) leaves the game
         * running with the log pipe still open. A game stuck in a modal retry loop
         * then writes to that log forever - which is exactly what happened here,
         * to the tune of tens of gigabytes across four abandoned children.
         */
        prctl(PR_SET_PDEATHSIG, SIGKILL);
        /* The parent may already be gone: check, rather than race. */
        if (getppid() == 1)
            _exit(4);

        if (nfsu2_teb_install(error, sizeof(error)) != 0) {
            fprintf(stderr, "TEB: %s\n", error);
            _exit(3);
        }
        if (nfsu2_seh_install(error, sizeof(error)) != 0)
            fprintf(stderr, "SEH unavailable: %s\n", error);
        run_entry_point(image.entry_point);
        _exit(0);
    }

    if (waitpid(child, &status, 0) < 0) {
        perror("waitpid");
        return 1;
    }
    printf("\n=== the game stopped ===\n");
    if (WIFSIGNALED(status)) {
        printf("killed by signal %d (%s)\n", WTERMSIG(status), strsignal(WTERMSIG(status)));
        printf("A fault that our handler did not translate, or one nothing handled -\n"
               "the exception report above it says which.\n");
    } else if (WIFEXITED(status)) {
        printf("exited with status %d\n", WEXITSTATUS(status));
        if (WEXITSTATUS(status) == 3)
            printf("(the TEB could not be installed - this build must be 32-bit)\n");
    }
    printf("\nRe-run with NFSU2_SHIM_TRACE=1 to see the last Win32 calls it made.\n");

    nfsu2_pe_unload(&image);
    free(exe_copy);
    return 0;
}
