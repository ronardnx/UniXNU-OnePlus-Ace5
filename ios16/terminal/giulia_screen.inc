/* USB worker owns this bounded snapshot buffer; it never queues raw pixels
 * through printf/the RAM text ring. GUI provider snapshots under its display
 * mutex, then releases it before any asynchronous USB transmission.
 */
/* GIULIA_SCREEN_CORE is embedded here by the profile generator. */
/* GIULIA_SCREEN_PROVIDER is embedded here by the profile generator. */
/* GIULIA_INPUT_CORE is embedded here by the profile generator. */
static struct {
    uint8_t frame[GSC_MAX_FRAME_BYTES];
    uint32_t id,offset,crc;
    uint64_t next_ms;
    unsigned live,requested,active,snapshot,yield_log,control,width,height,bytes,max_size,max_fps;
} gscreen={.max_size=695,.max_fps=1};
static uint64_t gscreen_now(void)
{uint64_t ns=0;absolutetime_to_nanoseconds(mach_absolute_time(),&ns);return ns/1000000;}
static void gscreen_disconnect(void)
{ginput_release();gscreen.live=gscreen.requested=gscreen.active=gscreen.yield_log=gscreen.control=0;}
static int gscreen_command(const char *cmd)
{
    uint64_t now=gscreen_now();ginput_tick(now);
    if(ginput_command(cmd,now))return 1;
    if(!strcmp(cmd,"reboot.fastboot"))ginput_release();
    if(!strcmp(cmd,"shell")) {
        /* Even a completed snapshot may have left a bad final header at the
         * receiver. A verified boundary precedes every raw PTY transition. */
        gscreen_disconnect();gscreen.control=1;return 0;
    }
    if(strcmp(cmd,"screen.start") && strcmp(cmd,"screen.stop") && strcmp(cmd,"screen.snapshot") &&
       strncmp(cmd,"screen.configure",16))return 0;
    if(!gs_enabled()) {printf("GIULIA SCREEN: requires giulia_debug=1\n");return 1;}
    if(!strncmp(cmd,"screen.configure",16)) {
        const char *p=cmd+16;unsigned max_size,max_fps;
        if(*p!=' ' || !ginput_uint(&p,2780,&max_size) || max_size<160 ||
           !ginput_uint(&p,30,&max_fps) || !max_fps || !ginput_end(p)) {
            printf("GIULIA SCREEN: rejected=invalid-config max_size=160..2780 max_fps=1..30\n");return 1;
        }
        gscreen.max_size=max_size;gscreen.max_fps=max_fps;gscreen.next_ms=0;
        printf("GIULIA SCREEN: configured max_size=%u max_fps=%u width=%u height=%u format=RGB565_LE\n",
            max_size,max_fps,(unsigned)((uint64_t)GSC_MAX_WIDTH*max_size/GSC_MAX_HEIGHT),max_size);
        return 1;
    }
    if(!strcmp(cmd,"screen.stop")) {
        gscreen_disconnect();gscreen.control=1;printf("GIULIA SCREEN: stopped\n");return 1;
    }
    if(!strcmp(cmd,"screen.start")) {if(!gscreen.live) {gscreen.live=1;gscreen.next_ms=0;}}
    else gscreen.requested=1;
    printf("GIULIA SCREEN: queued live=%u snapshot=%u size=%ux%u format=RGB565_LE max_size=%u max_fps=%u\n",
        gscreen.live,gscreen.requested,(unsigned)((uint64_t)GSC_MAX_WIDTH*gscreen.max_size/GSC_MAX_HEIGHT),
        gscreen.max_size,gscreen.max_size,gscreen.max_fps);
    return 1;
}
static int gscreen_transmit(uint8_t *out,unsigned capacity,unsigned *length)
{
    *length=0;
    ginput_tick(gscreen_now());
    if(gscreen.control) {
        *length=gsc_control(out,capacity,gscreen.id);gscreen.control=0;return *length!=0;
    }
    if(gscreen.yield_log) {gscreen.yield_log=0;return 0;}
    if(!gscreen.active) {
        uint64_t now=gscreen_now();
        if((!gscreen.requested && !gscreen.live) || now<gscreen.next_ms)return 0;
        unsigned width=0,height=0;
        int captured=giulia_screen_capture_size(gscreen.frame,sizeof(gscreen.frame),&width,&height,gscreen.max_size);
        gscreen.next_ms=now+(1000u+gscreen.max_fps-1u)/gscreen.max_fps;
        if(!captured || !width || width>GSC_MAX_WIDTH || !height || height>GSC_MAX_HEIGHT) {
            gscreen.requested=0;
            printf("GIULIA SCREEN: framebuffer unavailable; no pixels substituted\n");
            return 0;
        }
        gscreen.snapshot=gscreen.requested;gscreen.requested=0;
        gscreen.id++;if(!gscreen.id)gscreen.id=1;
        gscreen.width=width;gscreen.height=height;gscreen.bytes=width*height*2u;
        gscreen.offset=0;gscreen.crc=gsc_crc(gscreen.frame,gscreen.bytes);gscreen.active=1;
    }
    unsigned n=gsc_packet_size(out,capacity,u.mps,gscreen.frame,gscreen.id,
        gscreen.snapshot,gscreen.offset,gscreen.crc,gscreen.width,gscreen.height);
    if(!n) {gscreen_disconnect();printf("GIULIA SCREEN: invalid transport bounds\n");return 0;}
    gscreen.offset+=n-GSC_HEADER_BYTES;
    if(gscreen.offset==gscreen.bytes)gscreen.active=0;
    gscreen.yield_log=1;*length=n;return 1;
}
