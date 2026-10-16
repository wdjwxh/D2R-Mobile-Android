// Upstream dm/host/hle_fs.c overlay lookup and directory snapshots (944e199).
// Android uses exact Linux names instead of the Darwin xattr name adapter.
static int root_path(const char *root, const char *rest, char *out, size_t size) {
    if (snprintf(out, size, "%s", root) >= (int)size) { errno = ENAMETOOLONG; return 0; }
    size_t used = strlen(out);
    while (*rest) {
        while (*rest == '/') ++rest;
        if (!*rest) break;
        const char *end = strchr(rest, '/');
        if (!end) end = rest + strlen(rest);
        size_t length = (size_t)(end - rest);
        if (length >= 0x301) { errno = ENAMETOOLONG; return 0; }
        if (length == 2 && !strncmp(rest, "..", 2)) { errno = ENOENT; return 0; }
        if (length != 1 || *rest != '.') {
            if (used + length + 2 > size) { errno = ENAMETOOLONG; return 0; }
            out[used++] = '/';
            memcpy(out + used, rest, length);
            used += length; out[used] = 0;
            if (access(out, F_OK)) return 0;
        }
        rest = end;
    }
    return 1;
}

int dm_overlay_file(const char *rel, char *out, size_t size) {
    const char *overlay = getenv("DM_ROMFS_OVERLAY");
    for (const char *p = overlay ? overlay : ""; *p;) {
        size_t length = strcspn(p, ":");
        if (length && snprintf(out, size, "%.*s/%s", (int)length, p, rel) < (int)size && !access(out, R_OK)) return 1;
        p += length; if (*p == ':') ++p;
    }
    if (size) out[0] = 0;
    return 0;
}

void dm_android_touch_layout_boot(void) {
    // dm_padmode.c: k_pfx + pfx_write + touch branch of dm_padmode_boot.
    // This runs before nnMain creates any panels. Runtime controller/touch
    // switching still needs upstream's rebuild transaction and is separate.
    static const struct { uint32_t offset; const char *pad, *touch; } prefixes[] = {
        {0x1d21db5, "Controller/Console/Switch/", "0touch_console_switch_lay/"},
        {0x1d2c33f, "Controller/Console/", "Ztouch_console_lay/"},
        {0x1d3b705, "Controller/", "Xtouch_lay/"},
        {0x1d0f5b8, "Controller/ControllerButtons/%s/%s", "Ztouch_controller_buttons_ui/%s_%s"},
        {0x1d076c7, "Controller/Cursor/Idle", "Xtouch_cursor_lay/Idle"},
        {0x1d3acbc, "Controller/Cursor/Hold", "Xtouch_cursor_lay/Hold"},
        {0x1d4d5ea, "Controller/Cursor/Held", "Xtouch_cursor_lay/Held"},
    };
    module_t *main = NULL;
    for (int i = 0; i < g_nmod; ++i) if (!strcmp(g_mod[i].name, "main")) main = &g_mod[i];
    char marker[1100];
    if (!main || !dm_overlay_file("ctrl_touch", marker, sizeof marker)) {
        LOG("touch layout: ctrl_touch overlay marker absent; original paths retained");
        return;
    }
    for (unsigned i = 0; i < sizeof prefixes / sizeof *prefixes; ++i) {
        size_t length = strlen(prefixes[i].pad);
        if (length != strlen(prefixes[i].touch) || prefixes[i].offset + length + 1 > main->size ||
            (memcmp(main->base + prefixes[i].offset, prefixes[i].pad, length + 1) &&
             memcmp(main->base + prefixes[i].offset, prefixes[i].touch, length + 1))) {
            LOG("touch layout: unexpected main+%#x; no prefixes changed", prefixes[i].offset);
            return;
        }
    }
    for (unsigned i = 0; i < sizeof prefixes / sizeof *prefixes; ++i)
        memcpy(main->base + prefixes[i].offset, prefixes[i].touch, strlen(prefixes[i].pad));
    LOG("touch layout: upstream iOS paths selected before nnMain");
}

static int rom_overlay_path(const mount_t *mount, const char *rest, char *out, size_t size) {
    char *roots = strdup(mount->overlay);
    if (!roots) die("rom overlay path allocation failed");
    char *save = NULL;
    for (char *root = strtok_r(roots, ":", &save); root; root = strtok_r(NULL, ":", &save)) {
        if (root_path(root, rest, out, size) && !access(out, F_OK)) { free(roots); return 1; }
    }
    int found = root_path(mount->dir, rest, out, size) && !access(out, F_OK);
    free(roots);
    if (found) return 1;
    errno = ENOENT; return 0;
}

typedef struct {
    DIR *d;
    char path[1024];
    DirectoryEntry *snap;
    int64_t nsnap, psnap;
    int mode, snapshot;
} dirh_t;

static void dir_snapshot(dirh_t *handle, const mount_t *mount, const char *rest) {
    char *roots = strdup(mount->overlay);
    char (*names)[0x301] = calloc(2048, sizeof *names);
    const char **all = calloc(65, sizeof *all);
    char *parent = malloc(1024), *rel = malloc(2048), *resolved = malloc(1024);
    if (!roots || !names || !all || !parent || !rel || !resolved) die("rom overlay snapshot allocation failed");
    int count = 0, nroots = 0;
    all[nroots++] = mount->dir;
    char *save = NULL;
    for (char *root = strtok_r(roots, ":", &save); root && nroots < 65; root = strtok_r(NULL, ":", &save)) all[nroots++] = root;
    for (int i = nroots - 1; i >= 0; --i) {
        if (!root_path(all[i], rest, parent, 1024)) continue;
        DIR *directory = opendir(parent);
        if (!directory) continue;
        struct dirent *entry;
        while ((entry = readdir(directory))) {
            if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
            size_t length = strlen(entry->d_name);
            if (length > 6 && !strcmp(entry->d_name + length - 6, ".holes")) continue;
            int duplicate = 0;
            for (int j = 0; j < count; ++j) if (!strcmp(names[j], entry->d_name)) { duplicate = 1; break; }
            if (!duplicate && count < 2048) snprintf(names[count++], sizeof *names, "%s", entry->d_name);
        }
        closedir(directory);
    }
    handle->snap = calloc(count, sizeof *handle->snap);
    if (count && !handle->snap) die("rom overlay entries allocation failed");
    handle->snapshot = 1;
    for (int j = 0; j < count; ++j) {
        snprintf(rel, 2048, "%s/%s", rest, names[j]);
        if (!rom_overlay_path(mount, rel, resolved, 1024)) continue;
        struct stat st;
        if (stat(resolved, &st)) continue;
        DirectoryEntry *entry = &handle->snap[handle->nsnap++];
        snprintf(entry->name, sizeof entry->name, "%s", names[j]);
        entry->type = S_ISDIR(st.st_mode) ? ENT_DIR : ENT_FILE;
        entry->size = S_ISDIR(st.st_mode) ? 0 : st.st_size;
    }
    free(resolved); free(rel); free(parent); free(all); free(names); free(roots);
}
