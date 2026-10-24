/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Independently implemented giulia ADSP loader and PAS lifecycle.
 * ABI/sequence evidence: OnePlusOSS/android_kernel_oneplus_sm8650 commit
 * d763ed47deec55451366c1187f813f2f309d88a7:
 * drivers/remoteproc/qcom_q6v5_pas.c, drivers/soc/qcom/mdt_loader.c,
 * drivers/firmware/qcom_scm.c. No Linux implementation is incorporated.
 *
 * The platform must own both reserved carveouts, metadata mappings, clocks,
 * interconnect votes and ready/fatal notification. A DT address, terminal
 * argument or successful virtual mapping does not establish ownership.
 */
#ifndef GIULIA_AUDIO_BOOT_H
#define GIULIA_AUDIO_BOOT_H
#include <stdint.h>
#include <stddef.h>

enum { GAB_OK, GAB_ARGUMENT, GAB_PACK, GAB_PIN, GAB_PREREQUISITE,
    GAB_METADATA, GAB_PREPARE, GAB_PAS, GAB_WRITE, GAB_SYNC, GAB_READY,
    GAB_QUARANTINED, GAB_ALREADY_RUNNING, GAB_NOT_RUNNING, GAB_RELEASE };
enum { GAB_PHASE_NONE, GAB_PHASE_VALIDATE, GAB_PHASE_METADATA,
    GAB_PHASE_PREPARE, GAB_PHASE_INIT, GAB_PHASE_MEM_SETUP, GAB_PHASE_LOAD,
    GAB_PHASE_AUTH, GAB_PHASE_READY, GAB_PHASE_RUNNING, GAB_PHASE_SHUTDOWN,
    GAB_PHASE_RELEASE_METADATA, GAB_PHASE_UNPREPARE };
enum { GAB_PAS_INIT=1, GAB_PAS_MEM_SETUP=2, GAB_PAS_AUTH=5, GAB_PAS_SHUTDOWN=6 };
enum { GAB_IMAGES=2, GAB_MAX_SEGMENTS=100, GAB_PAGE=4096,
    GAB_MAX_PACK_BYTES=128*1024*1024, GAB_READY_TIMEOUT_MS=5000 };
#define GAB_DTB_PHYSICAL UINT64_C(0x9de80000)
#define GAB_DTB_BYTES UINT32_C(0x80000)
#define GAB_ADSP_PHYSICAL UINT64_C(0x9df00000)
#define GAB_ADSP_BYTES UINT32_C(0x5280000)

struct gab_image {
    uint32_t pas_id,base,bytes,metadata_offset,metadata_bytes;
    uint32_t first_segment,segment_count,load_bytes,entry,relocatable;
};
struct gab_segment {
    uint32_t image,index,offset,bytes,data_offset,file_bytes,flags;
};
struct gab_plan {
    const unsigned char *pack;
    size_t pack_bytes;
    unsigned segment_count;
    struct gab_image images[GAB_IMAGES];
    struct gab_segment segments[GAB_MAX_SEGMENTS];
};
struct gab_state;
struct gab_ops {
    void *cookie;
    /* Read-only, exact compiled SHA256 pin comparison; return 1 on match. */
    int (*verify_pack)(void *,const void *,size_t);
    /* Read-only, authoritative reservation/exclusive ownership/backend check.
     * Return exactly 1. In particular a previously running DSP cannot be
     * adopted simply because its ready bit is set. */
    int (*prerequisites)(void *);
    /* Allocate page-isolated, physically contiguous, non-cacheable metadata,
     * copy the provided blob and complete required cache/barrier work.
     * Failure must leave no allocation for this image. Return 0 on success. */
    int (*metadata_alloc)(void *,unsigned,const void *,size_t,uint64_t *);
    /* Return 0 only when this allocation is actually released. Failure must
     * retain the allocation/owner and leave it available for diagnosis. */
    int (*metadata_release)(void *,unsigned);
    /* Establish IRQ baseline and hold power, XO, AOP load state and bandwidth.
     * All prerequisites must remain held through stop or quarantine. */
    int (*prepare)(void *);
    /* NULL source means zero exactly bytes (BSS). No caller-chosen address. */
    int (*memory_write)(void *,uint64_t,const void *,size_t);
    int (*memory_sync)(void *,uint64_t,size_t);
    /* Blocking SCM completion: command is one of the four fixed PIL commands.
     * For INIT arg1 is metadata PA; SETUP has base and span; others have 0,0.
     * Both transport status and secure result must report success (0).
     * Backend owns bounded interrupted/busy handling and waitqueue support. */
    int (*pas)(void *,unsigned,unsigned,uint64_t,uint64_t);
    /* Observe a fresh ready event for this boot epoch, reject fatal/watchdog.
     * A stale SMEM bit or SCM auth success alone is not ready. */
    int (*wait_ready)(void *,unsigned);
    /* After both PAS shutdowns, drain IRQ/work and release held resources.
     * Return 0 only after completion; retain uncertain ownership on failure. */
    int (*unprepare)(void *,const struct gab_state *);
};
struct gab_state {
    unsigned phase,result,image,segment,prepared,running,quarantined;
    unsigned metadata_mask,init_attempted_mask,authenticated_mask;
    unsigned pas_calls,last_pas_command,last_pas_id;
    int backend_result,cleanup_result;
    unsigned cleanup_phase,cleanup_image;
    uint64_t metadata_physical[GAB_IMAGES];
    uint64_t copied_bytes,zeroed_bytes;
};

static inline uint16_t gab_u16(const unsigned char *p)
{ return (uint16_t)((uint16_t)p[0]|((uint16_t)p[1]<<8)); }
static inline uint32_t gab_u32(const unsigned char *p)
{ return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24); }
static inline int gab_zero(const unsigned char *p,size_t n)
{ while(n--)if(*p++)return 0;return 1; }
static inline int gab_range(size_t total,uint32_t offset,uint32_t bytes)
{ return offset<=total && bytes<=total-offset; }
static inline uint64_t gab_align(uint64_t n)
{ return (n+GAB_PAGE-1)&~(uint64_t)(GAB_PAGE-1); }
static inline int gab_overlap(uint64_t a,uint64_t an,uint64_t b,uint64_t bn)
{ return a<b+bn && b<a+an; }

/* Validate the complete package before allocating memory or calling hardware.
 * Header/ELF duplication is checked deliberately: packed destinations can
 * never override the authenticated ELF's load locations or BSS dimensions.
 * The caller keeps the pack immutable for the entire synchronous operation.
 * plan contents are usable only if this function returns GAB_OK. */
static inline int gab_validate(const void *data,size_t bytes,struct gab_plan *plan)
{
    static const unsigned char magic[8]={'G','A','F','W','P','K','1',0};
    const unsigned char *p=(const unsigned char *)data;
    uint32_t segment_count,next_segment=0;
    uint64_t cursor=GAB_PAGE;
    unsigned i,j;
    if(!p || !plan || bytes<GAB_PAGE || bytes>GAB_MAX_PACK_BYTES || bytes%GAB_PAGE)
        return GAB_PACK;
    for(i=0;i<8;++i)if(p[i]!=magic[i])return GAB_PACK;
    segment_count=gab_u32(p+16);
    if(gab_u32(p+8)!=bytes || gab_u32(p+12)!=2 ||
       segment_count<2 || segment_count>GAB_MAX_SEGMENTS ||
       gab_u32(p+20)!=1 || gab_u32(p+24)!=64 || gab_u32(p+28)!=192 ||
       !gab_zero(p+32,32) || !gab_zero(p+192+segment_count*32,GAB_PAGE-192-segment_count*32))
        return GAB_PACK;
    plan->pack=p;plan->pack_bytes=bytes;plan->segment_count=segment_count;
    for(i=0;i<GAB_IMAGES;++i) {
        struct gab_image *im=&plan->images[i];
        const unsigned char *r=p+64+i*64,*m;
        uint32_t phnum,header_bytes,hash_bytes=0,hashes=0,load_count=0;
        uint64_t lowest=UINT64_MAX,highest=0;
        im->pas_id=gab_u32(r);im->base=gab_u32(r+4);im->bytes=gab_u32(r+8);
        im->metadata_offset=gab_u32(r+12);im->metadata_bytes=gab_u32(r+16);
        im->first_segment=gab_u32(r+20);im->segment_count=gab_u32(r+24);
        im->load_bytes=gab_u32(r+28);im->entry=gab_u32(r+32);
        im->relocatable=gab_u32(r+36);
        if(im->pas_id!=(i?1u:36u) || im->base!=(i?GAB_ADSP_PHYSICAL:GAB_DTB_PHYSICAL) ||
           im->bytes!=(i?GAB_ADSP_BYTES:GAB_DTB_BYTES) || !gab_zero(r+40,24) ||
           im->relocatable!=1 || !im->segment_count || im->first_segment!=next_segment ||
           im->segment_count>segment_count-next_segment ||
           im->metadata_offset!=cursor || im->metadata_bytes<52 ||
           !gab_range(bytes,im->metadata_offset,im->metadata_bytes))return GAB_PACK;
        m=p+im->metadata_offset;
        if(gab_u32(m)!=UINT32_C(0x464c457f) || m[4]!=1 || m[5]!=1 || m[6]!=1 ||
           gab_u16(m+16)!=2 || gab_u16(m+18)!=(i?164:1) || gab_u32(m+20)!=1 ||
           gab_u32(m+24)!=im->entry || gab_u32(m+28)!=52 ||
           gab_u16(m+40)!=52 || gab_u16(m+42)!=32)return GAB_PACK;
        phnum=gab_u16(m+44);header_bytes=52+phnum*32;
        if(phnum<3 || phnum>GAB_MAX_SEGMENTS || header_bytes>im->metadata_bytes)
            return GAB_PACK;
        if(gab_u32(m+52)!=0 || gab_u32(m+56)!=0 ||
           gab_u32(m+68)!=header_bytes || gab_u32(m+72)!=0)return GAB_PACK;
        for(j=1;j<phnum;++j) {
            const unsigned char *ph=m+52+j*32;
            uint32_t kind=gab_u32(ph),pa=gab_u32(ph+12),file=gab_u32(ph+16);
            uint32_t mem=gab_u32(ph+20),flags=gab_u32(ph+24),align=gab_u32(ph+28);
            uint64_t end=(uint64_t)pa+mem;
            if((flags&UINT32_C(0x07000000))==UINT32_C(0x02000000)) {
                if(kind || !file || mem!=file)return GAB_PACK;
                ++hashes;hash_bytes=file;continue;
            }
            if(kind!=1 || !mem || file>mem || !(flags&UINT32_C(0x08000000)) ||
               end>UINT64_C(0x100000000) || (align && (align&(align-1))))return GAB_PACK;
            if(pa<lowest)lowest=pa;
            if(end>highest)highest=end;
            ++load_count;
        }
        if(hashes!=1 || (uint64_t)header_bytes+hash_bytes!=im->metadata_bytes ||
           load_count!=im->segment_count || (lowest&(GAB_PAGE-1)) ||
           lowest<im->base || highest>(uint64_t)im->base+im->bytes ||
           im->entry<lowest || im->entry>=highest ||
           gab_align(highest)-lowest!=im->load_bytes || im->load_bytes>im->bytes)
            return GAB_PACK;
        cursor=gab_align(cursor+im->metadata_bytes);
        if(cursor>bytes || !gab_zero(p+im->metadata_offset+im->metadata_bytes,
           (size_t)(cursor-im->metadata_offset-im->metadata_bytes)))return GAB_PACK;
        for(j=1;j<phnum;++j) {
            const unsigned char *ph=m+52+j*32,*sr;
            struct gab_segment *seg;
            uint32_t flags=gab_u32(ph+24),k;
            if((flags&UINT32_C(0x07000000))==UINT32_C(0x02000000))continue;
            sr=p+192+next_segment*32;seg=&plan->segments[next_segment];
            seg->image=gab_u32(sr);seg->index=gab_u32(sr+4);seg->offset=gab_u32(sr+8);
            seg->bytes=gab_u32(sr+12);seg->data_offset=gab_u32(sr+16);
            seg->file_bytes=gab_u32(sr+20);seg->flags=gab_u32(sr+24);
            if(seg->image!=i || seg->index!=j || seg->offset!=(uint64_t)gab_u32(ph+12)-lowest ||
               seg->bytes!=gab_u32(ph+20) || seg->file_bytes!=gab_u32(ph+16) ||
               seg->flags!=flags || gab_u32(sr+28) ||
               seg->offset>im->bytes || seg->bytes>im->bytes-seg->offset)return GAB_PACK;
            for(k=im->first_segment;k<next_segment;++k) {
                const struct gab_segment *old=&plan->segments[k];
                if(gab_overlap(seg->offset,seg->bytes,old->offset,old->bytes))return GAB_PACK;
            }
            if(seg->file_bytes) {
                uint64_t end;
                if(seg->data_offset!=cursor || !gab_range(bytes,seg->data_offset,seg->file_bytes))
                    return GAB_PACK;
                end=cursor+seg->file_bytes;cursor=gab_align(end);
                if(cursor>bytes || !gab_zero(p+(size_t)end,(size_t)(cursor-end)))return GAB_PACK;
            } else if(seg->data_offset)return GAB_PACK;
            ++next_segment;
        }
    }
    return next_segment==segment_count && cursor==bytes?GAB_OK:GAB_PACK;
}

static inline int gab_ops_valid(const struct gab_ops *o)
{
    return o && o->verify_pack && o->prerequisites && o->metadata_alloc &&
        o->metadata_release && o->prepare && o->memory_write && o->memory_sync &&
        o->pas && o->wait_ready && o->unprepare;
}
static inline int gab_quarantine(struct gab_state *s,unsigned result,int backend)
{
    if(s->quarantined)return (int)s->result;
    s->quarantined=1;s->backend_result=backend;return (int)(s->result=result);
}
static inline int gab_release_metadata(const struct gab_ops *o,struct gab_state *s)
{
    unsigned i;
    if(s->cleanup_result)return s->cleanup_result;
    for(i=GAB_IMAGES;i--;)if(s->metadata_mask&(1u<<i)) {
        int ret=o->metadata_release(o->cookie,i);
        if(ret) {
            if(!s->cleanup_result) {
                s->cleanup_result=ret;s->cleanup_phase=GAB_PHASE_RELEASE_METADATA;s->cleanup_image=i;
            }
        } else {
            s->metadata_mask&=~(1u<<i);s->metadata_physical[i]=0;
        }
    }
    return s->cleanup_result;
}
/* Preserve the allocation/validation failure separately from rollback. Both
 * metadata allocations are independent once no PAS call has been attempted. */
static inline int gab_metadata_failure(const struct gab_ops *o,struct gab_state *s,
    unsigned result,int backend)
{
    if(gab_release_metadata(o,s))return gab_quarantine(s,result,backend);
    s->backend_result=backend;return (int)(s->result=result);
}
static inline int gab_pas(const struct gab_ops *o,struct gab_state *s,
    unsigned command,unsigned id,uint64_t a,uint64_t b)
{
    s->last_pas_command=command;s->last_pas_id=id;++s->pas_calls;
    s->backend_result=o->pas(o->cookie,command,id,a,b);
    return s->backend_result;
}

/* Zero-initialize state only once per verified cold-boot ownership epoch.
 * All metadata stays resident until both shutdown acknowledgements succeed.
 * Unknown secure/ready outcomes require external reset/reboot reconciliation;
 * neither another terminal command nor an elapsed timeout clears quarantine. */
static inline int gab_start(const struct gab_ops *o,struct gab_state *s,
    const void *pack,size_t bytes)
{
    struct gab_plan plan;
    unsigned i,j;
    int ret;
    if(!s)return GAB_ARGUMENT;
    if(s->quarantined)return GAB_QUARANTINED;
    if(!gab_ops_valid(o))return (int)(s->result=GAB_ARGUMENT);
    if(s->running)return (int)(s->result=GAB_ALREADY_RUNNING);
    if(s->prepared || s->metadata_mask || s->init_attempted_mask || s->authenticated_mask ||
       s->metadata_physical[0] || s->metadata_physical[1] || s->cleanup_result)
        return gab_quarantine(s,GAB_QUARANTINED,0);
    s->backend_result=0;
    s->phase=GAB_PHASE_VALIDATE;
    if(gab_validate(pack,bytes,&plan))return (int)(s->result=GAB_PACK);
    if(o->verify_pack(o->cookie,pack,bytes)!=1)return (int)(s->result=GAB_PIN);
    if(o->prerequisites(o->cookie)!=1)return (int)(s->result=GAB_PREREQUISITE);
    s->phase=GAB_PHASE_METADATA;
    for(i=0;i<GAB_IMAGES;++i) {
        const struct gab_image *im=&plan.images[i];
        uint64_t pa=0,n=gab_align(im->metadata_bytes);
        s->image=i;
        ret=o->metadata_alloc(o->cookie,i,plan.pack+im->metadata_offset,im->metadata_bytes,&pa);
        if(ret)return gab_metadata_failure(o,s,GAB_METADATA,ret);
        s->metadata_mask|=1u<<i;s->metadata_physical[i]=pa;
        if(!pa || (pa&(GAB_PAGE-1)) || pa>UINT64_MAX-n ||
           gab_overlap(pa,n,GAB_DTB_PHYSICAL,GAB_DTB_BYTES) ||
           gab_overlap(pa,n,GAB_ADSP_PHYSICAL,GAB_ADSP_BYTES) ||
           (i && gab_overlap(pa,n,s->metadata_physical[0],gab_align(plan.images[0].metadata_bytes)))) {
            return gab_metadata_failure(o,s,GAB_METADATA,0);
        }
    }
    /* Recheck before the first hardware effect, after potentially blocking allocations. */
    if(o->prerequisites(o->cookie)!=1) {
        return gab_metadata_failure(o,s,GAB_PREREQUISITE,0);
    }
    s->phase=GAB_PHASE_PREPARE;s->prepared=1;
    ret=o->prepare(o->cookie);
    if(ret)return gab_quarantine(s,GAB_PREPARE,ret);
    for(i=0;i<GAB_IMAGES;++i) {
        const struct gab_image *im=&plan.images[i];
        s->image=i;s->phase=GAB_PHASE_INIT;s->init_attempted_mask|=1u<<i;
        ret=gab_pas(o,s,GAB_PAS_INIT,im->pas_id,s->metadata_physical[i],0);
        if(ret)return gab_quarantine(s,GAB_PAS,ret);
        s->phase=GAB_PHASE_MEM_SETUP;
        ret=gab_pas(o,s,GAB_PAS_MEM_SETUP,im->pas_id,im->base,im->load_bytes);
        if(ret)return gab_quarantine(s,GAB_PAS,ret);
        s->phase=GAB_PHASE_LOAD;
        for(j=im->first_segment;j<im->first_segment+im->segment_count;++j) {
            const struct gab_segment *seg=&plan.segments[j];
            uint64_t pa=(uint64_t)im->base+seg->offset;
            s->segment=seg->index;
            if(seg->file_bytes) {
                ret=o->memory_write(o->cookie,pa,plan.pack+seg->data_offset,seg->file_bytes);
                if(ret)return gab_quarantine(s,GAB_WRITE,ret);
                s->copied_bytes+=seg->file_bytes;
            }
            if(seg->bytes>seg->file_bytes) {
                ret=o->memory_write(o->cookie,pa+seg->file_bytes,NULL,seg->bytes-seg->file_bytes);
                if(ret)return gab_quarantine(s,GAB_WRITE,ret);
                s->zeroed_bytes+=seg->bytes-seg->file_bytes;
            }
            ret=o->memory_sync(o->cookie,pa,seg->bytes);
            if(ret)return gab_quarantine(s,GAB_SYNC,ret);
        }
        s->phase=GAB_PHASE_AUTH;
        ret=gab_pas(o,s,GAB_PAS_AUTH,im->pas_id,0,0);
        if(ret)return gab_quarantine(s,GAB_PAS,ret);
        s->authenticated_mask|=1u<<i;
    }
    s->phase=GAB_PHASE_READY;
    ret=o->wait_ready(o->cookie,GAB_READY_TIMEOUT_MS);
    if(ret)return gab_quarantine(s,GAB_READY,ret);
    s->running=1;s->phase=GAB_PHASE_RUNNING;
    return (int)(s->result=GAB_OK);
}

/* Call only after the audio/IPC owner has quiesced all clients. No force-stop
 * of an uncertain/in-flight secure operation is attempted. */
static inline int gab_stop(const struct gab_ops *o,struct gab_state *s)
{
    unsigned i;
    int ret;
    if(!s)return GAB_ARGUMENT;
    if(s->quarantined)return GAB_QUARANTINED;
    if(!gab_ops_valid(o))return (int)(s->result=GAB_ARGUMENT);
    if(!s->running)return (int)(s->result=GAB_NOT_RUNNING);
    s->phase=GAB_PHASE_SHUTDOWN;
    for(i=GAB_IMAGES;i--;) {
        s->image=i;
        ret=gab_pas(o,s,GAB_PAS_SHUTDOWN,i?1:36,0,0);
        if(ret)return gab_quarantine(s,GAB_PAS,ret);
        s->authenticated_mask&=~(1u<<i);s->init_attempted_mask&=~(1u<<i);
    }
    s->running=0;s->phase=GAB_PHASE_RELEASE_METADATA;
    ret=gab_release_metadata(o,s);
    if(ret)return gab_quarantine(s,GAB_RELEASE,ret);
    s->phase=GAB_PHASE_UNPREPARE;
    ret=o->unprepare(o->cookie,s);
    if(ret) {
        s->cleanup_result=ret;s->cleanup_phase=GAB_PHASE_UNPREPARE;s->cleanup_image=GAB_IMAGES;
        return gab_quarantine(s,GAB_RELEASE,ret);
    }
    s->prepared=0;
    s->phase=GAB_PHASE_NONE;
    return (int)(s->result=GAB_OK);
}
#endif
