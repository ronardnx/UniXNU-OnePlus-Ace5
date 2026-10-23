/* Snapshot the existing UFS owner under its lock; never wait behind disk I/O. */
void giulia_ufs_debug(void);
void giulia_ufs_debug(void)
{
    if (!lck_mtx_try_lock(&vermeer_ufs_lock)) {
        printf("GIULIA TERM: ufs busy; retry later\n");
        return;
    }
    printf("GIULIA TERM: ufs ready=%u failed=%u owned=%u attempted=%u writable=%u root_valid=%u first=%llu blocks=%llu sector_bytes=%u bmajor=%d cmajor=%d\n",
        vb.ready,vb.failed,vb.owned,vb.attempted,vb.writable,vb.root_valid,
        vb.first,vb.blocks,vb.secsize,vb.bmajor,vb.cmajor);
    lck_mtx_unlock(&vermeer_ufs_lock);
}
