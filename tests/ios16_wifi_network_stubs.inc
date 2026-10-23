/* Host-only OS boundaries. All network bridge bodies come from production. */
#include <cerrno>
enum {IFF_RUNNING=0x40,IFNET_FAMILY_ETHERNET=1,KEV_VENDOR_APPLE=1,
    KEV_NETWORK_CLASS=1,KEV_DL_SUBCLASS=2,KEV_DL_LINK_ON=16,KEV_DL_LINK_OFF=17,
    MBUF_DONTWAIT=0,MBUF_MCAST=1,MBUF_BCAST=2};
struct net_event_data {unsigned if_family,if_unit;char if_name[16];};
struct kev_msg {
    unsigned vendor_code,kev_class,kev_subclass,event_code;
    struct {void *data_ptr;size_t data_length;} dv[1];
};
static std::vector<unsigned> networkEvents;
struct NetworkEvent {unsigned code,kind;std::vector<uint8_t> data;};
static std::vector<NetworkEvent> networkEventData;
static int kev_post_msg(kev_msg *event) {
    networkEvents.push_back(event->event_code);const auto *p=(const uint8_t *)event->dv[0].data_ptr;
    networkEventData.push_back({event->event_code,event->kev_class,std::vector<uint8_t>(p,p+event->dv[0].data_length)});return 0;
}
static size_t host_strlcpy(char *dst,const char *src,size_t n) {
    size_t bytes=strlen(src);if(n){size_t count=bytes<n-1?bytes:n-1;memcpy(dst,src,count);dst[count]=0;}return bytes;
}
#define strlcpy host_strlcpy
static unsigned networkFlags;
static int ifnet_set_flags(ifnet_t,unsigned flags,unsigned mask) {networkFlags=(networkFlags&~mask)|(flags&mask);return 0;}
struct FakeMbuf {
    std::vector<uint8_t> bytes;
    size_t offset=0,length=0;
    void *header=nullptr;
    ifnet_t received=nullptr;
    unsigned flags=0;
    bool copyFailure=false;
};
using mbuf_t = FakeMbuf *;
static unsigned mbufs,networkInputs;
static bool failMbufAllocation,failMbufCopyback;
static std::vector<uint8_t> networkInput;
static unsigned networkInputFlags;
static int mbuf_allocpacket(int,size_t n,unsigned *chunks,mbuf_t *out) {
    assert(chunks && *chunks==1);if(failMbufAllocation)return ENOBUFS;
    *out=new FakeMbuf;(*out)->bytes.resize(n);(*out)->length=n;++mbufs;return 0;
}
static size_t mbuf_pkthdr_len(mbuf_t m) {return m->length;}
static int mbuf_copydata(mbuf_t m,size_t at,size_t n,void *out) {
    if(m->copyFailure || at>m->length || n>m->length-at)return EINVAL;
    memcpy(out,m->bytes.data()+m->offset+at,n);return 0;
}
static int mbuf_copyback(mbuf_t m,size_t at,size_t n,const void *in,int) {
    if(failMbufCopyback || at>m->length || n>m->length-at)return EINVAL;
    memcpy(m->bytes.data()+m->offset+at,in,n);return 0;
}
static void mbuf_freem(mbuf_t m) {assert(mbufs);--mbufs;delete m;}
static int mbuf_pkthdr_setrcvif(mbuf_t m,ifnet_t i) {m->received=i;return 0;}
static void *mbuf_data(mbuf_t m) {return m->bytes.data()+m->offset;}
static void mbuf_pkthdr_setheader(mbuf_t m,void *header) {m->header=header;}
static int mbuf_setflags_mask(mbuf_t m,unsigned flags,unsigned mask) {m->flags=(m->flags&~mask)|(flags&mask);return 0;}
static void mbuf_adj(mbuf_t m,int n) {assert(n>=0 && (size_t)n<=m->length);m->offset+=(unsigned)n;m->length-=(unsigned)n;}
static int ifnet_input(ifnet_t i,mbuf_t m,void *) {
    auto *r=(giulia_wifi_runtime *)ifnet_softc(i);
    assert(!r->lock->locked && m->received==i && m->header==m->bytes.data() && m->offset==14);
    networkInput=m->bytes;networkInputFlags=m->flags;++networkInputs;mbuf_freem(m);return 0;
}
static void read_random(void *p,unsigned n) {memset(p,0x51,n);}
static uint32_t gc_read(void *,uint32_t) {return 0;}
static void gc_write(void *,uint32_t,uint32_t) {}
static int gm_ready(void *,uint64_t,size_t) {return 1;}
static int gw_send(void *,unsigned,const void *,size_t) {return 1;}
static int (*networkPumpHook)(void *,unsigned);
static int gw_pump(void *context,unsigned us) {return networkPumpHook?networkPumpHook(context,us):1;}
static int gs_idle(void *) {return 1;}
