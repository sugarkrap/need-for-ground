/*
 * interlocked.c - out-of-line Interlocked* symbols.
 *
 * Wine's winnt.h defines these as FORCEINLINE static functions over the
 * compiler's atomic builtins (winnt.h:7695 onwards), which is ideal for a direct
 * call and useless for GetProcAddress: a static inline produces no symbol, so
 * dlsym finds nothing and the game's import of `InterlockedExchange` cannot be
 * satisfied.
 *
 * Renaming the header's inline versions out of the way with a macro, then
 * defining the real symbols, gives both: call sites still inline, and the names
 * exist in the dynamic symbol table.
 */
#include <nfsu2/win32_dllmacros.h>

/*
 * Must come before the Wine headers: this redirects their static inline
 * definitions to harmless names so the real symbols below are ours.
 */
#define InterlockedExchange        wine_inline_InterlockedExchange
#define InterlockedCompareExchange wine_inline_InterlockedCompareExchange
#define InterlockedIncrement       wine_inline_InterlockedIncrement
#define InterlockedDecrement       wine_inline_InterlockedDecrement
#define InterlockedExchangeAdd     wine_inline_InterlockedExchangeAdd

#include <nfsu2/win32_compat.h>

#undef InterlockedExchange
#undef InterlockedCompareExchange
#undef InterlockedIncrement
#undef InterlockedDecrement
#undef InterlockedExchangeAdd

LONG WINAPI InterlockedExchange(LONG volatile *destination, LONG value)
{
    return __atomic_exchange_n(destination, value, __ATOMIC_SEQ_CST);
}

LONG WINAPI InterlockedCompareExchange(LONG volatile *destination, LONG exchange, LONG comparand)
{
    LONG expected = comparand;

    /* Win32 returns the *previous* value either way, which is what
     * __atomic_compare_exchange_n leaves in `expected` on failure and what it
     * already held on success. */
    __atomic_compare_exchange_n(destination, &expected, exchange, 0,
                                __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return expected;
}

LONG WINAPI InterlockedIncrement(LONG volatile *destination)
{
    return __atomic_add_fetch(destination, 1, __ATOMIC_SEQ_CST);
}

LONG WINAPI InterlockedDecrement(LONG volatile *destination)
{
    return __atomic_sub_fetch(destination, 1, __ATOMIC_SEQ_CST);
}

LONG WINAPI InterlockedExchangeAdd(LONG volatile *destination, LONG value)
{
    return __atomic_fetch_add(destination, value, __ATOMIC_SEQ_CST);
}
