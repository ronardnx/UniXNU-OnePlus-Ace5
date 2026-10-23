/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * RFC 4493 AES-CMAC-128 over XNU's existing AES provider (BearSSL backend).
 */
#ifndef GIULIA_WIFI_PMF_CRYPTO_H
#define GIULIA_WIFI_PMF_CRYPTO_H
#include <stddef.h>
#include <stdint.h>
#ifndef GPM_CRYPTO_HOST
#include <libkern/crypto/aes.h>
#endif
static inline void gpm_crypto_zero(void *p,size_t n){volatile uint8_t *b=(volatile uint8_t *)p;while(n--)*b++=0;}
static inline void gpm_crypto_double(uint8_t p[16]){
    unsigned high=p[0]>>7;for(unsigned i=0;i<15;i++)p[i]=(uint8_t)((p[i]<<1)|(p[i+1]>>7));p[15]=(uint8_t)((p[15]<<1)^(high?0x87:0));
}
static inline int gpm_crypto_cmac(void *context,const uint8_t key[16],const uint8_t *p,size_t n,uint8_t out[16]){
    (void)context;if(!key || (!p && n) || !out || n>2048)return -1;
    aes_encrypt_ctx aes;uint8_t subkey[16]={0},iv[16]={0},block[16]={0},chain[16]={0};int error=-1;
    if(aes_encrypt_key(key,16,&aes) || aes_encrypt_cbc(block,iv,1,subkey,&aes))goto done;
    gpm_crypto_double(subkey);
    {size_t blocks=(n+15)/16;if(!blocks)blocks=1;
        for(size_t i=0;i<blocks;i++){
            size_t bytes=n>i*16?n-i*16:0;if(bytes>16)bytes=16;
            for(unsigned j=0;j<16;j++)block[j]=j<bytes?p[i*16+j]:0;
            if(i+1==blocks){if(bytes<16){gpm_crypto_double(subkey);block[bytes]=0x80;}
                for(unsigned j=0;j<16;j++)block[j]^=subkey[j];}
            for(unsigned j=0;j<16;j++)block[j]^=chain[j];
            if(aes_encrypt_cbc(block,iv,1,chain,&aes))goto done;
        }
    }
    for(unsigned i=0;i<16;i++)out[i]=chain[i];error=0;
done:
    if(error)gpm_crypto_zero(out,16);gpm_crypto_zero(&aes,sizeof(aes));gpm_crypto_zero(subkey,16);
    gpm_crypto_zero(iv,16);gpm_crypto_zero(block,16);gpm_crypto_zero(chain,16);return error;
}
#endif
