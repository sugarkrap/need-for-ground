/*
 * teb.c - see teb.h. Lives in src/win32/ because the TEB is Win32 runtime
 * state - the SEH chain and LastError are both fields in it.
 *
 * Everything here is i386-only, and deliberately so: %fs is free on i386 Linux
 * (glibc uses %gs) which is what makes this a few dozen lines rather than a
 * project. On any other architecture the entry points fail cleanly.
 */
#include <nfsu2/teb.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>

#if defined(__i386__)

#include <asm/ldt.h>
#include <pthread.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

/* Windows' TEB is one page in practice; the fields we populate are all in the
 * first 0x40 bytes, and the static TLS slots live at 0xe10. */
#define TEB_SIZE      0x1000
#define PEB_SIZE      0x0400
#define TLS_SLOT_COUNT 64

/* modify_ldt() function numbers: 0 reads, 1 writes. */
#define LDT_WRITE 1

static __thread void *g_teb;
static __thread unsigned short g_selector;

/*
 * One LDT entry index for the whole process, reused by every thread. That is
 * safe because the *selector* is shared while the segment base is not: each
 * thread writes its own base into the same entry number before loading %fs...
 *
 * except that would race. So each thread gets its own entry instead, allocated by
 * letting the kernel pick: entry_number = -1 is not supported by modify_ldt, so we
 * scan for a free one under a mutex. Sixteen entries is far more than this port
 * will ever need, and running out is reported rather than silently shared.
 */
#define MAX_LDT_ENTRIES 16
static pthread_mutex_t g_ldt_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned char g_ldt_used[MAX_LDT_ENTRIES];

static int write_ldt_entry(int entry, void *base, unsigned int limit)
{
    struct user_desc desc;

    memset(&desc, 0, sizeof(desc));
    desc.entry_number = (unsigned int)entry;
    desc.base_addr = (unsigned long)base;
    desc.limit = limit;
    desc.seg_32bit = 1;
    desc.contents = 0;       /* data segment, expand-up */
    desc.read_exec_only = 0; /* writable: code writes fs:[0] constantly */
    desc.limit_in_pages = 0;
    desc.seg_not_present = 0;
    desc.useable = 1;

    if (syscall(SYS_modify_ldt, LDT_WRITE, &desc, sizeof(desc)) != 0)
        return -errno;
    return 0;
}

static int allocate_ldt_entry(void *base, unsigned int limit, unsigned short *selector)
{
    int entry;
    int rc = -EMFILE;

    pthread_mutex_lock(&g_ldt_lock);
    for (entry = 0; entry < MAX_LDT_ENTRIES; entry++) {
        if (g_ldt_used[entry])
            continue;
        rc = write_ldt_entry(entry, base, limit);
        if (rc == 0) {
            g_ldt_used[entry] = 1;
            /* Selector: index, table indicator 1 for the LDT, RPL 3 for user. */
            *selector = (unsigned short)((entry << 3) | 0x7);
        }
        break;
    }
    pthread_mutex_unlock(&g_ldt_lock);
    return rc;
}

int nfsu2_teb_install(char *error, size_t error_size)
{
    unsigned char *teb;
    unsigned char *peb;
    void **tls_slots;
    unsigned short selector = 0;
    pthread_attr_t attr;
    void *stack_addr = NULL;
    size_t stack_size = 0;
    int rc;

    if (g_teb)
        return 0; /* already installed on this thread */

    /*
     * One mapping for TEB, PEB and the TLS array. mmap rather than malloc so the
     * segment base is page-aligned and the allocation cannot move.
     */
    teb = mmap(NULL, TEB_SIZE + PEB_SIZE + TLS_SLOT_COUNT * sizeof(void *),
               PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (teb == MAP_FAILED) {
        if (error)
            snprintf(error, error_size, "cannot map a TEB: %s", strerror(errno));
        return -errno;
    }
    peb = teb + TEB_SIZE;
    tls_slots = (void **)(peb + PEB_SIZE);

    /* Real stack bounds, so code that sanity-checks them against its own frames
     * agrees with reality. */
    if (pthread_getattr_np(pthread_self(), &attr) == 0) {
        pthread_attr_getstack(&attr, &stack_addr, &stack_size);
        pthread_attr_destroy(&attr);
    }

    /* ExceptionList = -1 marks the end of the SEH chain, which is what Windows
     * puts there before any __try runs. Zero would look like a valid record. */
    *(unsigned int *)(teb + NFSU2_TEB_EXCEPTION_LIST) = 0xffffffffu;
    *(void **)(teb + NFSU2_TEB_STACK_BASE) =
        stack_addr ? (unsigned char *)stack_addr + stack_size : NULL;
    *(void **)(teb + NFSU2_TEB_STACK_LIMIT) = stack_addr;
    *(void **)(teb + NFSU2_TEB_SELF) = teb;
    *(unsigned int *)(teb + NFSU2_TEB_PROCESS_ID) = (unsigned int)getpid();
    *(unsigned int *)(teb + NFSU2_TEB_THREAD_ID) = (unsigned int)(unsigned long)pthread_self();
    *(void **)(teb + NFSU2_TEB_TLS_POINTER) = tls_slots;
    *(void **)(teb + NFSU2_TEB_PEB) = peb;
    *(unsigned int *)(teb + NFSU2_TEB_LAST_ERROR) = 0u;

    /* A PEB with just enough to satisfy the usual probes: not being debugged, and
     * an image base. Anything more would be invention. */
    peb[0x02] = 0; /* BeingDebugged */
    *(void **)(peb + 0x08) = (void *)0x400000; /* ImageBaseAddress */

    /*
     * A limit of 0xfff (one page, byte granularity) would cover the TEB but not
     * the TLS array beyond it, and code indexes fs: well past the TEB. Cover the
     * whole mapping.
     */
    rc = allocate_ldt_entry(teb,
        (unsigned int)(TEB_SIZE + PEB_SIZE + TLS_SLOT_COUNT * sizeof(void *) - 1),
        &selector);
    if (rc != 0) {
        munmap(teb, TEB_SIZE + PEB_SIZE + TLS_SLOT_COUNT * sizeof(void *));
        if (error) {
            snprintf(error, error_size, "modify_ldt failed: %s%s", strerror(-rc),
                     rc == -EMFILE ? " (no free LDT entry)" : "");
        }
        return rc;
    }

    __asm__ volatile("movw %w0, %%fs" :: "r"(selector));

    g_teb = teb;
    g_selector = selector;
    return 0;
}

void *nfsu2_teb_current(void)
{
    return g_teb;
}

unsigned short nfsu2_teb_selector(void)
{
    return g_selector;
}

#else /* !__i386__ */

int nfsu2_teb_install(char *error, size_t error_size)
{
    if (error) {
        snprintf(error, error_size,
                 "a %%fs-based TEB is i386-only: on x86_64 %%fs belongs to glibc's TLS");
    }
    return -ENOTSUP;
}

void *nfsu2_teb_current(void)
{
    return NULL;
}

unsigned short nfsu2_teb_selector(void)
{
    return 0;
}

#endif
