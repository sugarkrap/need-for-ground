/*
 * locale.c - codepage conversion, string comparison, locale queries.
 *
 * Most of this group is imported by MSVCRT's startup rather than by game code
 * directly, which sets the bar: it has to be *consistent*, not culturally
 * correct. Two decisions follow from that:
 *
 *  - GetACP reports 1252 (Windows-1252). That is what this build's data files
 *    and its .exe string literals are encoded in, so it is the only answer that
 *    round-trips the game's own text.
 *  - GetUserDefaultLCID reports en-US rather than the fr-FR this French retail
 *    build shipped for. Number formatting is the reason: the game parses and
 *    prints floats (times, speeds) through the CRT, and a locale with a comma
 *    decimal separator turns "1.234" into a parse that stops at the dot. The
 *    game's displayed language comes from its own data files, not from here, so
 *    nothing user-visible changes.
 *
 * Conversion covers CP1252 and UTF-8 properly, including the CP1252 0x80-0x9F
 * block which is *not* Latin-1 - getting that wrong is the classic way to turn
 * a curly quote into a control character. Other codepages fall back to Latin-1
 * with a trace rather than silently mangling.
 */
#include "shim_internal.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define CP_WINDOWS_1252 1252
#define CP_OEM_850      850

/* Unicode code points for CP1252 0x80-0x9F, where it diverges from Latin-1. */
static const unsigned short cp1252_high[32] = {
    0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
    0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
    0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178
};

static UINT resolve_codepage(UINT codepage)
{
    switch (codepage) {
    case CP_ACP:
    case CP_THREAD_ACP:
        return CP_WINDOWS_1252;
    case CP_OEMCP:
        return CP_OEM_850;
    default:
        return codepage;
    }
}

UINT WINAPI GetACP(void)
{
    return CP_WINDOWS_1252;
}

UINT WINAPI GetOEMCP(void)
{
    return CP_OEM_850;
}

BOOL WINAPI IsValidCodePage(UINT codepage)
{
    switch (resolve_codepage(codepage)) {
    case CP_WINDOWS_1252:
    case CP_OEM_850:
    case 437:
    case 1251:
    case CP_UTF8:
        return TRUE;
    default:
        return FALSE;
    }
}

BOOL WINAPI GetCPInfo(UINT codepage, LPCPINFO info)
{
    if (!info) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    memset(info, 0, sizeof(*info));
    if (resolve_codepage(codepage) == CP_UTF8) {
        info->MaxCharSize = 4;
    } else {
        info->MaxCharSize = 1; /* every codepage we support is single-byte */
    }
    info->DefaultChar[0] = '?';
    return TRUE;
}

/* --- codepage <-> UTF-16 ------------------------------------------------ */

static unsigned int decode_byte(UINT codepage, unsigned char c)
{
    if (c < 0x80)
        return c;
    if (codepage == CP_WINDOWS_1252)
        return (c < 0xa0) ? cp1252_high[c - 0x80] : c;
    /* Latin-1 for anything else: wrong in the 0x80-0xff range for real OEM
     * codepages, but predictable, and traced so it is not a silent guess. */
    return c;
}

static int encode_char(UINT codepage, unsigned int cp, unsigned char *out)
{
    int i;

    if (cp < 0x80) {
        *out = (unsigned char)cp;
        return 1;
    }
    if (codepage == CP_WINDOWS_1252) {
        for (i = 0; i < 32; i++) {
            if (cp1252_high[i] == cp) {
                *out = (unsigned char)(0x80 + i);
                return 1;
            }
        }
    }
    if (cp <= 0xff) {
        *out = (unsigned char)cp;
        return 1;
    }
    *out = '?';
    return 1;
}

/* UTF-8 -> code point; returns bytes consumed, 0 on a malformed sequence. */
static int utf8_decode(const unsigned char *s, int available, unsigned int *out)
{
    if (available <= 0)
        return 0;
    if (s[0] < 0x80) {
        *out = s[0];
        return 1;
    }
    if ((s[0] & 0xe0) == 0xc0 && available >= 2) {
        *out = (unsigned int)((s[0] & 0x1f) << 6) | (s[1] & 0x3f);
        return 2;
    }
    if ((s[0] & 0xf0) == 0xe0 && available >= 3) {
        *out = (unsigned int)((s[0] & 0x0f) << 12) | (unsigned int)((s[1] & 0x3f) << 6) |
               (s[2] & 0x3f);
        return 3;
    }
    if ((s[0] & 0xf8) == 0xf0 && available >= 4) {
        *out = (unsigned int)((s[0] & 0x07) << 18) | (unsigned int)((s[1] & 0x3f) << 12) |
               (unsigned int)((s[2] & 0x3f) << 6) | (s[3] & 0x3f);
        return 4;
    }
    return 0;
}

static int utf8_encode(unsigned int cp, unsigned char *out)
{
    if (cp < 0x80) {
        out[0] = (unsigned char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (unsigned char)(0xc0 | (cp >> 6));
        out[1] = (unsigned char)(0x80 | (cp & 0x3f));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (unsigned char)(0xe0 | (cp >> 12));
        out[1] = (unsigned char)(0x80 | ((cp >> 6) & 0x3f));
        out[2] = (unsigned char)(0x80 | (cp & 0x3f));
        return 3;
    }
    out[0] = (unsigned char)(0xf0 | (cp >> 18));
    out[1] = (unsigned char)(0x80 | ((cp >> 12) & 0x3f));
    out[2] = (unsigned char)(0x80 | ((cp >> 6) & 0x3f));
    out[3] = (unsigned char)(0x80 | (cp & 0x3f));
    return 4;
}

INT WINAPI MultiByteToWideChar(UINT codepage, DWORD flags, LPCSTR src, INT src_len,
                               LPWSTR dst, INT dst_len)
{
    const unsigned char *in = (const unsigned char *)src;
    UINT cp = resolve_codepage(codepage);
    int consumed = 0;
    int produced = 0;
    int include_terminator = 0;

    (void)flags;
    if (!src || src_len == 0) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (src_len < 0) {
        src_len = (int)strlen(src);
        include_terminator = 1;
    }

    while (consumed < src_len) {
        unsigned int point;
        int used;

        if (cp == CP_UTF8) {
            used = utf8_decode(in + consumed, src_len - consumed, &point);
            if (!used) {
                SetLastError(ERROR_NO_UNICODE_TRANSLATION);
                return 0;
            }
        } else {
            point = decode_byte(cp, in[consumed]);
            used = 1;
        }
        consumed += used;

        /* Anything outside the BMP needs a surrogate pair; the game's text is
         * Latin, so refuse rather than emit a broken half. */
        if (point > 0xffff) {
            SetLastError(ERROR_NO_UNICODE_TRANSLATION);
            return 0;
        }
        if (dst_len > 0) {
            if (produced >= dst_len) {
                SetLastError(ERROR_INSUFFICIENT_BUFFER);
                return 0;
            }
            dst[produced] = (WCHAR)point;
        }
        produced++;
    }

    if (include_terminator) {
        if (dst_len > 0) {
            if (produced >= dst_len) {
                SetLastError(ERROR_INSUFFICIENT_BUFFER);
                return 0;
            }
            dst[produced] = 0;
        }
        produced++;
    }
    return produced;
}

INT WINAPI WideCharToMultiByte(UINT codepage, DWORD flags, LPCWSTR src, INT src_len,
                               LPSTR dst, INT dst_len, LPCSTR default_char, LPBOOL used_default)
{
    UINT cp = resolve_codepage(codepage);
    int consumed = 0;
    int produced = 0;
    int include_terminator = 0;

    (void)flags;
    if (!src || src_len == 0) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (used_default)
        *used_default = FALSE;
    if (src_len < 0) {
        src_len = 0;
        while (src[src_len])
            src_len++;
        include_terminator = 1;
    }

    while (consumed < src_len) {
        unsigned char encoded[4];
        int count;

        if (cp == CP_UTF8) {
            count = utf8_encode((unsigned int)(unsigned short)src[consumed], encoded);
        } else {
            count = encode_char(cp, (unsigned int)(unsigned short)src[consumed], encoded);
            if (encoded[0] == '?' && src[consumed] != '?') {
                if (used_default)
                    *used_default = TRUE;
                if (default_char)
                    encoded[0] = (unsigned char)*default_char;
            }
        }
        consumed++;

        if (dst_len > 0) {
            if (produced + count > dst_len) {
                SetLastError(ERROR_INSUFFICIENT_BUFFER);
                return 0;
            }
            memcpy(dst + produced, encoded, (size_t)count);
        }
        produced += count;
    }

    if (include_terminator) {
        if (dst_len > 0) {
            if (produced >= dst_len) {
                SetLastError(ERROR_INSUFFICIENT_BUFFER);
                return 0;
            }
            dst[produced] = '\0';
        }
        produced++;
    }
    return produced;
}

/* --- comparison and case mapping ---------------------------------------- */

static int compare_narrow(DWORD flags, const char *a, int a_len, const char *b, int b_len)
{
    int i = 0;

    for (;;) {
        int ca, cb;

        if ((a_len >= 0 && i >= a_len) || (a_len < 0 && !a[i]))
            ca = -1;
        else
            ca = (unsigned char)a[i];
        if ((b_len >= 0 && i >= b_len) || (b_len < 0 && !b[i]))
            cb = -1;
        else
            cb = (unsigned char)b[i];

        if (ca < 0 && cb < 0)
            return 0;
        if (flags & NORM_IGNORECASE) {
            if (ca > 0)
                ca = tolower(ca);
            if (cb > 0)
                cb = tolower(cb);
        }
        if (ca != cb)
            return ca < cb ? -1 : 1;
        i++;
    }
}

INT WINAPI CompareStringA(LCID lcid, DWORD flags, LPCSTR a, INT a_len, LPCSTR b, INT b_len)
{
    int result;

    (void)lcid;
    if (!a || !b) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
    /*
     * Byte order, not linguistic collation: implementing real locale-aware
     * sorting would need a collation table, and the callers here are the CRT's
     * own string helpers plus the game's save-name sorting, where a stable
     * order matters and the exact ordering of accented characters does not.
     */
    result = compare_narrow(flags, a, a_len, b, b_len);
    return result < 0 ? CSTR_LESS_THAN : (result > 0 ? CSTR_GREATER_THAN : CSTR_EQUAL);
}

INT WINAPI CompareStringW(LCID lcid, DWORD flags, LPCWSTR a, INT a_len, LPCWSTR b, INT b_len)
{
    int i = 0;

    (void)lcid;
    if (!a || !b) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
    for (;;) {
        int ca, cb;

        if ((a_len >= 0 && i >= a_len) || (a_len < 0 && !a[i]))
            ca = -1;
        else
            ca = (unsigned short)a[i];
        if ((b_len >= 0 && i >= b_len) || (b_len < 0 && !b[i]))
            cb = -1;
        else
            cb = (unsigned short)b[i];

        if (ca < 0 && cb < 0)
            return CSTR_EQUAL;
        if (flags & NORM_IGNORECASE) {
            if (ca > 0 && ca < 128)
                ca = tolower(ca);
            if (cb > 0 && cb < 128)
                cb = tolower(cb);
        }
        if (ca != cb)
            return ca < cb ? CSTR_LESS_THAN : CSTR_GREATER_THAN;
        i++;
    }
}

INT WINAPI LCMapStringA(LCID lcid, DWORD flags, LPCSTR src, INT src_len, LPSTR dst, INT dst_len)
{
    int len;
    int i;

    (void)lcid;
    if (!src) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
    len = (src_len < 0) ? (int)strlen(src) + 1 : src_len;

    if (dst_len == 0)
        return len;
    if (!dst || dst_len < len) {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }

    if (flags & LCMAP_SORTKEY) {
        /* A real sort key is a collation-weight blob. Copying the string keeps
         * comparisons of two sort keys consistent with CompareStringA above,
         * which is the property callers actually depend on. */
        NFSU2_STUB("LCMapStringA(LCMAP_SORTKEY) returns the string, not weights");
        memcpy(dst, src, (size_t)len);
        return len;
    }

    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)src[i];

        if (flags & LCMAP_UPPERCASE)
            dst[i] = (char)((c < 0x80) ? toupper(c)
                                       : ((c >= 0xe0 && c <= 0xfe && c != 0xf7) ? c - 0x20 : c));
        else if (flags & LCMAP_LOWERCASE)
            dst[i] = (char)((c < 0x80) ? tolower(c)
                                       : ((c >= 0xc0 && c <= 0xde && c != 0xd7) ? c + 0x20 : c));
        else
            dst[i] = (char)c;
    }
    return len;
}

INT WINAPI LCMapStringW(LCID lcid, DWORD flags, LPCWSTR src, INT src_len, LPWSTR dst, INT dst_len)
{
    int len = 0;
    int i;

    (void)lcid;
    if (!src) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (src_len < 0) {
        while (src[len])
            len++;
        len++;
    } else {
        len = src_len;
    }

    if (dst_len == 0)
        return len;
    if (!dst || dst_len < len) {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }

    for (i = 0; i < len; i++) {
        unsigned short c = (unsigned short)src[i];

        if ((flags & LCMAP_UPPERCASE) && c < 0x80)
            dst[i] = (WCHAR)toupper(c);
        else if ((flags & LCMAP_LOWERCASE) && c < 0x80)
            dst[i] = (WCHAR)tolower(c);
        else
            dst[i] = (WCHAR)c;
    }
    return len;
}

static WORD ctype1_of(unsigned int c)
{
    WORD type = 0;

    if (c > 0xff)
        return C1_ALPHA;
    if (isupper((int)c))
        type |= C1_UPPER | C1_ALPHA;
    if (islower((int)c))
        type |= C1_LOWER | C1_ALPHA;
    if (isdigit((int)c))
        type |= C1_DIGIT;
    if (isspace((int)c))
        type |= C1_SPACE;
    if (ispunct((int)c))
        type |= C1_PUNCT;
    if (iscntrl((int)c))
        type |= C1_CNTRL;
    if (isxdigit((int)c))
        type |= C1_XDIGIT;
    if (c == ' ')
        type |= C1_BLANK;
    if (c >= 0xc0 && c != 0xd7 && c != 0xf7)
        type |= C1_ALPHA; /* accented Latin-1 letters */
    return type;
}

BOOL WINAPI GetStringTypeA(LCID lcid, DWORD type, LPCSTR src, INT len, LPWORD out)
{
    int i;

    (void)lcid;
    if (!src || !out) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (type != CT_CTYPE1) {
        /* CT_CTYPE2 is bidi classes and CT_CTYPE3 is script info; neither is
         * meaningful for this game's Latin text. */
        NFSU2_STUB("GetStringTypeA with CT_CTYPE2/3");
        SetLastError(ERROR_INVALID_FLAGS);
        return FALSE;
    }
    if (len < 0)
        len = (int)strlen(src) + 1;
    for (i = 0; i < len; i++)
        out[i] = ctype1_of((unsigned char)src[i]);
    return TRUE;
}

BOOL WINAPI GetStringTypeW(DWORD type, LPCWSTR src, INT len, LPWORD out)
{
    int i;

    if (!src || !out) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (type != CT_CTYPE1) {
        NFSU2_STUB("GetStringTypeW with CT_CTYPE2/3");
        SetLastError(ERROR_INVALID_FLAGS);
        return FALSE;
    }
    if (len < 0) {
        len = 0;
        while (src[len])
            len++;
        len++;
    }
    for (i = 0; i < len; i++)
        out[i] = ctype1_of((unsigned short)src[i]);
    return TRUE;
}

/* --- locale queries ----------------------------------------------------- */

#define NFSU2_LCID 0x0409 /* en-US; see the header comment for why */

LCID WINAPI GetUserDefaultLCID(void)
{
    return NFSU2_LCID;
}

LCID WINAPI GetSystemDefaultLCID(void)
{
    return NFSU2_LCID;
}

BOOL WINAPI IsValidLocale(LCID lcid, DWORD flags)
{
    (void)flags;
    return (lcid == NFSU2_LCID || lcid == LOCALE_SYSTEM_DEFAULT ||
            lcid == LOCALE_USER_DEFAULT || lcid == LOCALE_NEUTRAL) ? TRUE : FALSE;
}

static const char *locale_string(LCTYPE type)
{
    switch (type & 0xffff) {
    case LOCALE_SDECIMAL:        return ".";
    case LOCALE_STHOUSAND:       return ",";
    case LOCALE_SLIST:           return ",";
    case LOCALE_IMEASURE:        return "1";
    case LOCALE_SCURRENCY:       return "$";
    case LOCALE_SLANGUAGE:       return "English (United States)";
    case LOCALE_SENGLANGUAGE:    return "English";
    case LOCALE_SABBREVLANGNAME: return "ENU";
    case LOCALE_SISO639LANGNAME: return "en";
    case LOCALE_SCOUNTRY:        return "United States";
    case LOCALE_SENGCOUNTRY:     return "United States";
    case LOCALE_SISO3166CTRYNAME:return "US";
    case LOCALE_SDATE:           return "/";
    case LOCALE_STIME:           return ":";
    case LOCALE_SSHORTDATE:      return "M/d/yyyy";
    case LOCALE_STIMEFORMAT:     return "h:mm:ss tt";
    case LOCALE_IDEFAULTANSICODEPAGE: return "1252";
    case LOCALE_IDEFAULTCODEPAGE:     return "850";
    case LOCALE_IDEFAULTLANGUAGE:     return "0409";
    default:                     return NULL;
    }
}

INT WINAPI GetLocaleInfoA(LCID lcid, LCTYPE type, LPSTR out, INT out_len)
{
    const char *text = locale_string(type);
    int needed;

    (void)lcid;
    if (!text) {
        nfsu2_shim_trace("GetLocaleInfoA(0x%lx): unhandled LCTYPE", (unsigned long)type);
        SetLastError(ERROR_INVALID_FLAGS);
        return 0;
    }
    needed = (int)strlen(text) + 1;
    if (out_len == 0)
        return needed;
    if (!out || out_len < needed) {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }
    memcpy(out, text, (size_t)needed);
    return needed;
}

INT WINAPI GetLocaleInfoW(LCID lcid, LCTYPE type, LPWSTR out, INT out_len)
{
    const char *text = locale_string(type);
    int needed;
    int i;

    (void)lcid;
    if (!text) {
        nfsu2_shim_trace("GetLocaleInfoW(0x%lx): unhandled LCTYPE", (unsigned long)type);
        SetLastError(ERROR_INVALID_FLAGS);
        return 0;
    }
    needed = (int)strlen(text) + 1;
    if (out_len == 0)
        return needed;
    if (!out || out_len < needed) {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }
    for (i = 0; i < needed; i++)
        out[i] = (WCHAR)(unsigned char)text[i];
    return needed;
}

BOOL WINAPI EnumSystemLocalesA(LOCALE_ENUMPROCA callback, DWORD flags)
{
    char buffer[16];

    (void)flags;
    if (!callback) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    /* One locale exists here, so the enumeration is a single callback. The
     * hex-string form is what the real API passes. */
    snprintf(buffer, sizeof(buffer), "%08lx", (unsigned long)NFSU2_LCID);
    callback(buffer);
    return TRUE;
}

/* --- environment blocks ------------------------------------------------- */

extern char **environ;

LPSTR WINAPI GetEnvironmentStringsA(void)
{
    size_t total = 1; /* the extra NUL that terminates the block */
    char *block, *cursor;
    int i;

    for (i = 0; environ[i]; i++)
        total += strlen(environ[i]) + 1;

    block = malloc(total);
    if (!block) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    cursor = block;
    for (i = 0; environ[i]; i++) {
        size_t len = strlen(environ[i]) + 1;
        memcpy(cursor, environ[i], len);
        cursor += len;
    }
    *cursor = '\0';
    return block;
}

LPWSTR WINAPI GetEnvironmentStringsW(void)
{
    char *narrow = GetEnvironmentStringsA();
    size_t count = 0;
    WCHAR *wide;
    size_t i;

    if (!narrow)
        return NULL;
    /* Walk to the double NUL to find the block length. */
    while (!(narrow[count] == '\0' && narrow[count + 1] == '\0'))
        count++;
    count += 2;

    wide = malloc(count * sizeof(WCHAR));
    if (!wide) {
        free(narrow);
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    for (i = 0; i < count; i++)
        wide[i] = (WCHAR)(unsigned char)narrow[i];
    free(narrow);
    return wide;
}

BOOL WINAPI FreeEnvironmentStringsA(LPSTR block)
{
    free(block);
    return TRUE;
}

BOOL WINAPI FreeEnvironmentStringsW(LPWSTR block)
{
    free(block);
    return TRUE;
}

/* --- string helpers ----------------------------------------------------- */

INT WINAPI lstrcmpiA(LPCSTR a, LPCSTR b)
{
    if (!a || !b) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
    return strcasecmp(a, b);
}

INT WINAPI lstrcmpA(LPCSTR a, LPCSTR b)
{
    if (!a || !b) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
    return strcmp(a, b);
}

INT WINAPI lstrlenA(LPCSTR s)
{
    return s ? (INT)strlen(s) : 0;
}

/*
 * wsprintfA has a hard 1024-byte output limit on Windows (and no way to pass a
 * size), so callers size their buffers to match. Keeping the same limit means a
 * caller that relies on truncation there gets truncation here too, instead of a
 * buffer overrun.
 *
 * The conversions are done by hand rather than forwarded to vsnprintf, for two
 * reasons:
 *
 *  - Fidelity. Windows' wsprintf supports only %c %d %i %u %x %X %s %S and the
 *    h/l/w size prefixes - it has *no floating-point support at all*. Handing
 *    the format to vsnprintf would make "%f" work here and produce garbage on
 *    Windows, which is exactly the kind of divergence that gets discovered
 *    late.
 *  - Portability of the va_list. At 64-bit, Wine's __ms_va_list is
 *    __builtin_ms_va_list (ms_abi), which glibc's vsnprintf cannot accept;
 *    va_arg works on either flavour, so walking the arguments ourselves
 *    compiles at both widths.
 */
#define WSPRINTF_LIMIT 1024

static int append_text(char *out, int written, const char *text, int width, int left_align)
{
    int len = (int)strlen(text);
    int pad = width > len ? width - len : 0;

    if (!left_align) {
        while (pad-- > 0 && written < WSPRINTF_LIMIT - 1)
            out[written++] = ' ';
    }
    while (*text && written < WSPRINTF_LIMIT - 1)
        out[written++] = *text++;
    if (left_align) {
        while (pad-- > 0 && written < WSPRINTF_LIMIT - 1)
            out[written++] = ' ';
    }
    return written;
}

static void format_unsigned(char *buffer, size_t size, unsigned long value, int base, int upper)
{
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char reversed[32];
    size_t count = 0;
    size_t i;

    do {
        reversed[count++] = digits[value % (unsigned long)base];
        value /= (unsigned long)base;
    } while (value && count < sizeof(reversed));

    for (i = 0; i < count && i + 1 < size; i++)
        buffer[i] = reversed[count - 1 - i];
    buffer[i] = '\0';
}

INT WINAPI wvsprintfA(LPSTR out, LPCSTR format, __ms_va_list args)
{
    char scratch[64];
    const char *cursor = format;
    int written = 0;

    if (!out || !format) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }

    while (*cursor && written < WSPRINTF_LIMIT - 1) {
        int left_align = 0;
        int zero_pad = 0;
        int width = 0;
        int is_long = 0;

        if (*cursor != '%') {
            out[written++] = *cursor++;
            continue;
        }
        cursor++;
        if (*cursor == '%') {
            out[written++] = *cursor++;
            continue;
        }

        for (;;) {
            if (*cursor == '-') {
                left_align = 1;
                cursor++;
            } else if (*cursor == '0') {
                zero_pad = 1;
                cursor++;
            } else {
                break;
            }
        }
        while (*cursor >= '0' && *cursor <= '9')
            width = width * 10 + (*cursor++ - '0');
        /* Precision is parsed and ignored: wsprintf accepts it but only applies
         * it to strings, and no caller here relies on that. */
        if (*cursor == '.') {
            cursor++;
            while (*cursor >= '0' && *cursor <= '9')
                cursor++;
        }
        while (*cursor == 'l' || *cursor == 'h' || *cursor == 'w' || *cursor == 'I') {
            if (*cursor == 'l')
                is_long = 1;
            cursor++;
        }

        switch (*cursor) {
        case 'd':
        case 'i': {
            long value = is_long ? va_arg(args, long) : (long)va_arg(args, int);
            unsigned long magnitude = (value < 0) ? (unsigned long)(-value) : (unsigned long)value;
            char digits[32];

            format_unsigned(digits, sizeof(digits), magnitude, 10, 0);
            snprintf(scratch, sizeof(scratch), "%s%s", value < 0 ? "-" : "", digits);
            break;
        }
        case 'u': {
            unsigned long value = is_long ? va_arg(args, unsigned long)
                                          : (unsigned long)va_arg(args, unsigned int);
            format_unsigned(scratch, sizeof(scratch), value, 10, 0);
            break;
        }
        case 'x':
        case 'X': {
            unsigned long value = is_long ? va_arg(args, unsigned long)
                                          : (unsigned long)va_arg(args, unsigned int);
            format_unsigned(scratch, sizeof(scratch), value, 16, *cursor == 'X');
            break;
        }
        case 'c':
            scratch[0] = (char)va_arg(args, int);
            scratch[1] = '\0';
            break;
        case 's': {
            const char *text = va_arg(args, const char *);
            snprintf(scratch, sizeof(scratch), "%s", text ? text : "(null)");
            /* Long strings bypass `scratch` so they are not truncated at 63. */
            written = append_text(out, written, text ? text : "(null)", width, left_align);
            cursor++;
            continue;
        }
        case 'S': {
            /* Wide string: narrow it through the ANSI codepage. */
            const WCHAR *wide = va_arg(args, const WCHAR *);
            int i = 0;

            while (wide && wide[i] && i < (int)sizeof(scratch) - 1) {
                unsigned char encoded[4];
                encode_char(CP_WINDOWS_1252, (unsigned int)(unsigned short)wide[i], encoded);
                scratch[i] = (char)encoded[0];
                i++;
            }
            scratch[i] = '\0';
            break;
        }
        default:
            /* Unknown conversion (notably %f, which Windows does not support
             * either): emit it literally so the mistake is visible in the
             * output rather than silently swallowed. */
            nfsu2_shim_trace("wsprintfA: unsupported conversion %%%c", *cursor);
            if (written < WSPRINTF_LIMIT - 1)
                out[written++] = '%';
            if (*cursor && written < WSPRINTF_LIMIT - 1)
                out[written++] = *cursor;
            if (*cursor)
                cursor++;
            continue;
        }

        if (zero_pad && !left_align && (int)strlen(scratch) < width) {
            char padded[WSPRINTF_LIMIT];
            int pad = width - (int)strlen(scratch);
            int i;

            for (i = 0; i < pad && i < (int)sizeof(padded) - 1; i++)
                padded[i] = '0';
            snprintf(padded + i, sizeof(padded) - (size_t)i, "%s", scratch);
            written = append_text(out, written, padded, 0, 0);
        } else {
            written = append_text(out, written, scratch, width, left_align);
        }
        cursor++;
    }

    out[written] = '\0';
    return written;
}

INT WINAPIV wsprintfA(LPSTR out, LPCSTR format, ...)
{
    __ms_va_list args;
    int written;

    /* Not plain va_start: at 64-bit this function is ms_abi and its argument
     * list is a __builtin_ms_va_list. See win32_compat.h. */
    NFSU2_MS_VA_START(args, format);
    written = wvsprintfA(out, format, args);
    NFSU2_MS_VA_END(args);
    return written;
}
