/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Pointer-free QMI/WMI TLV shape validation. Values, secrets and resource
 * addresses stay resident. The same parser is used by the native module and
 * resident fallback; no decision here performs I/O or changes protocol state.
 */
#ifndef GIULIA_WIFI_PROTOCOL_H
#define GIULIA_WIFI_PROTOCOL_H
#include <stdint.h>
#include <stddef.h>
#define GWPR_ABI 1u
#define GWPR_DOMAIN 2u
#define GWPR_INTERFACE_ABI 0x47480001u
#define GWPR_MAX_WORDS 32u
#define GWPR_META_WORDS 3u
#define GWPR_MAX_SHAPES (GWPR_MAX_WORDS-GWPR_META_WORDS)
#define GWPR_QMI 1u
#define GWPR_WMI 2u
#define GWPR_MAX_QMI_BYTES 8192u
#define GWPR_MAX_WMI_BYTES 2036u
enum gwpr_status {GWPR_VALID,GWPR_ARGUMENT,GWPR_FRAMING,GWPR_DUPLICATE,
    GWPR_WIDTH,GWPR_BYPASS,GWPR_READ};
typedef uint64_t (*gwpr_select_fn)(void *,unsigned,unsigned,unsigned,const void *,size_t);
static inline uint64_t gwpr_result(unsigned status,unsigned count,unsigned offset,unsigned present)
{return status|((uint64_t)count<<8)|((uint64_t)offset<<24)|((uint64_t)present<<40);}
static inline unsigned gwpr_status(uint64_t result){return (unsigned)(result&255u);}
static inline unsigned gwpr_wmi_known(unsigned message) {
    return message==1 || message==2 || message==3 || message==0x4009 ||
        message==0x4022 || message==0x3001 || message==0x7001;
}
static inline int gwpr_metadata(unsigned operation,unsigned message,unsigned flag,size_t bytes) {
    if(operation==GWPR_QMI)return bytes<=GWPR_MAX_QMI_BYTES && message<=0xffffu &&
        (flag==2 || flag==4 || (!flag && !message));
    return operation==GWPR_WMI && bytes<=GWPR_MAX_WMI_BYTES && !flag && message<=0xffffffu;
}
static inline unsigned gwpr_byte(const void *data,size_t offset)
{return ((const volatile uint8_t *)data)[offset];}
static inline uint64_t gwpr_shape(unsigned tag,unsigned bytes,unsigned offset,unsigned header)
{return tag|((uint64_t)bytes<<16)|((uint64_t)offset<<32)|((uint64_t)header<<48);}
/* Extraction only: preserve an incomplete header or oversized value as a
 * descriptor. The parser, not this bounded copy, decides protocol validity.
 * Zero means the complete shape stream cannot fit; use the full fallback. */
static inline unsigned gwpr_request(uint64_t request[GWPR_MAX_WORDS],unsigned operation,
    unsigned message,unsigned flag,const void *data,size_t bytes) {
    if(!request || (bytes&&!data) || !gwpr_metadata(operation,message,flag,bytes))return 0;
    if(operation==GWPR_WMI && !gwpr_wmi_known(message))return 0;
    unsigned header=operation==GWPR_QMI?3u:4u,count=0;size_t offset=0;
    request[0]=GWPR_ABI|((uint64_t)operation<<32);request[1]=message|((uint64_t)flag<<32);
    while(offset<bytes) {
        if(count==GWPR_MAX_SHAPES)return 0;
        unsigned available=(unsigned)(bytes-offset);if(available>header)available=header;
        unsigned tag=0,length=0;
        if(available==header) {
            if(operation==GWPR_QMI){tag=gwpr_byte(data,offset);length=gwpr_byte(data,offset+1)|(gwpr_byte(data,offset+2)<<8);}
            else {length=gwpr_byte(data,offset)|(gwpr_byte(data,offset+1)<<8);tag=gwpr_byte(data,offset+2)|(gwpr_byte(data,offset+3)<<8);}
        }
        request[GWPR_META_WORDS+count++]=gwpr_shape(tag,length,(unsigned)offset,available);
        if(available!=header || length>bytes-offset-header)break;
        offset+=header+length;
    }
    request[2]=(uint32_t)bytes|((uint64_t)count<<32);return GWPR_META_WORDS+count;
}
struct gwpr_cursor {unsigned operation,message,flag,bytes,offset,count,present,status;uint8_t seen[32];};
static inline void gwpr_begin(struct gwpr_cursor *s,unsigned operation,unsigned message,unsigned flag,unsigned bytes) {
    s->operation=operation;s->message=message;s->flag=flag;s->bytes=bytes;s->offset=s->count=s->present=0;
    s->status=gwpr_metadata(operation,message,flag,bytes)?GWPR_VALID:GWPR_ARGUMENT;
    if(s->status==GWPR_VALID && operation==GWPR_WMI && !gwpr_wmi_known(message))s->status=GWPR_BYPASS;
    for(unsigned i=0;i<32;i++)s->seen[i]=0;
}
static inline void gwpr_consume(struct gwpr_cursor *s,uint64_t shape) {
    if(s->status!=GWPR_VALID)return;
    unsigned tag=(unsigned)(shape&0xffffu),bytes=(unsigned)((shape>>16)&0xffffu);
    unsigned offset=(unsigned)((shape>>32)&0xffffu),header=(unsigned)((shape>>48)&255u);
    unsigned expected=s->operation==GWPR_QMI?3u:4u;
    if((shape>>56) || offset!=s->offset || offset>s->bytes || header!=expected ||
        header>s->bytes-offset || bytes>s->bytes-offset-header || (s->operation==GWPR_QMI && tag>255)) {
        s->status=GWPR_FRAMING;return;
    }
    if(s->operation==GWPR_QMI) {
        if(s->seen[tag/8]&(1u<<(tag%8))){s->status=GWPR_DUPLICATE;return;}
        s->seen[tag/8]|=(uint8_t)(1u<<(tag%8));
        if(s->flag==4 && (s->message==0x38 || s->message==0x2c) && tag>=0x10 && tag<=0x12) {
            unsigned wanted=4;
            if(s->message==0x38 && tag==0x11)wanted=2;
            if(s->message==0x38 && tag==0x12)wanted=8;
            if(bytes!=wanted){s->status=GWPR_WIDTH;return;}
            s->present|=1u<<(tag-0x10);
        }
    }
    s->offset+=header+bytes;s->count++;
}
static inline uint64_t gwpr_finish(struct gwpr_cursor *s) {
    if(s->status==GWPR_VALID && s->offset!=s->bytes)s->status=GWPR_FRAMING;
    return gwpr_result(s->status,s->count,
        s->status==GWPR_VALID || s->status==GWPR_BYPASS?0xffffu:s->offset,s->present);
}
static inline uint64_t gwpr_parse(const uint64_t *request,unsigned words) {
    if(!request || words<GWPR_META_WORDS || words>GWPR_MAX_WORDS ||
        (uint32_t)request[0]!=GWPR_ABI || (uint32_t)(request[2]>>32)!=words-GWPR_META_WORDS)
        return gwpr_result(GWPR_ARGUMENT,0,0xffffu,0);
    struct gwpr_cursor s;gwpr_begin(&s,(unsigned)(request[0]>>32),(uint32_t)request[1],
        (unsigned)(request[1]>>32),(uint32_t)request[2]);
    for(unsigned i=GWPR_META_WORDS;i<words;i++)gwpr_consume(&s,request[i]);
    return gwpr_finish(&s);
}
/* The unbounded-by-mailbox fallback still walks the complete, bounded packet.
 * Packets with more than 29 headers are never truncated to fit a hot call. */
static inline uint64_t gwpr_baseline(unsigned operation,unsigned message,unsigned flag,const void *data,size_t bytes) {
    if((bytes&&!data) || bytes>UINT32_MAX)return gwpr_result(GWPR_ARGUMENT,0,0xffffu,0);
    struct gwpr_cursor s;gwpr_begin(&s,operation,message,flag,(unsigned)bytes);
    unsigned header=operation==GWPR_QMI?3u:4u;
    while(s.status==GWPR_VALID && s.offset<s.bytes) {
        unsigned offset=s.offset,available=s.bytes-offset;if(available>header)available=header;
        unsigned tag=0,length=0;
        if(available==header) {
            if(operation==GWPR_QMI){tag=gwpr_byte(data,offset);length=gwpr_byte(data,offset+1)|(gwpr_byte(data,offset+2)<<8);}
            else {length=gwpr_byte(data,offset)|(gwpr_byte(data,offset+1)<<8);tag=gwpr_byte(data,offset+2)|(gwpr_byte(data,offset+3)<<8);}
        }
        gwpr_consume(&s,gwpr_shape(tag,length,offset,available));
    }
    return gwpr_finish(&s);
}
#endif
