/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Small SHA-256 implementation for original-firmware integrity bindings.
 */
#ifndef GIULIA_WIFI_SHA256_H
#define GIULIA_WIFI_SHA256_H
#include <stdint.h>
#include <stddef.h>
static inline uint32_t gwh_rotr(uint32_t x,unsigned n){return (x>>n)|(x<<(32-n));}
static inline void gwh_block(uint32_t state[8],const unsigned char *p)
{
    static const uint32_t k[64]={
      0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
      0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
      0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
      0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
      0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
      0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
      0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
      0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
    uint32_t w[64],a=state[0],b=state[1],c=state[2],d=state[3];
    uint32_t e=state[4],f=state[5],g=state[6],h=state[7];
    for(unsigned i=0;i<16;i++)w[i]=((uint32_t)p[i*4]<<24)|((uint32_t)p[i*4+1]<<16)|((uint32_t)p[i*4+2]<<8)|p[i*4+3];
    for(unsigned i=16;i<64;i++)w[i]=(gwh_rotr(w[i-2],17)^gwh_rotr(w[i-2],19)^(w[i-2]>>10))+w[i-7]+
      (gwh_rotr(w[i-15],7)^gwh_rotr(w[i-15],18)^(w[i-15]>>3))+w[i-16];
    for(unsigned i=0;i<64;i++) {
        uint32_t t=h+(gwh_rotr(e,6)^gwh_rotr(e,11)^gwh_rotr(e,25))+((e&f)^(~e&g))+k[i]+w[i];
        uint32_t u=(gwh_rotr(a,2)^gwh_rotr(a,13)^gwh_rotr(a,22))+((a&b)^(a&c)^(b&c));
        h=g;g=f;f=e;e=d+t;d=c;c=b;b=a;a=t+u;
    }
    state[0]+=a;state[1]+=b;state[2]+=c;state[3]+=d;state[4]+=e;state[5]+=f;state[6]+=g;state[7]+=h;
}
static inline void gwh_sha256(const void *bytes,size_t length,unsigned char digest[32])
{
    uint32_t state[8]={0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    const unsigned char *data=(const unsigned char *)bytes;unsigned char tail[128]={0};
    size_t blocks=length/64,remain=length%64;
    for(size_t i=0;i<blocks;i++)gwh_block(state,data+i*64);
    for(size_t i=0;i<remain;i++)tail[i]=data[blocks*64+i];
    tail[remain]=0x80;unsigned total=remain<56?64:128;uint64_t bits=(uint64_t)length*8;
    for(unsigned i=0;i<8;i++)tail[total-1-i]=(unsigned char)(bits>>(8*i));
    gwh_block(state,tail);if(total==128)gwh_block(state,tail+64);
    for(unsigned i=0;i<32;i++)digest[i]=(unsigned char)(state[i/4]>>(24-8*(i%4)));
}
static inline int gwh_equal(const unsigned char a[32],const unsigned char b[32])
{unsigned difference=0;for(unsigned i=0;i<32;i++)difference|=a[i]^b[i];return difference==0;}
#endif
