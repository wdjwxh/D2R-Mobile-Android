// nn::fs 文件系统 HLE — 移植自上游 dm/host/hle_fs.c（Android 精简版）。
// 简化依据：Linux 文件系统天然区分大小写 → 上游的 xattr 大小写映射整段不需要；
// clonefile → 递归拷贝；renamex_np(RENAME_SWAP) → renameat2(RENAME_EXCHANGE)；
// overlay 路径优先级与目录合并快照复用上游，Linux 不需要 xattr 名称映射。
// 挂载点：rom → $DM_ROMFS（18 个 .data 的目录）；temp → $DM_TEMP；save → $DM_SAVE（事务式）。
#include "dm_host.h"
#include "nvn/nvn_dirty.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stddef.h>
#include <stdatomic.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <unistd.h>

typedef uint32_t Result;
#define RES_OK 0
#define RES_PATH_NOT_FOUND 0x202
#define RES_PATH_EXISTS 0x402
#define RES_FAIL 0x3E2

#define MODE_READ 1
#define MODE_WRITE 2
#define MODE_APPEND 4
#define DIRMODE_DIR 1
#define DIRMODE_FILE 2
#define ENT_DIR 0
#define ENT_FILE 1

// Horizon ABI, identical to upstream hle_fs.c. The guest advances by 0x310;
// an extra int32_t pad would move size to 0x310 and make each entry 0x318,
// hiding every directory entry after the first when reopening saved games.
typedef struct { char name[0x301]; uint8_t pad0[3]; int8_t type; uint8_t pad1[3]; int64_t size; } DirectoryEntry;
_Static_assert(sizeof(DirectoryEntry)==0x310,"DirectoryEntry Horizon ABI");
_Static_assert(offsetof(DirectoryEntry,type)==0x304 && offsetof(DirectoryEntry,size)==0x308,"DirectoryEntry field offsets");

#define MAX_MOUNT 8
typedef struct { char name[32]; char dir[1024]; char overlay[4096]; char commit[1024]; int ro; } mount_t;
static mount_t g_mnt[MAX_MOUNT];
static int g_nmnt;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_path_lock = PTHREAD_MUTEX_INITIALIZER;
static void path_unlock(pthread_mutex_t **m) { pthread_mutex_unlock(*m); }
#define PATH_LOCK() pthread_mutex_t *path_guard __attribute__((cleanup(path_unlock))) = &g_path_lock; pthread_mutex_lock(path_guard)
static uint8_t g_fmode[65536];
static uint8_t g_fsave[65536]; /* 1: current working tree; 2: rejected tree. */
static _Atomic int g_save_poison;
static void save_poison(int e) { int zero = 0; atomic_compare_exchange_strong(&g_save_poison, &zero, e ? e : EIO); }
void dm_fs_save_poison(int e) { save_poison(e); }
void front_save_failed(int error);
static int save_path(const char *path) {
    const char *c = strchr(path, ':'); size_t n = c ? (size_t)(c-path) : 0;
    for (int i = 0; c && i < g_nmnt; ++i)
        if (strlen(g_mnt[i].name)==n && !strncmp(g_mnt[i].name,path,n)) return g_mnt[i].commit[0]!=0;
    return 0;
}
static int save_fd(uint64_t h) { return h < sizeof g_fsave ? g_fsave[h] : 0; }

/* Upstream sidecar transaction API. Hooks are empty until their complete
 * gameplay integration explicitly installs them; successful swaps alone count.
 * Like upstream, the mount table is append-only and paths remain stable. */
const char *dm_save_workdir(void) {
    for (int i = 0; i < g_nmnt; ++i) if (g_mnt[i].commit[0]) return g_mnt[i].dir;
    return NULL;
}
const char *dm_save_commitdir(void) {
    for (int i = 0; i < g_nmnt; ++i) if (g_mnt[i].commit[0]) return g_mnt[i].commit;
    return NULL;
}
void (*g_dm_pre_commit_hook[5])(const char *workdir);
static _Atomic uint64_t g_save_commits;
uint64_t dm_fs_commit_count(void) { return atomic_load(&g_save_commits); }

static Result err_res(const char *op, const char *path) {
    if (errno == ENOENT || errno == ENOTDIR) {
        // 保留首次缺文件路径，区分可选配置与真正的资源缺失。
        static unsigned missing;
        if (__atomic_fetch_add(&missing, 1, __ATOMIC_RELAXED) < 32)
            LOG("fs 缺文件 %s: %s", op, path);
        return RES_PATH_NOT_FOUND;
    }
    if (errno == EEXIST) return RES_PATH_EXISTS;
    LOG("fs %s %s errno=%d", op, path, errno);
    return RES_FAIL;
}

static int rm_tree(const char *p) {
    struct stat st;
    if (lstat(p, &st)) return errno == ENOENT ? 0 : -1;
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(p);
        if (!d) return -1;
        struct dirent *e;
        char c[1200];
        while ((e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            snprintf(c, sizeof c, "%s/%s", p, e->d_name);
            if (rm_tree(c)) { closedir(d); return -1; }
        }
        closedir(d);
        return rmdir(p);
    }
    return unlink(p);
}

static int copy_tree(const char *from, const char *to) {
    struct stat st;
    if (stat(from, &st)) return -1;
    // Match upstream clonefile semantics: Rift's save pre-commit check uses
    // the character file's nanosecond mtime to detect an actual rewrite.
    const struct timespec times[2] = {st.st_atim, st.st_mtim};
    if (S_ISDIR(st.st_mode)) {
        if (mkdir(to, 0755) && errno != EEXIST) return -1;
        DIR *d = opendir(from);
        if (!d) return -1;
        struct dirent *e;
        char a[1200], b[1200];
        while ((e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            snprintf(a, sizeof a, "%s/%s", from, e->d_name);
            snprintf(b, sizeof b, "%s/%s", to, e->d_name);
            if (copy_tree(a, b)) { closedir(d); return -1; }
        }
        closedir(d);
        return utimensat(AT_FDCWD, to, times, 0);
    }
    int i = open(from, O_RDONLY), o = open(to, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (i < 0 || o < 0) { if (i >= 0) close(i); if (o >= 0) close(o); return -1; }
    // This HLE runs on small guest stacks. A 64 KiB automatic buffer also
    // inflates every directory recursion frame, overflowing save threads.
    char *buf = malloc(1 << 16);
    if (!buf) { close(i); close(o); return -1; }
    int result = 0;
    for (;;) {
        ssize_t r = read(i, buf, 1 << 16);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) { if (r < 0) result = -1; break; }
        ssize_t used = 0;
        while (used < r) {
            ssize_t n = write(o, buf + used, (size_t)(r - used));
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) { result = -1; break; }
            used += n;
        }
        if (result) break;
    }
    if (!result && futimens(o, times)) result = -1;
    free(buf); close(i); if (close(o)) result = -1;
    return result;
}

static const char *envdir(const char *var, const char *rel, char *buf, size_t n) {
    const char *v = getenv(var);
    if (v) return v;
    snprintf(buf, n, "%s/%s", getenv("DM_ROOT") ? getenv("DM_ROOT") : ".", rel);
    return buf;
}

#include "fs_overlay.inc"

// 路径翻译：客体 "rom:/x/y" → 宿主 <挂载目录>/x/y（组件级 ".." 拒绝；Linux 大小写敏感即原样）
static int host_path_ex(const char *p, char *out, size_t n, int *ro, int create) {
    const char *c = strchr(p, ':');
    if (!c) { errno = ENOENT; return 0; }
    size_t ln = c - p;
    for (int i = 0; i < g_nmnt; i++) {
        if (strlen(g_mnt[i].name) != ln || strncmp(g_mnt[i].name, p, ln)) continue;
        if (ro) *ro = g_mnt[i].ro;
        const char *rest = c + 1;
        if (g_mnt[i].overlay[0] && !create) return rom_overlay_path(&g_mnt[i], rest, out, n);
        snprintf(out, n, "%s", g_mnt[i].dir);
        size_t used = strlen(out);
        while (*rest) {
            while (*rest == '/') rest++;
            if (!*rest) break;
            const char *end = strchr(rest, '/'); if (!end) end = rest + strlen(rest);
            size_t k = end - rest;
            if (k >= 0x301) { errno = ENAMETOOLONG; return 0; }
            if (k == 2 && !strncmp(rest, "..", 2)) { errno = ENOENT; return 0; }
            if (used + 1 + k + 1 >= n) { errno = ENAMETOOLONG; return 0; }
            out[used++] = '/';
            memcpy(out + used, rest, k); used += k; out[used] = 0;
            const char *next = end; while (*next == '/') next++;
            if (create && !*next) return 1;   // 末段允许不存在（创建语义）
            if (access(out, F_OK) && !create) { errno = ENOENT; return 0; }
            rest = end;
        }
        return 1;
    }
    errno = ENOENT;
    return 0;
}

#define HP_EX(path, create) PATH_LOCK(); char hp[1024]; int ro = 0; if (!host_path_ex(path, hp, sizeof hp, &ro, create)) return err_res("path", path)
#define HP(path) HP_EX(path, 0)
#define RW() if (ro) { LOG("fs 写只读挂载：%s", hp); return RES_FAIL; }

static Result mount_ex(const char *name, const char *dir, int ro, int txn) {
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < g_nmnt; i++)
        if (!strcmp(g_mnt[i].name, name)) { pthread_mutex_unlock(&g_lock); return RES_OK; }
    if (g_nmnt >= MAX_MOUNT) die("挂载表满");
    mount_t *m = &g_mnt[g_nmnt];
    snprintf(m->name, sizeof m->name, "%s", name);
    m->ro = ro; m->commit[0] = 0; m->dir[0] = 0;
    m->overlay[0] = 0;
    // MountRom accepts a guest-selected mount name (the game uses "$"),
    // so identify its read-only mount by type rather than a literal alias.
    if (ro && !txn && getenv("DM_ROMFS_OVERLAY"))
        snprintf(m->overlay, sizeof m->overlay, "%s", getenv("DM_ROMFS_OVERLAY"));
    if (txn) {
        char work[1100];
        snprintf(work, sizeof work, "%s.work", dir);
        mkdir(dir, 0755);
        rm_tree(work);
        if (copy_tree(dir, work)) die("存档事务：拷贝 %s → %s 失败", dir, work);
        snprintf(m->commit, sizeof m->commit, "%s", dir);
        snprintf(m->dir, sizeof m->dir, "%s", work);
    } else {
        snprintf(m->dir, sizeof m->dir, "%s", dir);
    }
    g_nmnt++;
    pthread_mutex_unlock(&g_lock);
    if (!ro && !txn) mkdir(dir, 0755);
    LOG("fs 挂载 %s: → %s%s", name, m->dir, ro ? "（只读）" : txn ? "（事务式存档）" : "");
    if (m->overlay[0]) LOG("fs RomFS overlay: %s", m->overlay);
    return RES_OK;
}

// ---------- 接口（24 个，名称与上游一致） ----------
static Result h_fs_QueryRomCache(unsigned long *out) { *out = 0x1000; return RES_OK; }
static Result h_fs_MountRom(const char *name, void *cache, unsigned long sz) {
    (void)cache; (void)sz;
    char b[1024];
    return mount_ex(name, envdir("DM_ROMFS", "romfs", b, sizeof b), 1, 0);
}
static Result h_fs_MountTemp(const char *name) {
    char b[1024];
    return mount_ex(name, envdir("DM_TEMP", "temp", b, sizeof b), 0, 0);
}
static Result h_fs_EnsureSave(const void *uid) { (void)uid; return RES_OK; }
static Result h_fs_MountSave(const char *name, const void *uid) {
    (void)uid;
    char b[1024];
    return mount_ex(name, envdir("DM_SAVE", "save", b, sizeof b), 0, 1);
}
static Result h_fs_Commit(const char *name) {
    PATH_LOCK();
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < g_nmnt; i++) if (!strcmp(g_mnt[i].name, name) && g_mnt[i].commit[0]) {
        for (int k = 0; k < 5; ++k)
            if (g_dm_pre_commit_hook[k]) g_dm_pre_commit_hook[k](g_mnt[i].dir);
        int perr = atomic_exchange(&g_save_poison, 0);
        if (perr) {
            /* Upstream restores the last committed tree on any failed save.
             * Keep the rejected working copy for recovery, instead of deleting
             * it. Build the replacement first so a full disk cannot destroy it. */
            char rejected[1200];
            snprintf(rejected, sizeof rejected, "%s.rejected.XXXXXX", g_mnt[i].commit);
            if (!mkdtemp(rejected) || copy_tree(g_mnt[i].commit, rejected) ||
                syscall(SYS_renameat2, AT_FDCWD, rejected, AT_FDCWD, g_mnt[i].dir, 2))
                die("Save rollback failed errno=%d; committed save preserved", errno);
            int nopen = 0;
            for (size_t k = 0; k < sizeof g_fsave; ++k)
                if (g_fsave[k]) { g_fsave[k] = 2; ++nopen; }
            pthread_mutex_unlock(&g_lock);
            LOG("Save commit rejected errno=%d; last committed save restored; rejected files retained at %s; stale handles=%d", perr, rejected, nopen);
            front_save_failed(perr);
            return RES_FAIL;
        }
        char neu[1100];
        snprintf(neu, sizeof neu, "%s.new", g_mnt[i].commit);
        if (rm_tree(neu)) { pthread_mutex_unlock(&g_lock); return err_res("commit cleanup", neu); }
        if (copy_tree(g_mnt[i].dir, neu)) { int e=errno; pthread_mutex_unlock(&g_lock); LOG("fs Commit 拷贝失败"); front_save_failed(e); return RES_FAIL; }
        // Match upstream: exchange the snapshot and committed tree. Keep .work
        // intact so open guest file handles continue to address the working tree.
        long r = syscall(SYS_renameat2, AT_FDCWD, neu, AT_FDCWD, g_mnt[i].commit, 2 /*RENAME_EXCHANGE*/);
        if (r == 0) {
            atomic_fetch_add(&g_save_commits, 1);
            if (rm_tree(neu)) LOG("fs Commit 旧快照清理失败 errno=%d", errno);
        } else {
            // Never report success after a failed exchange or replace a valid
            // committed save through a non-atomic delete/rename fallback.
            int saved_errno = errno;
            rm_tree(neu);
            pthread_mutex_unlock(&g_lock);
            front_save_failed(saved_errno);
            errno = saved_errno;
            return err_res("commit exchange", name);
        }
        pthread_mutex_unlock(&g_lock);
        LOG("fs Commit %s 完成", name);
        return RES_OK;
    }
    pthread_mutex_unlock(&g_lock);
    return RES_PATH_NOT_FOUND;
}
static void h_fs_Unmount(const char *name) { (void)name; }

static Result h_fs_OpenFile(uint64_t *out, const char *path, int mode) {
    HP(path);
    if (mode & (MODE_WRITE | MODE_APPEND)) RW();
    int fd = open(hp, (mode & (MODE_WRITE | MODE_APPEND)) ? ((mode & MODE_READ) ? O_RDWR : O_WRONLY) : O_RDONLY);
    if (fd < 0) return err_res("open", hp);
    struct stat st;
    fstat(fd, &st);
    if (S_ISDIR(st.st_mode)) { close(fd); return RES_PATH_NOT_FOUND; }
    if (fd >= (int)sizeof g_fmode) { close(fd); errno=EMFILE; return err_res("open tracking",hp); }
    g_fmode[fd] = (uint8_t)mode; g_fsave[fd] = (uint8_t)save_path(path);
    *out = (uint64_t)fd;
    return RES_OK;
}
static Result h_fs_ReadFile(unsigned long *out, uint64_t h, int64_t off, void *buf, unsigned long sz) {
    // Kernel copy_to_user returns EFAULT on protected pages; it cannot invoke
    // our userspace write-fault handler like an ordinary guest store does.
    dm_guest_write_prep(buf, sz);
    unsigned long got = 0;
    while (got < sz) {
        ssize_t r = pread((int)h, (uint8_t *)buf + got, sz - got, off + got);
        if (r < 0) { if (errno == EINTR) continue; return err_res("pread", "fd"); }
        if (r == 0) break;
        got += r;
    }
    *out = got;
    return RES_OK;
}
static Result h_fs_WriteFile(uint64_t h, int64_t off, const void *buf, unsigned long sz, const void *opt) {
    PATH_LOCK();
    (void)opt;
    if (save_fd(h)==2) { errno=ESTALE; save_poison(errno); return err_res("write rejected handle","fd"); }
    unsigned long put = 0;
    while (put < sz) {
        ssize_t r = pwrite((int)h, (const uint8_t *)buf + put, sz - put, off + put);
        if (r < 0) { if (errno == EINTR) continue; if (save_fd(h)) save_poison(errno); return err_res("pwrite", "fd"); }
        if (r == 0) { errno=ENOSPC; if (save_fd(h)) save_poison(errno); return err_res("pwrite zero bytes","fd"); }
        put += r;
    }
    return RES_OK;
}
static Result h_fs_GetFileSize(int64_t *out, uint64_t h) {
    struct stat st;
    if (fstat((int)h, &st)) return err_res("fstat", "fd");
    *out = st.st_size;
    return RES_OK;
}
static Result h_fs_SetFileSize(uint64_t h, int64_t sz) {
    PATH_LOCK();
    if (save_fd(h)==2) { errno=ESTALE; save_poison(errno); return err_res("truncate rejected handle","fd"); }
    if (ftruncate((int)h, sz)) { if (save_fd(h)) save_poison(errno); return err_res("ftruncate", "fd"); }
    return RES_OK;
}
static Result h_fs_FlushFile(uint64_t h) { (void)h; return RES_OK; }
static void h_fs_CloseFile(uint64_t h) {
    PATH_LOCK();
    int is_save=save_fd(h);
    if (h < sizeof g_fmode) { g_fmode[h] = 0; g_fsave[h] = 0; }
    if (close((int)h) && is_save) save_poison(errno);
}
static int h_fs_GetFileOpenMode(uint64_t h) { return (int)h < (int)sizeof g_fmode ? g_fmode[(int)h] : 0; }
static Result h_fs_CreateFile(const char *path, int64_t sz) {
    HP_EX(path, 1);
    int fd = open(hp, O_CREAT | O_EXCL | O_WRONLY, 0644);
    if (fd < 0) return err_res("create", hp);
    if (sz > 0 && ftruncate(fd, sz)) { int e=errno; close(fd); if (save_path(path)) save_poison(e); errno=e; return err_res("create size",hp); }
    if (close(fd)) { if (save_path(path)) save_poison(errno); return err_res("create close",hp); }
    return RES_OK;
}
static Result h_fs_DeleteFile(const char *path) {
    HP(path);
    return unlink(hp) ? err_res("unlink", hp) : RES_OK;
}
static Result h_fs_CreateDirectory(const char *path) {
    HP_EX(path, 1);
    return mkdir(hp, 0755) ? err_res("mkdir", hp) : RES_OK;
}
static Result h_fs_DeleteDirRec(const char *path) {
    HP(path);
    return rm_tree(hp) ? err_res("rmtree", hp) : RES_OK;
}
static Result h_fs_RenameFile(const char *from, const char *to) {
    HP_EX(from, 1);
    char hp2[1024];
    int ro2 = 0;
    if (!host_path_ex(to, hp2, sizeof hp2, &ro2, 1)) return err_res("path", to);
    if (ro || ro2) { LOG("fs 改名涉只读挂载"); return RES_FAIL; }
    return rename(hp, hp2) ? err_res("rename", hp) : RES_OK;
}
static Result h_fs_GetEntryType(int32_t *out, const char *path) {
    HP(path);
    struct stat st;
    if (stat(hp, &st)) return err_res("stat", hp);
    *out = S_ISDIR(st.st_mode) ? ENT_DIR : ENT_FILE;
    return RES_OK;
}
static Result h_fs_OpenDirectory(uint64_t *out, const char *path, int mode) {
    HP(path);
    DIR *d = opendir(hp);
    if (!d) return err_res("opendir", hp);
    dirh_t *h = calloc(1, sizeof *h);
    if (!h) { closedir(d); return RES_FAIL; }
    h->d = d;
    snprintf(h->path, sizeof h->path, "%s", hp);
    h->mode = mode;
    const char *colon = strchr(path, ':');
    if (colon) for (int i = 0; i < g_nmnt; ++i)
        if (g_mnt[i].overlay[0] && strlen(g_mnt[i].name) == (size_t)(colon - path) &&
            !strncmp(g_mnt[i].name, path, (size_t)(colon - path))) {
            dir_snapshot(h, &g_mnt[i], colon + 1);
            break;
        }
    *out = (uint64_t)h;
    return RES_OK;
}
static Result h_fs_ReadDirectory(int64_t *count, DirectoryEntry *ents, uint64_t hh, int64_t max) {
    dirh_t *h = (void *)hh;
    int64_t n = 0;
    if (h->snapshot) {
        while (n < max && h->psnap < h->nsnap) {
            DirectoryEntry *entry = &h->snap[h->psnap++];
            if (!(h->mode & (entry->type == ENT_DIR ? DIRMODE_DIR : DIRMODE_FILE))) continue;
            ents[n++] = *entry;
        }
        *count = n;
        return RES_OK;
    }
    struct dirent *e;
    char c[1200];
    while (n < max && (e = readdir(h->d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        { size_t dl = strlen(e->d_name); if (dl > 6 && !strcmp(e->d_name + dl - 6, ".holes")) continue; }
        snprintf(c, sizeof c, "%s/%s", h->path, e->d_name);
        struct stat st;
        if (stat(c, &st)) continue;
        int isdir = S_ISDIR(st.st_mode);
        if (!(h->mode & (isdir ? DIRMODE_DIR : DIRMODE_FILE))) continue;
        DirectoryEntry *o = &ents[n++];
        memset(o, 0, sizeof *o);
        snprintf(o->name, sizeof o->name, "%s", e->d_name);
        o->type = isdir ? ENT_DIR : ENT_FILE;
        o->size = isdir ? 0 : st.st_size;
    }
    *count = n;
    return RES_OK;
}
static void h_fs_CloseDirectory(uint64_t hh) {
    dirh_t *h = (void *)hh;
    closedir(h->d);
    free(h->snap);
    free(h);
}

// 强制接管表（force_lookup 的 fs 段）
typedef struct { const char *name; void *fn; } hle_ent;
const hle_ent *fs_force_lookup(const char *name) {
    static const hle_ent F[] = {
        {"_ZN2nn2fs22QueryMountRomCacheSizeEPm", (void *)h_fs_QueryRomCache},
        {"_ZN2nn2fs8MountRomEPKcPvm", (void *)h_fs_MountRom},
        {"_ZN2nn2fs21MountTemporaryStorageEPKc", (void *)h_fs_MountTemp},
        {"_ZN2nn2fs14EnsureSaveDataERKNS_7account3UidE", (void *)h_fs_EnsureSave},
        {"_ZN2nn2fs13MountSaveDataEPKcRKNS_7account3UidE", (void *)h_fs_MountSave},
        {"_ZN2nn2fs6CommitEPKc", (void *)h_fs_Commit},
        {"_ZN2nn2fs7UnmountEPKc", (void *)h_fs_Unmount},
        {"_ZN2nn2fs8OpenFileEPNS0_10FileHandleEPKci", (void *)h_fs_OpenFile},
        {"_ZN2nn2fs8ReadFileEPmNS0_10FileHandleElPvm", (void *)h_fs_ReadFile},
        {"_ZN2nn2fs9WriteFileENS0_10FileHandleElPKvmRKNS0_11WriteOptionE", (void *)h_fs_WriteFile},
        {"_ZN2nn2fs11GetFileSizeEPlNS0_10FileHandleE", (void *)h_fs_GetFileSize},
        {"_ZN2nn2fs11SetFileSizeENS0_10FileHandleEl", (void *)h_fs_SetFileSize},
        {"_ZN2nn2fs9FlushFileENS0_10FileHandleE", (void *)h_fs_FlushFile},
        {"_ZN2nn2fs9CloseFileENS0_10FileHandleE", (void *)h_fs_CloseFile},
        {"_ZN2nn2fs15GetFileOpenModeENS0_10FileHandleE", (void *)h_fs_GetFileOpenMode},
        {"_ZN2nn2fs10CreateFileEPKcl", (void *)h_fs_CreateFile},
        {"_ZN2nn2fs10DeleteFileEPKc", (void *)h_fs_DeleteFile},
        {"_ZN2nn2fs15CreateDirectoryEPKc", (void *)h_fs_CreateDirectory},
        {"_ZN2nn2fs26DeleteDirectoryRecursivelyEPKc", (void *)h_fs_DeleteDirRec},
        {"_ZN2nn2fs10RenameFileEPKcS2_", (void *)h_fs_RenameFile},
        {"_ZN2nn2fs12GetEntryTypeEPNS0_18DirectoryEntryTypeEPKc", (void *)h_fs_GetEntryType},
        {"_ZN2nn2fs13OpenDirectoryEPNS0_15DirectoryHandleEPKci", (void *)h_fs_OpenDirectory},
        {"_ZN2nn2fs13ReadDirectoryEPlPNS0_14DirectoryEntryENS0_15DirectoryHandleEl", (void *)h_fs_ReadDirectory},
        {"_ZN2nn2fs14CloseDirectoryENS0_15DirectoryHandleE", (void *)h_fs_CloseDirectory},
    };
    for (unsigned i = 0; i < sizeof F / sizeof F[0]; i++)
        if (!strcmp(F[i].name, name)) return &F[i];
    return NULL;
}
