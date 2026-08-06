/*
 * registry.c - the registry entry points the game uses, backed by a text file.
 *
 * NFSU2 keeps its install path, video settings and profile location under
 * HKLM\SOFTWARE\EA GAMES\... and expects them to persist between runs. There is
 * no system registry here, so the store is a single file in the game root:
 *
 *     [HKEY_LOCAL_MACHINE\SOFTWARE\EA GAMES\Need for Speed Underground 2]
 *     InstallDir=sz:/home/user/games/nfsu2
 *     Language=dword:0000040c
 *     Blob=hex:00,1f,a0
 *
 * Chosen over a hidden dotfile because a user debugging their install should be
 * able to read and edit it, which is the whole reason this data is interesting.
 *
 * Deliberately not implemented: enumeration (RegEnumKeyEx/RegEnumValue),
 * deletion, and security/access masks. The game imports none of them, and each
 * would need a real key hierarchy rather than the flat path list below.
 */
#include "../win32/shim_internal.h"

#include <ctype.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define MAX_KEYS 64
#define MAX_VALUES_PER_KEY 32
#define MAX_VALUE_BYTES 1024

struct reg_value {
    char *name;
    DWORD type;
    unsigned char data[MAX_VALUE_BYTES];
    DWORD size;
};

struct reg_key {
    char *path; /* full path, e.g. "HKEY_LOCAL_MACHINE\\SOFTWARE\\EA GAMES" */
    struct reg_value values[MAX_VALUES_PER_KEY];
    int value_count;
    int open_count;
};

static struct reg_key g_keys[MAX_KEYS];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_loaded;
static int g_dirty;

/* --- predefined roots --------------------------------------------------- */

static const char *root_name(HKEY key)
{
    /*
     * The predefined handles are constants, not pointers - but compared against
     * the HKEY_* macros rather than against literals like 0x80000002, because
     * those macros sign-extend: HKEY_LOCAL_MACHINE is
     * (HKEY)(ULONG_PTR)(LONG)0x80000002, which is 0xffffffff80000002 in a
     * 64-bit pointer. A numeric case label silently stops matching.
     */
    if (key == HKEY_CLASSES_ROOT)
        return "HKEY_CLASSES_ROOT";
    if (key == HKEY_CURRENT_USER)
        return "HKEY_CURRENT_USER";
    if (key == HKEY_LOCAL_MACHINE)
        return "HKEY_LOCAL_MACHINE";
    if (key == HKEY_USERS)
        return "HKEY_USERS";
    if (key == HKEY_CURRENT_CONFIG)
        return "HKEY_CURRENT_CONFIG";
    if (key == HKEY_PERFORMANCE_DATA)
        return "HKEY_PERFORMANCE_DATA";
    return NULL;
}

static int key_index(HKEY key)
{
    struct reg_key *k = (struct reg_key *)key;

    if (root_name(key) || !k)
        return -1;
    if (k < g_keys || k >= g_keys + MAX_KEYS)
        return -1;
    return (int)(k - g_keys);
}

/* Build "ROOT\subkey" from an HKEY plus a relative path. */
static int build_path(HKEY key, LPCSTR subkey, char *out, size_t out_size)
{
    const char *base = root_name(key);
    int index;

    if (!base) {
        index = key_index(key);
        if (index < 0 || !g_keys[index].path)
            return -1;
        base = g_keys[index].path;
    }

    if (subkey && *subkey) {
        if ((size_t)snprintf(out, out_size, "%s\\%s", base, subkey) >= out_size)
            return -1;
    } else {
        if ((size_t)snprintf(out, out_size, "%s", base) >= out_size)
            return -1;
    }
    return 0;
}

/* --- the file store ---------------------------------------------------- */

static void store_path(char *out, size_t out_size)
{
    snprintf(out, out_size, "%s/registry.ini", nfsu2_path_root());
}

static struct reg_key *find_key(const char *path)
{
    int i;

    for (i = 0; i < MAX_KEYS; i++) {
        if (g_keys[i].path && strcasecmp(g_keys[i].path, path) == 0)
            return &g_keys[i];
    }
    return NULL;
}

static struct reg_key *create_key(const char *path)
{
    int i;

    for (i = 0; i < MAX_KEYS; i++) {
        if (!g_keys[i].path) {
            g_keys[i].path = strdup(path);
            g_keys[i].value_count = 0;
            return g_keys[i].path ? &g_keys[i] : NULL;
        }
    }
    nfsu2_shim_trace("registry: key table full (%d)", MAX_KEYS);
    return NULL;
}

static struct reg_value *find_value(struct reg_key *key, LPCSTR name)
{
    int i;
    const char *wanted = (name && *name) ? name : "";

    for (i = 0; i < key->value_count; i++) {
        if (strcasecmp(key->values[i].name, wanted) == 0)
            return &key->values[i];
    }
    return NULL;
}

static void parse_value(struct reg_key *key, char *line)
{
    struct reg_value *value;
    char *equals = strchr(line, '=');
    char *payload;

    if (!equals || key->value_count >= MAX_VALUES_PER_KEY)
        return;
    *equals = '\0';
    payload = equals + 1;

    value = &key->values[key->value_count];
    memset(value, 0, sizeof(*value));
    value->name = strdup(line);
    if (!value->name)
        return;

    if (strncmp(payload, "dword:", 6) == 0) {
        DWORD v = (DWORD)strtoul(payload + 6, NULL, 16);
        value->type = REG_DWORD;
        value->size = sizeof(v);
        memcpy(value->data, &v, sizeof(v));
    } else if (strncmp(payload, "hex:", 4) == 0) {
        char *cursor = payload + 4;
        value->type = REG_BINARY;
        while (*cursor && value->size < MAX_VALUE_BYTES) {
            value->data[value->size++] = (unsigned char)strtoul(cursor, &cursor, 16);
            if (*cursor == ',')
                cursor++;
            else
                break;
        }
    } else {
        const char *text = payload;
        if (strncmp(payload, "sz:", 3) == 0)
            text = payload + 3;
        value->type = REG_SZ;
        value->size = (DWORD)strlen(text) + 1;
        if (value->size > MAX_VALUE_BYTES)
            value->size = MAX_VALUE_BYTES;
        memcpy(value->data, text, value->size - 1);
        value->data[value->size - 1] = '\0';
    }
    key->value_count++;
}

static void load_store(void)
{
    char path[PATH_MAX_FALLBACK];
    char line[MAX_VALUE_BYTES + 256];
    struct reg_key *current = NULL;
    FILE *f;

    if (g_loaded)
        return;
    g_loaded = 1;

    store_path(path, sizeof(path));
    f = fopen(path, "r");
    if (!f) {
        nfsu2_shim_trace("registry: no store at %s (starting empty)", path);
        return;
    }

    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);

        while (len && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';
        if (!len || line[0] == '#' || line[0] == ';')
            continue;

        if (line[0] == '[') {
            char *close = strchr(line, ']');
            if (!close)
                continue;
            *close = '\0';
            current = find_key(line + 1);
            if (!current)
                current = create_key(line + 1);
        } else if (current) {
            parse_value(current, line);
        }
    }
    fclose(f);
    nfsu2_shim_trace("registry: loaded %s", path);
}

static void save_store(void)
{
    char path[PATH_MAX_FALLBACK];
    FILE *f;
    int i, j;

    if (!g_dirty)
        return;

    store_path(path, sizeof(path));
    f = fopen(path, "w");
    if (!f) {
        nfsu2_shim_trace("registry: cannot write %s", path);
        return;
    }

    fputs("# nfsu2-unwrap native port: registry store.\n"
          "# Values are sz: (string), dword: (hex) or hex: (comma-separated bytes).\n",
          f);

    for (i = 0; i < MAX_KEYS; i++) {
        if (!g_keys[i].path)
            continue;
        fprintf(f, "\n[%s]\n", g_keys[i].path);
        for (j = 0; j < g_keys[i].value_count; j++) {
            struct reg_value *v = &g_keys[i].values[j];

            switch (v->type) {
            case REG_DWORD: {
                DWORD dw = 0;
                memcpy(&dw, v->data, v->size < sizeof(dw) ? v->size : sizeof(dw));
                fprintf(f, "%s=dword:%08lx\n", v->name, (unsigned long)dw);
                break;
            }
            case REG_SZ:
            case REG_EXPAND_SZ:
                fprintf(f, "%s=sz:%.*s\n", v->name, (int)(v->size ? v->size - 1 : 0),
                        (const char *)v->data);
                break;
            default: {
                DWORD k;
                fprintf(f, "%s=hex:", v->name);
                for (k = 0; k < v->size; k++)
                    fprintf(f, "%s%02x", k ? "," : "", v->data[k]);
                fputc('\n', f);
                break;
            }
            }
        }
    }
    fclose(f);
    g_dirty = 0;
}

/* Called from nfsu2_win32_shutdown so settings survive a clean exit. */
void nfsu2_registry_flush(void)
{
    pthread_mutex_lock(&g_lock);
    save_store();
    pthread_mutex_unlock(&g_lock);
}

/* --- the Win32 entry points -------------------------------------------- */

static LSTATUS open_key(HKEY parent, LPCSTR subkey, PHKEY out, int create)
{
    char path[PATH_MAX_FALLBACK];
    struct reg_key *key;
    LSTATUS status = ERROR_SUCCESS;

    if (!out)
        return ERROR_INVALID_PARAMETER;

    pthread_mutex_lock(&g_lock);
    load_store();

    if (build_path(parent, subkey, path, sizeof(path)) != 0) {
        pthread_mutex_unlock(&g_lock);
        return ERROR_INVALID_HANDLE;
    }

    key = find_key(path);
    if (!key) {
        if (!create) {
            nfsu2_shim_trace("RegOpenKey: %s not found", path);
            pthread_mutex_unlock(&g_lock);
            return ERROR_FILE_NOT_FOUND;
        }
        key = create_key(path);
        if (!key) {
            pthread_mutex_unlock(&g_lock);
            return ERROR_NOT_ENOUGH_MEMORY;
        }
        g_dirty = 1;
    }
    key->open_count++;
    *out = (HKEY)key;
    pthread_mutex_unlock(&g_lock);
    return status;
}

LSTATUS WINAPI RegOpenKeyA(HKEY parent, LPCSTR subkey, PHKEY out)
{
    return open_key(parent, subkey, out, 0);
}

LSTATUS WINAPI RegOpenKeyExA(HKEY parent, LPCSTR subkey, DWORD options, REGSAM access, PHKEY out)
{
    (void)options; (void)access; /* no access control in this store */
    return open_key(parent, subkey, out, 0);
}

LSTATUS WINAPI RegCreateKeyA(HKEY parent, LPCSTR subkey, PHKEY out)
{
    return open_key(parent, subkey, out, 1);
}

LSTATUS WINAPI RegCreateKeyExA(HKEY parent, LPCSTR subkey, DWORD reserved, LPSTR class_name,
                               DWORD options, REGSAM access, LPSECURITY_ATTRIBUTES sa,
                               PHKEY out, LPDWORD disposition)
{
    LSTATUS status;

    (void)reserved; (void)class_name; (void)options; (void)access; (void)sa;

    pthread_mutex_lock(&g_lock);
    load_store();
    pthread_mutex_unlock(&g_lock);

    if (disposition) {
        char path[PATH_MAX_FALLBACK];
        pthread_mutex_lock(&g_lock);
        *disposition = (build_path(parent, subkey, path, sizeof(path)) == 0 && find_key(path))
                           ? REG_OPENED_EXISTING_KEY : REG_CREATED_NEW_KEY;
        pthread_mutex_unlock(&g_lock);
    }
    status = open_key(parent, subkey, out, 1);
    return status;
}

LSTATUS WINAPI RegCloseKey(HKEY key)
{
    int index;

    if (root_name(key))
        return ERROR_SUCCESS; /* predefined handles are never closed */

    pthread_mutex_lock(&g_lock);
    index = key_index(key);
    if (index < 0) {
        pthread_mutex_unlock(&g_lock);
        return ERROR_INVALID_HANDLE;
    }
    if (g_keys[index].open_count > 0)
        g_keys[index].open_count--;
    /* The key itself stays: it is a persistent store entry, not a handle. */
    save_store();
    pthread_mutex_unlock(&g_lock);
    return ERROR_SUCCESS;
}

LSTATUS WINAPI RegQueryValueExA(HKEY key, LPCSTR name, LPDWORD reserved, LPDWORD type,
                                LPBYTE data, LPDWORD data_size)
{
    struct reg_value *value;
    int index;
    LSTATUS status = ERROR_SUCCESS;

    (void)reserved;

    pthread_mutex_lock(&g_lock);
    load_store();
    index = key_index(key);
    if (index < 0 || !g_keys[index].path) {
        pthread_mutex_unlock(&g_lock);
        return ERROR_INVALID_HANDLE;
    }

    value = find_value(&g_keys[index], name);
    if (!value) {
        nfsu2_shim_trace("RegQueryValueExA: %s\\%s not set",
                         g_keys[index].path, name ? name : "(default)");
        pthread_mutex_unlock(&g_lock);
        return ERROR_FILE_NOT_FOUND;
    }

    if (type)
        *type = value->type;

    if (!data) {
        /* Size query. */
        if (data_size)
            *data_size = value->size;
    } else if (!data_size) {
        status = ERROR_INVALID_PARAMETER;
    } else if (*data_size < value->size) {
        *data_size = value->size;
        status = ERROR_MORE_DATA;
    } else {
        memcpy(data, value->data, value->size);
        *data_size = value->size;
    }

    pthread_mutex_unlock(&g_lock);
    return status;
}

LSTATUS WINAPI RegSetValueExA(HKEY key, LPCSTR name, DWORD reserved, DWORD type,
                              const BYTE *data, DWORD data_size)
{
    struct reg_key *k;
    struct reg_value *value;
    int index;

    (void)reserved;

    if (data_size > MAX_VALUE_BYTES) {
        nfsu2_shim_trace("RegSetValueExA: value too large (%lu bytes)", (unsigned long)data_size);
        return ERROR_INVALID_PARAMETER;
    }

    pthread_mutex_lock(&g_lock);
    load_store();
    index = key_index(key);
    if (index < 0 || !g_keys[index].path) {
        pthread_mutex_unlock(&g_lock);
        return ERROR_INVALID_HANDLE;
    }
    k = &g_keys[index];

    value = find_value(k, name);
    if (!value) {
        if (k->value_count >= MAX_VALUES_PER_KEY) {
            pthread_mutex_unlock(&g_lock);
            return ERROR_NOT_ENOUGH_MEMORY;
        }
        value = &k->values[k->value_count];
        memset(value, 0, sizeof(*value));
        value->name = strdup((name && *name) ? name : "");
        if (!value->name) {
            pthread_mutex_unlock(&g_lock);
            return ERROR_NOT_ENOUGH_MEMORY;
        }
        k->value_count++;
    }

    value->type = type;
    value->size = data_size;
    if (data && data_size)
        memcpy(value->data, data, data_size);
    g_dirty = 1;
    save_store();
    pthread_mutex_unlock(&g_lock);
    return ERROR_SUCCESS;
}
