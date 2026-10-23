#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <openssl/evp.h>
#include <openssl/core_names.h>
typedef struct {uint8_t key[16];} aes_encrypt_ctx;
static int aes_encrypt_key(const uint8_t *key,int bytes,aes_encrypt_ctx *ctx){if(bytes!=16)return -1;memcpy(ctx->key,key,16);return 0;}
static int aes_encrypt_cbc(const uint8_t *in,const uint8_t iv[16],unsigned blocks,uint8_t *out,aes_encrypt_ctx *ctx){
    EVP_CIPHER_CTX *evp=EVP_CIPHER_CTX_new();assert(evp);int n=0,last=0;
    int ok=EVP_EncryptInit_ex(evp,EVP_aes_128_cbc(),NULL,ctx->key,iv)&&EVP_CIPHER_CTX_set_padding(evp,0)&&
        EVP_EncryptUpdate(evp,out,&n,in,(int)blocks*16)&&EVP_EncryptFinal_ex(evp,out+n,&last);EVP_CIPHER_CTX_free(evp);return ok?0:-1;
}
#define GPM_CRYPTO_HOST 1
#include "../ios16/giulia_wifi_pmf_crypto.h"
#include "../ios16/giulia_wifi_pmf.h"
static uint64_t clock_ms;
static uint64_t now(void *ctx){(void)ctx;return clock_ms;}
static int random_bytes(void *ctx,uint8_t *out,size_t n){(void)ctx;for(size_t i=0;i<n;i++)out[i]=(uint8_t)(0x31+i);return 0;}
static void hex(const char *s,uint8_t *p,size_t n){for(size_t i=0;i<n;i++){unsigned x=0;assert(sscanf(s+2*i,"%2x",&x)==1);p[i]=(uint8_t)x;}}
static void ref_cmac(const uint8_t key[16],const uint8_t *p,size_t n,uint8_t out[16]){
    EVP_MAC *mac=EVP_MAC_fetch(NULL,"CMAC",NULL);assert(mac);EVP_MAC_CTX *ctx=EVP_MAC_CTX_new(mac);assert(ctx);
    char cipher[]="AES-128-CBC";OSSL_PARAM params[]={OSSL_PARAM_construct_utf8_string(OSSL_MAC_PARAM_CIPHER,cipher,0),OSSL_PARAM_construct_end()};size_t bytes=0;
    assert(EVP_MAC_init(ctx,key,16,params) && EVP_MAC_update(ctx,p,n) && EVP_MAC_final(ctx,out,&bytes,16) && bytes==16);
    EVP_MAC_CTX_free(ctx);EVP_MAC_free(mac);
}
static const uint8_t local[6]={2,1,2,3,4,5},ap[6]={2,9,8,7,6,5};
static void init(struct gpm_state *s){struct gpm_ops ops={NULL,gpm_crypto_cmac,random_bytes,now};clock_ms=10;assert(gpm_init(s,&ops,local,ap)==GPM_OK);}
static void header(uint8_t *p,unsigned type,unsigned group){memset(p,0,128);p[0]=(uint8_t)type;memcpy(p+4,local,6);if(group)memset(p+4,0xff,6);memcpy(p+10,ap,6);memcpy(p+16,ap,6);}
static size_t bip(uint8_t frame[128],const uint8_t key[16],uint64_t ipn){
    header(frame,0xc0,1);frame[24]=8;frame[26]=76;frame[27]=16;frame[28]=4;
    for(unsigned i=0;i<6;i++)frame[30+i]=(uint8_t)(ipn>>(8*i));
    uint8_t aad[40]={0},mic[16];aad[0]=frame[0];aad[1]=frame[1]&0xc7;memcpy(aad+2,frame+4,18);memcpy(aad+20,frame+24,20);
    ref_cmac(key,aad,sizeof(aad),mic);memcpy(frame+36,mic,8);return 44;
}
static size_t unicast(uint8_t frame[128],unsigned type,uint64_t pn,unsigned action,const uint8_t transaction[2]){
    header(frame,type,0);frame[1]=0x40;frame[24]=(uint8_t)pn;frame[25]=(uint8_t)(pn>>8);frame[27]=0x20;
    for(unsigned i=0;i<4;i++)frame[28+i]=(uint8_t)(pn>>(16+8*i));
    if(type==0xd0){frame[32]=8;frame[33]=(uint8_t)action;memcpy(frame+34,transaction,2);return 44;}
    frame[32]=8;return 42;
}
int main(void){
    uint8_t key[16],message[64],actual[16],expected[16];hex("2b7e151628aed2a6abf7158809cf4f3c",key,16);
    hex("6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e5130c81c46a35ce411e5fbc1191a0a52eff69f2445df4f9b17ad2b417be66c3710",message,64);
    const unsigned sizes[]={0,16,40,64};const char *tags[]={"bb1d6929e95937287fa37d129b756746b","070a16b46b4d4144f79bdd9dd04a287c","dfa66747de9ae63030ca32611497c827","51f0bebf7e3b9d92fc49741779363cfe"};
    for(unsigned i=0;i<4;i++){hex(tags[i],expected,16);assert(!gpm_crypto_cmac(NULL,key,message,sizes[i],actual));assert(!memcmp(actual,expected,16));ref_cmac(key,message,sizes[i],actual);assert(!memcmp(actual,expected,16));}
    struct gpm_state s;init(&s);assert(!gpm_ready(&s));assert(gpm_install_pairwise(&s,key)==GPM_OK);assert(!gpm_ready(&s));
    assert(gpm_install_igtk(&s,4,1,key)==GPM_OK && gpm_ready(&s));uint8_t frame[128],out[128];size_t n=bip(frame,key,2),used=sizeof(out);
    frame[36]^=1;assert(gpm_filter(&s,frame,n,0,out,&used)==GPM_DROP && s.ipn[0]==1);frame[36]^=1;used=sizeof(out);
    assert(gpm_filter(&s,frame,n,0,out,&used)==GPM_OK && used==26 && out[24]==8 && s.ipn[0]==2);
    used=sizeof(out);assert(gpm_filter(&s,frame,n,0,out,&used)==GPM_DROP);assert(gpm_install_igtk(&s,4,0,key)==GPM_OK && s.ipn[0]==2);
    assert(gpm_install_igtk(&s,5,0,key)==GPM_KEY && s.igtk_valid==1);
    n=unicast(frame,0xc0,5,0,NULL);used=sizeof(out);assert(gpm_filter(&s,frame,n,8,out,&used)==GPM_DROP && !s.rx_pn);
    used=sizeof(out);assert(gpm_filter(&s,frame,n,0,out,&used)==GPM_OK && used==26 && !(out[1]&0x40) && s.rx_pn==5);
    used=sizeof(out);assert(gpm_filter(&s,frame,n,0,out,&used)==GPM_DROP);assert(gpm_install_pairwise(&s,key)==GPM_OK && s.rx_pn==5);
    header(frame,0xc0,0);frame[24]=7;used=sizeof(out);assert(gpm_filter(&s,frame,26,0,out,&used)==GPM_DROP && s.query_active);
    used=sizeof(out);assert(gpm_next_tx(&s,out,&used)==GPM_OK && used==28 && out[24]==8 && !out[25]);
    uint8_t prepared[128];size_t prepared_bytes=sizeof(prepared);assert(gpm_protect(&s,out,used,prepared,&prepared_bytes)==GPM_OK);
    assert(prepared_bytes==44 && (prepared[1]&0x40) && prepared[24]==1 && prepared[27]==0x20 && prepared[32]==8 && !prepared[33]);
    for(unsigned i=36;i<44;i++)assert(!prepared[i]);
    uint8_t transaction[2]={0,0};n=unicast(frame,0xd0,6,1,transaction);used=sizeof(out);assert(gpm_filter(&s,frame,n,0,out,&used)==GPM_DROP && s.query_active);
    memcpy(transaction,s.transaction,2);n=unicast(frame,0xd0,7,1,transaction);used=sizeof(out);assert(gpm_filter(&s,frame,n,0,out,&used)==GPM_DROP && !s.query_active);
    n=unicast(frame,0xd0,8,0,transaction);used=sizeof(out);assert(gpm_filter(&s,frame,n,0,out,&used)==GPM_DROP);
    used=sizeof(out);assert(gpm_next_tx(&s,out,&used)==GPM_OK && out[25]==1 && !memcmp(out+26,transaction,2));
    header(frame,0xa0,0);frame[24]=6;used=sizeof(out);assert(gpm_filter(&s,frame,26,0,out,&used)==GPM_DROP && s.query_active);
    clock_ms+=200;assert(gpm_tick(&s)==GPM_OK && s.pending);clock_ms+=800;assert(gpm_tick(&s)==GPM_TIMEOUT && !s.pending);
    gpm_clear(&s);for(size_t i=0;i<sizeof(s);i++)assert(!((uint8_t *)&s)[i]);
    puts("PMF: RFC4493/OpenSSL CMAC oracle, BIP MMIE tamper/IPN replay, WCN7850 decrypted CCMP PN, WMI IV/MIC layout and authenticated SA Query response/retry/timeout passed");return 0;
}
