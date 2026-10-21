/* Host fault-injection test of the exact kernel helper; no real filesystem I/O. */
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef int vfs_context_t;
struct vnode_attr { unsigned va_uid, va_gid; uint64_t va_data_size; };
struct node { int type, mount; struct vnode_attr attrs; };
typedef struct node *vnode_t;
#define NULLVP ((vnode_t)0)
#define MNT_ROOTFS 1
#define VNODE_LOOKUP_NOFOLLOW 1
#define VATTR_INIT(a) memset((a), 0, sizeof(*(a)))
#define VATTR_WANTED(a, field) ((void)(a))
static struct node root, directory, source, backup;
static vnode_t rootvnode = &root;
static struct { char f_fstypename[8]; } fs;
static int ready, source_exists, backup_exists, rename_calls, rename_error;
static int PE_parse_boot_argn(const char *key, void *value, int size) {
    assert(strcmp(key, "rd") == 0 && size >= 12);
    strcpy(value, "vermeerufs0"); return 1;
}
static int vermeer_ufs_ios_root_ready(void) { return ready; }
static int vnode_mount(vnode_t vp) { return vp->mount; }
static int vfs_flags(int mount) { (void)mount; return MNT_ROOTFS; }
#define vfs_statfs(mount) ((void)(mount), &fs)
static vfs_context_t vfs_context_kernel(void) { return 1; }
static int vnode_lookup(const char *path, int flags, vnode_t *vp, vfs_context_t ctx) {
    assert(flags == VNODE_LOOKUP_NOFOLLOW && ctx == 1);
    if (strstr(path, "giulia-seed-backup")) {
        if (!backup_exists) return ENOENT;
        *vp = &backup;
    } else if (strstr(path, "com.apple.migration.plist")) {
        if (!source_exists) return ENOENT;
        *vp = &source;
    } else *vp = &directory;
    return 0;
}
static int vnode_getattr(vnode_t vp, struct vnode_attr *va, vfs_context_t ctx) {
    (void)ctx; *va = vp->attrs; return 0;
}
static int vnode_isdir(vnode_t vp) { return vp->type == 2; }
static int vnode_isreg(vnode_t vp) { return vp->type == 1; }
static void vnode_put(vnode_t vp) { (void)vp; }
int giulia_ios_rename_exclusive(const char *from, const char *to, vfs_context_t ctx) {
    assert(strstr(from, "com.apple.migration.plist") && strstr(to, "giulia-seed-backup"));
    assert(ctx == 1); rename_calls++;
    if (rename_error) return rename_error;
    if (backup_exists) return EEXIST;
    assert(source_exists); source_exists = 0; backup_exists = 1; return 0;
}
#include "../ios16/giulia_restore_migration.h"
static void reset(void) {
    root = (struct node){2, 1, {0, 0, 0}};
    directory = (struct node){2, 1, {501, 501, 0}};
    source = (struct node){1, 1, {501, 501, 512}};
    backup = source; strcpy(fs.f_fstypename, "hfs");
    ready = source_exists = 1; backup_exists = rename_calls = rename_error = 0;
}
int main(void) {
    reset(); assert(!giulia_restore_migration() && !source_exists && backup_exists && rename_calls == 1);
    source_exists = 1; assert(!giulia_restore_migration() && source_exists && rename_calls == 1);
    reset(); source_exists = 0; assert(!giulia_restore_migration() && !rename_calls);
    reset(); source.attrs.va_uid = 0; assert(giulia_restore_migration() == EPERM && !rename_calls);
    reset(); source.attrs.va_gid = 0; assert(giulia_restore_migration() == EPERM && !rename_calls);
    reset(); source.type = 3; assert(giulia_restore_migration() == EPERM && !rename_calls);
    reset(); directory.attrs.va_uid = 0; assert(giulia_restore_migration() == EPERM && !rename_calls);
    reset(); source.mount = 2; assert(giulia_restore_migration() == EPERM && !rename_calls);
    reset(); ready = 0; assert(giulia_restore_migration() == EPERM && !rename_calls);
    reset(); strcpy(fs.f_fstypename, "apfs"); assert(giulia_restore_migration() == EPERM && !rename_calls);
    reset(); source.attrs.va_data_size = 1048577; assert(giulia_restore_migration() == EPERM && !rename_calls);
    reset(); rename_error = EIO; assert(giulia_restore_migration() == EIO && source_exists && !backup_exists);
    puts("Migration restore: 12 success, idempotency, ownership, volume and error cases passed");
    return 0;
}
