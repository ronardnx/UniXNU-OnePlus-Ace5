/* CDC-backed byte stream. One root daemon owns the device, one host session.
 * All syscalls are nonblocking; userspace polls with bounded buffers.
 */
#include <sys/conf.h>
#include <sys/uio.h>
#include <sys/kauth.h>
#include <miscfs/devfs/devfs.h>
#include <kern/locks.h>
/* GIULIA_DEBUG_ABI is embedded here by the profile generator. */
#define GS_SIZE 32768u
static LCK_GRP_DECLARE(gs_group,"giulia_shell");
static LCK_MTX_DECLARE(gs_lock,&gs_group);
static struct {
    unsigned char rx[GS_SIZE],tx[GS_SIZE];
    unsigned rin,rout,tin,tout;
    bool opened,active,ending,registered;
    uint32_t generation,columns,rows;
} gs;

static int gs_enabled(void)
{
    int enabled=0;
    PE_parse_boot_argn("giulia_debug",&enabled,sizeof(enabled));
    return enabled==1;
}
static void gs_end_locked(void)
{
    if (gs.active) { gs.active=false; gs.ending=true; gs.generation++; }
    gs.rin=gs.rout=0;
}
static void gs_disconnect(void)
{
    lck_mtx_lock(&gs_lock);
    gs_end_locked();
    gs.tin=gs.tout=0; gs.ending=false;
    lck_mtx_unlock(&gs_lock);
}
static int gs_open(dev_t dev, int flags, int type, struct proc *p)
{
    (void)dev;(void)flags;(void)type;(void)p;
    if (!gs_enabled() || kauth_cred_getuid(kauth_cred_get())!=0) return EPERM;
    lck_mtx_lock(&gs_lock);
    int error=gs.opened?EBUSY:0;
    if (!error) gs.opened=true;
    lck_mtx_unlock(&gs_lock);
    return error;
}
static int gs_close(dev_t dev, int flags, int type, struct proc *p)
{
    (void)dev;(void)flags;(void)type;(void)p;
    lck_mtx_lock(&gs_lock); gs_end_locked(); gs.opened=false; lck_mtx_unlock(&gs_lock);
    return 0;
}
static int gs_read(dev_t dev, struct uio *io, int flags)
{
    (void)dev;(void)flags;
    if (!uio_resid(io)) return 0;
    unsigned char data[512]; unsigned n=0;
    lck_mtx_lock(&gs_lock);
    while (gs.active && gs.rout!=gs.rin && n<sizeof(data) && n<(unsigned)uio_resid(io))
        data[n++]=gs.rx[gs.rout++ % GS_SIZE];
    lck_mtx_unlock(&gs_lock);
    return n?uiomove((caddr_t)data,(int)n,io):EWOULDBLOCK;
}
static int gs_write(dev_t dev, struct uio *io, int flags)
{
    (void)dev;(void)flags;
    unsigned char data[512];
    if (!uio_resid(io)) return 0;
    lck_mtx_lock(&gs_lock);
    if (!gs.active) { lck_mtx_unlock(&gs_lock); return EPIPE; }
    unsigned room=GS_SIZE-(gs.tin-gs.tout);
    unsigned n=(unsigned)MIN(uio_resid(io),(user_ssize_t)sizeof(data));
    if (n>room) n=room;
    if (!n) { lck_mtx_unlock(&gs_lock); return EWOULDBLOCK; }
    /* Sleeping mutex, not a spinlock; uiomove may fault. Single opener. */
    int error=uiomove((caddr_t)data,(int)n,io);
    if (!error) for(unsigned i=0;i<n;i++) gs.tx[gs.tin++ % GS_SIZE]=data[i];
    lck_mtx_unlock(&gs_lock);
    return error;
}
static int gs_ioctl(dev_t dev, u_long cmd, caddr_t data, int flags, struct proc *p)
{
    (void)dev;(void)flags;(void)p;
    int error=0;
    lck_mtx_lock(&gs_lock);
    if(cmd==GIULIA_SHELL_STATE) {
        struct giulia_shell_state *s=(void *)data;
        s->active=gs.active;s->generation=gs.generation;s->columns=gs.columns;s->rows=gs.rows;
    } else if(cmd==GIULIA_SHELL_END) gs_end_locked();
    else error=ENOTTY;
    lck_mtx_unlock(&gs_lock);
    return error;
}
static const struct cdevsw gs_cdevsw={
    .d_open=gs_open,.d_close=gs_close,.d_read=gs_read,.d_write=gs_write,.d_ioctl=gs_ioctl,
    .d_stop=eno_stop,.d_reset=eno_reset,.d_select=eno_select,.d_mmap=eno_mmap,.d_strategy=eno_strat,
    .d_reserved_1=eno_getc,.d_reserved_2=eno_putc,.d_type=0,
};
static void gs_register(void)
{
    if (!gs_enabled() || gs.registered) return;
    int major=cdevsw_add(-1,&gs_cdevsw);
    if(major<0) { printf("GIULIA SHELL: cdev registration failed\n"); return; }
    if(!devfs_make_node(makedev(major,0),DEVFS_CHAR,UID_ROOT,GID_WHEEL,0600,"giulia-debug")) {
        (void)cdevsw_remove(major,&gs_cdevsw);
        printf("GIULIA SHELL: devfs registration failed\n"); return;
    }
    gs.columns=80;gs.rows=24;gs.registered=true;
    printf("GIULIA SHELL: /dev/giulia-debug ready; requires root PTY daemon\n");
}
static int gs_command(const char *cmd)
{
    if(!strncmp(cmd,"shell.size ",11)) {
        const char *p=cmd+11;unsigned values[2]={0,0};bool valid=true;
        for(unsigned i=0;i<2;i++) {
            while(*p==' ')p++;
            unsigned digits=0;
            while(*p>='0' && *p<='9') {
                if(values[i]>1000) {valid=false;break;}
                values[i]=10*values[i]+(unsigned)(*p++-'0');digits++;
            }
            if(!digits || !values[i] || values[i]>1000)valid=false;
        }
        while(*p==' ')p++;
        if(!valid || *p) {printf("GIULIA SHELL: size requires columns rows in 1..1000\n");return 1;}
        lck_mtx_lock(&gs_lock);gs.columns=values[0];gs.rows=values[1];lck_mtx_unlock(&gs_lock);
        printf("GIULIA SHELL: size=%ux%u\n",values[0],values[1]);return 1;
    }
    if(strcmp(cmd,"shell")) return 0;
    if(!gs_enabled()) { printf("GIULIA SHELL: disabled; boot giulia_debug=1\n"); return 1; }
    lck_mtx_lock(&gs_lock);
    if(!gs.opened) { lck_mtx_unlock(&gs_lock); printf("GIULIA SHELL: daemon unavailable\n"); return 1; }
    if(gs.active || gs.ending) { lck_mtx_unlock(&gs_lock); return 1; }
    gs.rin=gs.rout=gs.tin=gs.tout=0;
    const char *marker=GIULIA_SHELL_BEGIN;
    while(*marker) gs.tx[gs.tin++]=*marker++;
    gs.active=true;gs.generation++;
    lck_mtx_unlock(&gs_lock);
    return 1;
}
static int gs_receive(const uint8_t *data,unsigned length)
{
    lck_mtx_lock(&gs_lock);
    if(!gs.active && !gs.ending) { lck_mtx_unlock(&gs_lock); return 0; }
    for(unsigned i=0;i<length && gs.active;i++) {
        if(data[i]==GIULIA_SHELL_ESCAPE) { gs_end_locked(); break; }
        if(gs.rin-gs.rout==GS_SIZE) {
            /* End the session rather than silently corrupt an input command. */
            gs_end_locked(); break;
        }
        gs.rx[gs.rin++ % GS_SIZE]=data[i];
    }
    lck_mtx_unlock(&gs_lock);
    return 1;
}
static int gs_transmit(char *out,unsigned capacity,unsigned *length)
{
    lck_mtx_lock(&gs_lock);
    if(!gs.active && !gs.ending) { lck_mtx_unlock(&gs_lock); return 0; }
    unsigned n=0;
    while(gs.tout!=gs.tin && n<capacity) out[n++]=(char)gs.tx[gs.tout++ % GS_SIZE];
    if(gs.ending && gs.tin==gs.tout && capacity-n>=sizeof(GIULIA_SHELL_FINISH)-1) {
        memcpy(out+n,GIULIA_SHELL_FINISH,sizeof(GIULIA_SHELL_FINISH)-1);
        n+=sizeof(GIULIA_SHELL_FINISH)-1;gs.ending=false;
    }
    *length=n;
    lck_mtx_unlock(&gs_lock);
    return 1;
}
