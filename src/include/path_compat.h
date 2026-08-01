#ifndef PATH_COMPAT_H
#define PATH_COMPAT_H

static inline CHAR16* visor_path_without_boot_mount(CHAR16 *path) {
    static const CHAR16 prefix[] = { '\\', 'b', 'o', 'o', 't', '\\', 0 };

    if (!path) return NULL;
    for (int i = 0; prefix[i]; i++) {
        CHAR16 c = path[i];
        if (c >= 'A' && c <= 'Z') c = (CHAR16)(c + 32);
        if (c != prefix[i]) return NULL;
    }
    if (path[6] == '\0') return NULL;

    return path + 5;
}

#endif
