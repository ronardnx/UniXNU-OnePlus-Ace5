/* Opt-in undo of the GUI seed experiment, before launchd/cfprefsd start.
 * Preserve the complete preference file with an exclusive atomic rename.
 * This restores DataMigration's decision path; it does not draw a progress bar
 * or change any migration result. No device or arbitrary path is accepted.
 */
int giulia_ios_rename_exclusive(const char *, const char *, vfs_context_t);

static int giulia_restore_migration(void)
{
    static const char directory[] = "/private/var/mobile/Library/Preferences";
    static const char source[] = "/private/var/mobile/Library/Preferences/com.apple.migration.plist";
    static const char backup[] = "/private/var/mobile/Library/Preferences/com.apple.migration.giulia-seed-backup.plist";
    char rd[32] = {0};
    if (!PE_parse_boot_argn("rd", rd, sizeof(rd)) || strcmp(rd, "vermeerufs0") ||
        !vermeer_ufs_ios_root_ready() || !rootvnode ||
        !(vfs_flags(vnode_mount(rootvnode)) & MNT_ROOTFS) ||
        strcmp(vfs_statfs(vnode_mount(rootvnode))->f_fstypename, "hfs")) return EPERM;
    vfs_context_t ctx = vfs_context_kernel();
    vnode_t parent = NULLVP, vp = NULLVP;
    int error = vnode_lookup(directory, VNODE_LOOKUP_NOFOLLOW, &parent, ctx);
    if (error) return error;
    struct vnode_attr va;
    VATTR_INIT(&va); VATTR_WANTED(&va, va_uid);
    error = vnode_getattr(parent, &va, ctx);
    if (!error && (!vnode_isdir(parent) || vnode_mount(parent) != vnode_mount(rootvnode) ||
                  va.va_uid != 501)) error = EPERM;
    vnode_put(parent);
    if (error) return error;

    error = vnode_lookup(backup, VNODE_LOOKUP_NOFOLLOW, &vp, ctx);
    if (!error) {
        vnode_put(vp);
        printf("GIULIA MIGRATION: backup already exists; preserving current migration state\n");
        return 0;
    }
    if (error != ENOENT) return error;
    error = vnode_lookup(source, VNODE_LOOKUP_NOFOLLOW, &vp, ctx);
    if (error == ENOENT) {
        printf("GIULIA MIGRATION: no preference override to restore\n");
        return 0;
    }
    if (error) return error;
    VATTR_INIT(&va); VATTR_WANTED(&va, va_uid); VATTR_WANTED(&va, va_gid);
    VATTR_WANTED(&va, va_data_size);
    error = vnode_getattr(vp, &va, ctx);
    if (!error && (!vnode_isreg(vp) || vnode_mount(vp) != vnode_mount(rootvnode) ||
                  va.va_uid != 501 || va.va_gid != 501 || va.va_data_size > 1048576)) error = EPERM;
    vnode_put(vp);
    if (error) return error;
    error = giulia_ios_rename_exclusive(source, backup, ctx);
    printf("GIULIA MIGRATION: restore preference rename result=%d; backup committed=%d\n", error, error == 0);
    return error;
}
