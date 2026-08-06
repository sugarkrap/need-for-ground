/*
 * path.c - Windows path -> host path translation.
 *
 * The game opens paths like "DATA\FRONTEND\FE_ART.BUN", occasionally with a
 * drive letter, and its case does not match what is on disk (2004 tooling
 * wrote the tree in one case and the code asks for another). On NTFS/Wine
 * that just works; on a case-sensitive filesystem it has to be resolved
 * component by component.
 *
 * Strategy: split on / and \, then for each component try the literal name
 * first (the common case, one stat) and only fall back to a case-insensitive
 * directory scan when that misses. Successful resolutions are cached, keyed
 * by the incoming Windows path, because the game re-opens the same files
 * constantly (streaming) and a scandir per open in a hot loop is not free.
 */
#include "shim_internal.h"

#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

static char g_root[PATH_MAX_FALLBACK];

/* Small open-addressed cache of translated paths. */
#define CACHE_SLOTS 512
struct cache_entry {
    char *win;
    char *host;
};
static struct cache_entry g_cache[CACHE_SLOTS];

static unsigned long hash_ci(const char *s)
{
    unsigned long h = 5381;
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '\\')
            c = '/';
        else if (c >= 'A' && c <= 'Z')
            c = (unsigned char)(c - 'A' + 'a');
        h = ((h << 5) + h) ^ c;
    }
    return h;
}

static const char *cache_lookup(const char *win)
{
    struct cache_entry *e = &g_cache[hash_ci(win) % CACHE_SLOTS];
    if (e->win && strcmp(e->win, win) == 0)
        return e->host;
    return NULL;
}

static void cache_store(const char *win, const char *host)
{
    struct cache_entry *e = &g_cache[hash_ci(win) % CACHE_SLOTS];
    char *w = strdup(win);
    char *h = strdup(host);

    if (!w || !h) {
        free(w);
        free(h);
        return;
    }
    free(e->win);
    free(e->host);
    e->win = w;
    e->host = h;
}

void nfsu2_path_reset(void)
{
    size_t i;
    for (i = 0; i < CACHE_SLOTS; i++) {
        free(g_cache[i].win);
        free(g_cache[i].host);
        g_cache[i].win = NULL;
        g_cache[i].host = NULL;
    }
    g_root[0] = '\0';
}

const char *nfsu2_path_root(void)
{
    return g_root[0] ? g_root : ".";
}

int nfsu2_path_set_root(const char *root, char *out, size_t out_size)
{
    char buf[PATH_MAX_FALLBACK];

    if (!root || !*root)
        root = getenv("NFSU2_ROOT");
    if (!root || !*root)
        root = ".";

    if (!realpath(root, buf))
        return -errno;

    if (strlen(buf) + 1 > sizeof(g_root))
        return -ENAMETOOLONG;

    nfsu2_path_reset();
    memcpy(g_root, buf, strlen(buf) + 1);

    if (out && out_size) {
        snprintf(out, out_size, "%s", g_root);
    }
    return 0;
}

/* Case-insensitive lookup of `name` inside directory `dir`. */
static int find_ci(const char *dir, const char *name, char *out, size_t out_size)
{
    DIR *d = opendir(dir[0] ? dir : ".");
    struct dirent *ent;
    int found = 0;

    if (!d)
        return -errno;

    while ((ent = readdir(d)) != NULL) {
        if (strcasecmp(ent->d_name, name) == 0) {
            snprintf(out, out_size, "%s", ent->d_name);
            found = 1;
            break;
        }
    }
    closedir(d);
    return found ? 0 : -ENOENT;
}

int nfsu2_path_to_host(const char *win_path, char *out, size_t out_size)
{
    char work[PATH_MAX_FALLBACK];
    char acc[PATH_MAX_FALLBACK];
    const char *cached;
    size_t acc_len;
    char *cursor;
    int missing_tail = 0;

    if (!win_path || !out || out_size == 0)
        return -EINVAL;

    /* Already a host path? Pass absolute POSIX paths through untouched: that
     * is what GetModuleFileNameA hands back, and the game round-trips it. */
    if (win_path[0] == '/' && !strchr(win_path, '\\')) {
        if (snprintf(out, out_size, "%s", win_path) >= (int)out_size)
            return -ENAMETOOLONG;
        return 0;
    }

    cached = cache_lookup(win_path);
    if (cached) {
        if (snprintf(out, out_size, "%s", cached) >= (int)out_size)
            return -ENAMETOOLONG;
        return 0;
    }

    if (snprintf(work, sizeof(work), "%s", win_path) >= (int)sizeof(work))
        return -ENAMETOOLONG;

    cursor = work;

    /* Drive letter and/or leading separator: both mean "game root". There is
     * no meaningful C:\ on this platform and the game only ever uses its own
     * install drive. */
    if (((cursor[0] >= 'A' && cursor[0] <= 'Z') || (cursor[0] >= 'a' && cursor[0] <= 'z')) &&
        cursor[1] == ':')
        cursor += 2;
    while (*cursor == '\\' || *cursor == '/')
        cursor++;

    acc_len = (size_t)snprintf(acc, sizeof(acc), "%s", nfsu2_path_root());
    if (acc_len >= sizeof(acc))
        return -ENAMETOOLONG;

    while (*cursor) {
        char comp[NAME_MAX + 1];
        char resolved[NAME_MAX + 1];
        char probe[PATH_MAX_FALLBACK];
        size_t len = 0;
        struct stat st;

        while (*cursor && *cursor != '\\' && *cursor != '/') {
            if (len + 1 >= sizeof(comp))
                return -ENAMETOOLONG;
            comp[len++] = *cursor++;
        }
        comp[len] = '\0';
        while (*cursor == '\\' || *cursor == '/')
            cursor++;

        if (len == 0 || strcmp(comp, ".") == 0)
            continue;

        if (strcmp(comp, "..") == 0) {
            char *slash = strrchr(acc, '/');
            if (slash && slash != acc)
                *slash = '\0';
            acc_len = strlen(acc);
            continue;
        }

        if (snprintf(probe, sizeof(probe), "%s/%s", acc, comp) >= (int)sizeof(probe))
            return -ENAMETOOLONG;

        if (lstat(probe, &st) == 0) {
            /* Exact match on disk - the fast path. */
            snprintf(resolved, sizeof(resolved), "%s", comp);
        } else if (find_ci(acc, comp, resolved, sizeof(resolved)) != 0) {
            /*
             * Not found. If this was the last component, that is fine: the
             * caller may be creating it. Anything earlier is a hard miss.
             */
            if (*cursor != '\0')
                return -ENOENT;
            snprintf(resolved, sizeof(resolved), "%s", comp);
            missing_tail = 1;
        }

        if (acc_len + 1 + strlen(resolved) + 1 > sizeof(acc))
            return -ENAMETOOLONG;
        acc[acc_len++] = '/';
        memcpy(acc + acc_len, resolved, strlen(resolved) + 1);
        acc_len += strlen(resolved);
    }

    if (snprintf(out, out_size, "%s", acc) >= (int)out_size)
        return -ENAMETOOLONG;

    /* Only cache fully-resolved paths; a missing tail may appear later. */
    if (!missing_tail)
        cache_store(win_path, acc);

    return 0;
}
