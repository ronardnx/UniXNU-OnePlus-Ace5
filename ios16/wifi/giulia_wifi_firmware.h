/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Read-only firmware pack. Its complete SHA-256 is compiled into the driver;
 * individual payloads retain the signed vendor bytes, including ELF headers.
 */
#ifndef GIULIA_WIFI_FIRMWARE_H
#define GIULIA_WIFI_FIRMWARE_H
#include <stdint.h>
#include <stddef.h>
#include "../common/giulia_sha256.h"
struct gwf_view {const unsigned char *data;size_t bytes;unsigned count,valid;};
struct gwf_asset {const unsigned char *data,*sha256;size_t bytes;};
static inline uint32_t gwf_u32(const unsigned char *p)
{return p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);}
static inline int gwf_name(const unsigned char *p)
{
    unsigned segment=0;
    for(unsigned i=0;i<64;i++) {
        unsigned c=p[i];
        if(!c)return i && segment;
        if(c=='/') {if(!segment)return 0;segment=0;continue;}
        if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-'||c=='.'))return 0;
        if(!segment && c=='.' && (i==63 || !p[i+1] || p[i+1]=='.' || p[i+1]=='/'))return 0;
        segment++;
    }
    return 0;
}
static inline int gwf_open(struct gwf_view *v,const void *bytes,size_t n,const unsigned char expected[32])
{
    if(!v)return 0;
    v->valid=0;
    if(!bytes || !expected || n<64 || n>64*1024*1024)return 0;
    const unsigned char *p=(const unsigned char *)bytes;static const unsigned char magic[8]={'G','W','F','W','P','K','1',0};
    for(unsigned i=0;i<8;i++)if(p[i]!=magic[i])return 0;
    unsigned count=gwf_u32(p+12),start=gwf_u32(p+28);
    if(gwf_u32(p+8)!=1 || !count || count>128 || gwf_u32(p+16)!=n || gwf_u32(p+20)!=64 ||
       gwf_u32(p+24)!=112 || start<64+count*112 || (start&0x3fff) || start>n ||
       gwf_u32(p+32)!=23851 || gwf_u32(p+36)!=0x4e43 || gwf_u32(p+40)!=0x110717cb)return 0;
    unsigned char digest[32];gwh_sha256(p,n,digest);if(!gwh_equal(digest,expected))return 0;
    size_t previous=start;
    for(unsigned i=0;i<count;i++) {
        const unsigned char *e=p+64+i*112;unsigned offset=gwf_u32(e+64),size=gwf_u32(e+68);
        if(!gwf_name(e) || !size || size>16*1024*1024 || offset<previous || (offset&63) ||
           offset>n || size>n-offset)return 0;
        for(unsigned j=0;j<i;j++) {
            const unsigned char *other=p+64+j*112;unsigned k=0;
            while(k<64 && e[k]==other[k] && e[k])k++;
            if(k<64 && !e[k] && !other[k])return 0;
        }
        previous=(size_t)offset+size;
    }
    v->data=p;v->bytes=n;v->count=count;v->valid=1;return 1;
}
static inline int gwf_get(const struct gwf_view *v,const char *name,struct gwf_asset *asset)
{
    if(!v || !v->valid || !name || !asset)return 0;
    for(unsigned i=0;i<v->count;i++) {
        const unsigned char *e=v->data+64+i*112;unsigned j=0;
        while(j<64 && e[j] && name[j] && e[j]==(unsigned char)name[j])j++;
        if(j==64 || e[j] || name[j])continue;
        unsigned offset=gwf_u32(e+64),size=gwf_u32(e+68);unsigned char digest[32];
        gwh_sha256(v->data+offset,size,digest);if(!gwh_equal(digest,e+72))return 0;
        asset->data=v->data+offset;asset->bytes=size;asset->sha256=e+72;return 1;
    }
    return 0;
}
static inline int gwf_main(const struct gwf_view *v,unsigned major,struct gwf_asset *a)
{return major==1?gwf_get(v,"kiwi/amss.bin",a):(major==2?gwf_get(v,"kiwi/amss20.bin",a):0);}
static inline int gwf_phy(const struct gwf_view *v,unsigned major,struct gwf_asset *a)
{return major==1?gwf_get(v,"kiwi/phy_ucode.elf",a):(major==2?gwf_get(v,"kiwi/phy_ucode20.elf",a):0);}
static inline int gwf_board(const struct gwf_view *v,uint32_t chip,uint32_t board,struct gwf_asset *a)
{
    static const char hex[]="0123456789abcdef";
    char name[64]="kiwi/";unsigned at=5;
    const char *prefix=(chip&0x10)?"bdwlang":"bdwlan";
    if(board>0xffff)return 0;
    if(board>0xff)prefix="bdwlan";
    while(*prefix)name[at++]=*prefix++;
    if(board==0xff) {const char *s=".b0c";while(*s)name[at++]=*s++;}
    else {
        if(board>0xff){name[at++]=hex[(board>>12)&15];name[at++]=hex[(board>>8)&15];}
        name[at++]='.';name[at++]='e';name[at++]=hex[(board>>4)&15];name[at++]=hex[board&15];
    }
    name[at]=0;return gwf_get(v,name,a);
}
#endif
