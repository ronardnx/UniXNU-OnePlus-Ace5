/* USB worker owns this bounded snapshot buffer; it never queues raw pixels
 * through printf/the RAM text ring. GUI provider snapshots under its display
 * mutex, then releases it before any asynchronous USB transmission.
 */
/* GIULIA_SCREEN_CORE is embedded here by the profile generator. */
/* GIULIA_SCREEN_PROVIDER is embedded here by the profile generator. */
static struct {
    uint8_t frame[GSC_FRAME_BYTES];
    uint32_t id,offset,crc;
    uint64_t next_ms;
    unsigned live,requested,active,snapshot,yield_log,control;
} gscreen;
static uint64_t gscreen_now(void)
{uint64_t ns=0;absolutetime_to_nanoseconds(mach_absolute_time(),&ns);return ns/1000000;}
static void gscreen_disconnect(void)
{gscreen.live=gscreen.requested=gscreen.active=gscreen.yield_log=gscreen.control=0;}
static int gscreen_command(const char *cmd)
{
    if(!strcmp(cmd,"shell")) {
        /* Even a completed snapshot may have left a bad final header at the
         * receiver. A verified boundary precedes every raw PTY transition. */
        gscreen_disconnect();gscreen.control=1;return 0;
    }
    if(strcmp(cmd,"screen.start") && strcmp(cmd,"screen.stop") && strcmp(cmd,"screen.snapshot"))return 0;
    if(!gs_enabled()) {printf("GIULIA SCREEN: requires giulia_debug=1\n");return 1;}
    if(!strcmp(cmd,"screen.stop")) {
        gscreen_disconnect();gscreen.control=1;printf("GIULIA SCREEN: stopped\n");return 1;
    }
    if(!strcmp(cmd,"screen.start")) {if(!gscreen.live) {gscreen.live=1;gscreen.next_ms=0;}}
    else gscreen.requested=1;
    printf("GIULIA SCREEN: queued live=%u snapshot=%u size=%ux%u format=RGB565_LE interval_ms=1000\n",
        gscreen.live,gscreen.requested,GSC_WIDTH,GSC_HEIGHT);
    return 1;
}
static int gscreen_transmit(uint8_t *out,unsigned capacity,unsigned *length)
{
    *length=0;
    if(gscreen.control) {
        *length=gsc_control(out,capacity,gscreen.id);gscreen.control=0;return *length!=0;
    }
    if(gscreen.yield_log) {gscreen.yield_log=0;return 0;}
    if(!gscreen.active) {
        uint64_t now=gscreen_now();
        if((!gscreen.requested && !gscreen.live) || now<gscreen.next_ms)return 0;
        unsigned width=0,height=0;
        int captured=giulia_screen_capture(gscreen.frame,sizeof(gscreen.frame),&width,&height);
        gscreen.next_ms=now+1000;
        if(!captured || width!=GSC_WIDTH || height!=GSC_HEIGHT) {
            gscreen.requested=0;
            printf("GIULIA SCREEN: framebuffer unavailable; no pixels substituted\n");
            return 0;
        }
        gscreen.snapshot=gscreen.requested;gscreen.requested=0;
        gscreen.id++;if(!gscreen.id)gscreen.id=1;
        gscreen.offset=0;gscreen.crc=gsc_crc(gscreen.frame,sizeof(gscreen.frame));gscreen.active=1;
    }
    unsigned n=gsc_packet(out,capacity,u.mps,gscreen.frame,gscreen.id,
        gscreen.snapshot,gscreen.offset,gscreen.crc);
    if(!n) {gscreen_disconnect();printf("GIULIA SCREEN: invalid transport bounds\n");return 0;}
    gscreen.offset+=n-GSC_HEADER_BYTES;
    if(gscreen.offset==GSC_FRAME_BYTES)gscreen.active=0;
    gscreen.yield_log=1;*length=n;return 1;
}
