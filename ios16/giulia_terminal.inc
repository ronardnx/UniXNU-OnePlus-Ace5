/* Included in the CDC translation unit after its transport definitions.
 * Bounded queries and explicit development shell/MMIO/Fastboot commands.
 */
#include <sys/proc.h>
#include <sys/proc_internal.h>
/* vm_page.h is private to the Mach build and is not exported to BSD sources.
 * Match the unsigned counters defined in Darwin 22 vm_resident.c, as vm_unix.c
 * does for its read-only page-count sysctls. */
extern unsigned int vm_page_free_count,vm_page_active_count;
extern unsigned int vm_page_inactive_count,vm_page_wire_count;

/* version[] is already declared by sys/systm.h as a writable char array. */
extern unsigned long gPhysBase, gPhysSize;
extern void giulia_ufs_debug(void);

static int giulia_terminal_proc(proc_t p, void *arg)
{
    unsigned *count = arg;
    char name[64] = {0};
    proc_name(proc_pid(p), name, sizeof(name));
    printf("GIULIA TERM: pid=%d name=%s\n", proc_pid(p), name);
    return ++*count >= 128 ? PROC_RETURNED_DONE : PROC_RETURNED;
}

static int giulia_terminal_command(const char *cmd)
{
    if(gscreen_command(cmd))return 1;
    if(gs_command(cmd) || gm_command(cmd) || gr_command(cmd)) return 1;
    if (!strcmp(cmd,"info")) {
        printf("GIULIA TERM: protocol=2 board=OnePlus-Ace-5 soc=SM8650\n%s\n",version);
    } else if (!strcmp(cmd,"uptime")) {
        uint64_t ns=0;
        absolutetime_to_nanoseconds(mach_absolute_time(),&ns);
        printf("GIULIA TERM: uptime_ms=%llu\n",ns/1000000);
    } else if (!strcmp(cmd,"memory")) {
        printf("GIULIA TERM: boot_ram_base=%#lx boot_ram_bytes=%lu page_bytes=%lu free_pages=%u active_pages=%u inactive_pages=%u wired_pages=%u (approximate counters)\n",
            gPhysBase,gPhysSize,(unsigned long)PAGE_SIZE,vm_page_free_count,
            vm_page_active_count,vm_page_inactive_count,vm_page_wire_count);
    } else if (!strcmp(cmd,"ps")) {
        unsigned count=0;
        proc_iterate(PROC_ALLPROCLIST,giulia_terminal_proc,&count,NULL,NULL);
        printf("GIULIA TERM: processes=%u limit=128 truncated=%u\n",count,count==128);
    } else if (!strcmp(cmd,"bootargs")) {
        const char *names[]={"cpus","serial","vermeer_usb","giulia_touch","giulia_touch_gpio",
            "giulia_gpu","giulia_wifip","giulia_ufs_probe","vermeer_ufs_highdma"};
        for (unsigned i=0;i<sizeof(names)/sizeof(names[0]);i++) {
            int value=0;
            int present=PE_parse_boot_argn(names[i],&value,sizeof(value));
            printf("GIULIA TERM: bootarg=%s present=%d value=%d\n",names[i],present,value);
        }
    } else if (!strcmp(cmd,"display")) {
        printf("GIULIA TERM: boot_framebuffer width=%lu height=%lu rowbytes=%lu depth=%lu (boot geometry; not live scanout health)\n",
            (unsigned long)PE_state.video.v_width,(unsigned long)PE_state.video.v_height,
            (unsigned long)PE_state.video.v_rowBytes,(unsigned long)PE_state.video.v_depth);
    } else if (!strcmp(cmd,"usb")) {
        printf("GIULIA TERM: usb configured=%u dtr=%u failed=%u events=%u setups=%u mps=%u tx=%llu rx=%llu cursor=%llu log=%llu dropped=%llu\n",
            u.configured,u.dtr,u.failed,u.events,u.setups,u.mps,u.tx_bytes,u.rx_bytes,
            u.cursor,vermeer_debug_total(),vermeer_debug_dropped());
    } else if (!strcmp(cmd,"ufs")) {
        giulia_ufs_debug();
    } else if (!strcmp(cmd,"features")) {
        printf("GIULIA TERM: commands=help,ping,status,replay,info,uptime,memory,ps,bootargs,display,usb,ufs,features,sample\n");
        printf("GIULIA TERM: protocol=2 shell=PTY-daemon giulia_debug=%d reg=map/read/write/rmw/dump/unmap (requires giulia_mmio=1)\n",gs_enabled());
        printf("GIULIA TERM: reboot=reboot.fastboot,reboot.status (requires giulia_debug=1; sync before reset)\n");
        printf("GIULIA TERM: screen=screen.start,screen.stop,screen.snapshot GSC1 RGB565_LE 316x695 max_fps=1\n");
    } else return 0;
    printf("GIULIA TERM: end %s\n",cmd);
    return 1;
}
