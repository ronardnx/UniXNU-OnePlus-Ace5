/* Offline production-header test. OpenSSL provides the XNU SHA1/AES ABI;
 * it also independently constructs AP frames and verifies station MICs. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <openssl/sha.h>
#include <openssl/aes.h>
#include <openssl/hmac.h>
#include <openssl/cmac.h>
typedef SHA_CTX SHA1_CTX;
typedef AES_KEY aes_decrypt_ctx;
static void SHA1Init(SHA1_CTX *s){assert(SHA1_Init(s));}
static void SHA1Update(SHA1_CTX *s,const void *p,size_t n){assert(SHA1_Update(s,p,n));}
static void SHA1Final(void *p,SHA1_CTX *s){assert(SHA1_Final(p,s));}
static int aes_decrypt_key(const unsigned char *p,int n,aes_decrypt_ctx *s){return AES_set_decrypt_key(p,8*n,s);}
static int aes_decrypt_cbc(const unsigned char *in,const unsigned char *iv,unsigned blocks,unsigned char *out,aes_decrypt_ctx *s){
    unsigned char copy[16];memcpy(copy,iv,16);AES_cbc_encrypt(in,out,16*blocks,s,copy,AES_DECRYPT);return 0;
}
#define GWR_CRYPTO_HOST 1
#include "../ios16/wifi/giulia_wifi_rsn_crypto.h"
#include "../ios16/wifi/giulia_wifi_rsn.h"
typedef AES_KEY aes_encrypt_ctx;
static int aes_encrypt_key(const uint8_t *p,int n,aes_encrypt_ctx *s){return AES_set_encrypt_key(p,8*n,s);}
static int aes_encrypt_cbc(const uint8_t *in,const uint8_t *iv,unsigned blocks,uint8_t *out,aes_encrypt_ctx *s){uint8_t copy[16];memcpy(copy,iv,16);AES_cbc_encrypt(in,out,16*blocks,s,copy,AES_ENCRYPT);return 0;}
#define GPM_CRYPTO_HOST 1
#include "../ios16/wifi/giulia_wifi_pmf_crypto.h"
#include "../ios16/wifi/giulia_wifi_pmf.h"
static const uint8_t station_rsn[22]={48,20,1,0,0,15,172,4,1,0,0,15,172,4,1,0,0,15,172,2,0,0};
static const uint8_t access_rsn[22]={48,20,1,0,0,15,172,4,1,0,0,15,172,4,1,0,0,15,172,2,0x80,0};
static const uint8_t local[6]={2,0,0,0,0,1},ap[6]={2,0,0,0,0,2};
struct harness {
    uint64_t now;unsigned sends,installs,ready,opened,closed,random_calls,fail_send,fail_key,fail_ready;
    unsigned last_index,last_pairwise,port_open,no_op,fail_authorize;uint64_t last_rsc;
    uint8_t last_key[16],sent[512];size_t sent_bytes;unsigned igtk_installs,fail_igtk;struct gpm_state pmf;
};
static int random32(void *v,uint8_t p[32]){struct harness *h=v;for(unsigned i=0;i<32;i++)p[i]=(uint8_t)(0x40+i+h->random_calls);h->random_calls++;return 0;}
static int send_eapol(void *v,const uint8_t *p,size_t n){struct harness *h=v;if(h->fail_send)return -1;assert(n<=sizeof(h->sent));memcpy(h->sent,p,n);h->sent_bytes=n;h->sends++;return 0;}
static int install_key(void *v,unsigned index,unsigned pairwise,uint64_t rsc,const uint8_t key[16]){
    struct harness *h=v;if(h->fail_key)return -1;
    h->last_index=index;h->last_pairwise=pairwise;h->last_rsc=rsc;memcpy(h->last_key,key,16);
    if(h->no_op){h->ready=1;return 0;}
    h->installs++;h->ready=0;h->port_open=0;return 0;
}
static int key_ready(void *v,unsigned index,unsigned pairwise){struct harness *h=v;assert(h->last_index==index && h->last_pairwise==pairwise);return h->fail_ready?-1:(int)h->ready;}
static int authorize(void *v,unsigned open){struct harness *h=v;if(open && h->fail_authorize)return -1;h->port_open=open;if(open)h->opened++;else h->closed++;return 0;}
static uint64_t now_ms(void *v){return ((struct harness *)v)->now;}
static void initialize(struct gwr_state *s,struct harness *h){
    memset(h,0,sizeof(*h));memset(s,0,sizeof(*s));uint8_t pmk[32];for(unsigned i=0;i<32;i++)pmk[i]=(uint8_t)i;
    struct gwr_ops ops={h,random32,gwr_crypto_hmac_sha1,gwr_crypto_unwrap,send_eapol,install_key,key_ready,authorize,now_ms};
    assert(gwr_init(s,&ops,pmk,local,ap,station_rsn,sizeof(station_rsn),access_rsn,sizeof(access_rsn))==GWR_OK);
}
static void hex(uint8_t *out,const char *in){for(size_t i=0;in[2*i];i++){unsigned byte;assert(sscanf(in+2*i,"%2x",&byte)==1);out[i]=(uint8_t)byte;}}
static void sign_frame(const uint8_t kck[16],uint8_t *frame,size_t n){
    uint8_t digest[20];unsigned bytes=20;memset(frame+81,0,16);assert(HMAC(EVP_sha1(),kck,16,frame,n,digest,&bytes));assert(bytes==20);memcpy(frame+81,digest,16);
}
static void verify_response(const struct gwr_state *s,const struct harness *h,unsigned info,uint64_t replay){
    assert(h->sent_bytes>=99 && gwr_be16(h->sent+2)==h->sent_bytes-4 && gwr_be16(h->sent+5)==info);
    assert(gwr_be16(h->sent+7)==0 && gwr_be64(h->sent+9)==replay);
    uint8_t copy[512];memcpy(copy,h->sent,h->sent_bytes);sign_frame(s->ptk,copy,h->sent_bytes);assert(!memcmp(copy+81,h->sent+81,16));
}
static size_t message1(uint8_t frame[1024],uint64_t replay,unsigned delta){
    memset(frame,0,1024);frame[0]=2;frame[1]=3;frame[4]=2;gwr_w16(frame+2,95);gwr_w16(frame+5,0x008a);gwr_w16(frame+7,16);gwr_w64(frame+9,replay);
    for(unsigned i=0;i<32;i++)frame[17+i]=(uint8_t)(0x80+i+delta);return 99;
}
/* AP uses OpenSSL's separate RFC3394 implementation, never the tested unwrap. */
static size_t key_message(const struct gwr_state *s,uint8_t frame[1024],uint64_t replay,unsigned pairwise,unsigned index,unsigned gtk_byte,unsigned has_gtk,unsigned encrypted){
    uint8_t plain[64]={0};size_t bytes=0;
    if(pairwise){memcpy(plain,access_rsn,sizeof(access_rsn));bytes=sizeof(access_rsn);}
    if(has_gtk){uint8_t *v=plain+bytes;v[0]=221;v[1]=22;v[2]=0;v[3]=15;v[4]=172;v[5]=1;v[6]=(uint8_t)index;v[7]=0;memset(v+8,gtk_byte,16);bytes+=24;}
    size_t n=message1(frame,replay,0);(void)n;
    memcpy(frame+17,s->anonce,32);gwr_w16(frame+5,pairwise?(encrypted?0x13ca:0x03ca):0x1382);frame[65]=9;
    if(encrypted){if(bytes<16 || (bytes&7)){plain[bytes++]=221;while(bytes<16 || (bytes&7))plain[bytes++]=0;}
        AES_KEY aes;assert(!AES_set_encrypt_key(s->ptk+16,128,&aes));assert(AES_wrap_key(&aes,NULL,frame+99,plain,(unsigned)bytes)==(int)bytes+8);bytes+=8;
    }else memcpy(frame+99,plain,bytes);
    gwr_w16(frame+97,(unsigned)bytes);gwr_w16(frame+2,95+(unsigned)bytes);sign_frame(s->ptk,frame,99+bytes);return 99+bytes;
}
static void start(struct gwr_state *s,struct harness *h){uint8_t frame[1024];size_t n=message1(frame,1,0);assert(gwr_receive(s,ap,frame,n)==GWR_OK);assert(s->phase==GWR_WAIT_M3 && h->sends==1);verify_response(s,h,0x010a,1);}
static void acknowledge_keys(struct gwr_state *s,struct harness *h,unsigned gtk){
    unsigned installs=h->installs;assert(gwr_tick(s)==GWR_OK && h->installs==installs+1 && h->last_pairwise);
    assert(gwr_tick(s)==GWR_OK && h->installs==installs+1 && !s->authorized);
    h->ready=1;assert(gwr_tick(s)==GWR_OK && s->phase==GWR_GTK_WAIT);
    if(gtk){assert(gwr_tick(s)==GWR_OK && h->installs==installs+2 && !h->last_pairwise && !s->authorized);
        assert(gwr_tick(s)==GWR_OK && !s->authorized);h->ready=1;}
    assert(gwr_tick(s)==GWR_OK);
}
static void complete(struct gwr_state *s,struct harness *h){uint8_t frame[1024];start(s,h);size_t n=key_message(s,frame,2,1,1,0x77,1,1);assert(gwr_receive(s,ap,frame,n)==GWR_OK);acknowledge_keys(s,h,1);assert(s->phase==GWR_RUN && s->authorized);}
static void test_crypto(void){
    uint8_t key[80],out[64],expected[64];memset(key,0x0b,20);assert(!gwr_crypto_hmac_sha1(NULL,key,20,(const uint8_t *)"Hi There",8,out));
    hex(expected,"b617318655057264e28bc0b6fb378c8ef146be00");assert(!memcmp(out,expected,20));
    memset(key,0xaa,sizeof(key));const char *longkey="Test Using Larger Than Block-Size Key - Hash Key First";
    assert(!gwr_crypto_hmac_sha1(NULL,key,sizeof(key),(const uint8_t *)longkey,strlen(longkey),out));
    hex(expected,"aa4ae5e15272d00e95705637ce8a3b55ed402112");assert(!memcmp(out,expected,20));
    hex(key,"000102030405060708090a0b0c0d0e0f");uint8_t wrapped[24];hex(wrapped,"1fa68b0a8112b447aef34bd8fb5a7b829d3e862371d2cfe5");
    size_t n=sizeof(out);assert(!gwr_crypto_unwrap(NULL,key,wrapped,sizeof(wrapped),out,&n) && n==16);
    hex(expected,"00112233445566778899aabbccddeeff");assert(!memcmp(out,expected,16));
    n=sizeof(out);wrapped[5]^=1;memset(out,0xaa,64);assert(gwr_crypto_unwrap(NULL,key,wrapped,sizeof(wrapped),out,&n) && !n && !gwr_nonzero(out,16));wrapped[5]^=1;
    n=15;assert(gwr_crypto_unwrap(NULL,key,wrapped,24,out,&n) && !n);
    n=24;assert(!gwr_crypto_unwrap(NULL,key,wrapped,24,wrapped,&n) && !memcmp(wrapped,expected,16));
    puts("RSN crypto: RFC2202 HMAC-SHA1 and RFC3394 AES unwrap vectors, integrity, bounds, in-place passed");
}
static void test_handshake(void){
    struct gwr_state s;struct harness h;initialize(&s,&h);start(&s,&h);
    uint8_t expected[64];hex(expected,"6abbbd80641e857359643339a9940ba322ca80053f6418c29797cc57f447f660e9e6ea4f1b951c5839b9940b7402a290759ae6f4694f0745cd4fd3cf09b87f55");
    assert(!memcmp(s.ptk,expected,64));assert(h.sent_bytes==121 && !memcmp(h.sent+99,station_rsn,22));
    uint8_t frame[1024];size_t n=message1(frame,1,0);assert(gwr_receive(&s,ap,frame,n)==GWR_OK && h.random_calls==1);
    verify_response(&s,&h,0x010a,1);n=message1(frame,2,0);assert(gwr_receive(&s,ap,frame,n)==GWR_OK && h.random_calls==1);
    n=key_message(&s,frame,3,1,1,0x77,1,1);unsigned sends=h.sends;
    assert(gwr_receive(&s,ap,frame,n)==GWR_OK && s.phase==GWR_PTK_WAIT && h.installs==0 && h.sends==sends);
    assert(gwr_receive(&s,ap,frame,n)==GWR_OK && h.installs==0 && h.sends==sends);
    assert(!gwr_tick(&s) && h.installs==1 && h.last_pairwise && !memcmp(h.last_key,expected+32,16));
    assert(!gwr_tick(&s) && !s.authorized && h.sends==sends);h.ready=1;assert(!gwr_tick(&s) && s.phase==GWR_GTK_WAIT);
    assert(!gwr_tick(&s) && h.installs==2 && !h.last_pairwise && h.last_index==1 && h.last_rsc==9 && h.last_key[0]==0x77);
    assert(gwr_receive(&s,ap,frame,n)==GWR_OK && h.installs==2 && h.sends==sends);h.ready=1;assert(!gwr_tick(&s));
    assert(s.phase==GWR_RUN && s.authorized && h.opened==1);verify_response(&s,&h,0x030a,3);
    assert(!gwr_nonzero(s.ptk+32,32) && !gwr_nonzero(s.pending_gtk,16));
    sends=h.sends;assert(!gwr_receive(&s,ap,frame,n) && h.installs==2 && h.sends==sends+1);
    n=key_message(&s,frame,4,1,1,0x77,1,1);assert(!gwr_receive(&s,ap,frame,n) && h.installs==2);verify_response(&s,&h,0x030a,4);
    n=key_message(&s,frame,5,1,1,0x66,1,1);assert(gwr_receive(&s,ap,frame,n)==GWR_DROP && s.replay==4);
    n=key_message(&s,frame,5,0,2,0x66,1,1);sends=h.sends;assert(!gwr_receive(&s,ap,frame,n) && s.phase==GWR_GROUP_WAIT);
    assert(!gwr_tick(&s) && h.installs==3 && !s.authorized && !h.port_open && h.sends==sends);assert(!gwr_tick(&s));
    assert(!gwr_receive(&s,ap,frame,n) && h.installs==3);h.ready=1;assert(!gwr_tick(&s));verify_response(&s,&h,0x0302,5);
    assert(h.opened==2 && h.port_open && s.phase==GWR_RUN);assert(!gwr_receive(&s,ap,frame,n) && h.installs==3);
    n=key_message(&s,frame,6,0,2,0x66,1,1);assert(!gwr_receive(&s,ap,frame,n));assert(!gwr_tick(&s) && h.installs==3);verify_response(&s,&h,0x0302,6);
    n=key_message(&s,frame,7,0,3,0x66,1,1);assert(gwr_receive(&s,ap,frame,n)==GWR_DROP && s.replay==6 && h.installs==3);
    gwr_clear(&s);assert(!gwr_nonzero((uint8_t *)&s,sizeof(s)) && h.closed==2);
    puts("RSN handshake: independent PTK/MIC, separate AP/station RSN, firmware-gated PTK/GTK/M4, group rekey and KRACK retries passed");
}
static void test_rejections(void){
    struct gwr_state s;struct harness h;initialize(&s,&h);start(&s,&h);uint8_t frame[1024];size_t n=key_message(&s,frame,2,1,1,0x77,1,1);
    uint8_t wrong[6]={2,0,0,0,0,3};assert(gwr_receive(&s,wrong,frame,n)==GWR_DROP);
    assert(gwr_receive(&s,ap,frame,n-1)==GWR_DROP);frame[81]^=1;assert(gwr_receive(&s,ap,frame,n)==GWR_DROP);frame[81]^=1;
    frame[17]^=1;sign_frame(s.ptk,frame,n);assert(gwr_receive(&s,ap,frame,n)==GWR_DROP);frame[17]^=1;
    gwr_w64(frame+9,1);sign_frame(s.ptk,frame,n);assert(gwr_receive(&s,ap,frame,n)==GWR_DROP);
    gwr_w64(frame+9,2);frame[71]=1;sign_frame(s.ptk,frame,n);assert(gwr_receive(&s,ap,frame,n)==GWR_DROP);frame[71]=0;
    frame[105]^=1;sign_frame(s.ptk,frame,n);assert(gwr_receive(&s,ap,frame,n)==GWR_DROP);
    n=key_message(&s,frame,2,1,1,0x77,1,0);assert(gwr_receive(&s,ap,frame,n)==GWR_DROP); /* plaintext GTK */
    /* Association RSN is valid but is not the authenticated AP RSN. */
    n=key_message(&s,frame,2,1,1,0,0,0);memcpy(frame+99,station_rsn,22);sign_frame(s.ptk,frame,n);assert(gwr_receive(&s,ap,frame,n)==GWR_DROP);
    assert(s.phase==GWR_WAIT_M3 && !h.installs && !s.authorized);h.now=1000;assert(!gwr_tick(&s) && h.sends==2);
    h.now=10000;assert(gwr_tick(&s)==GWR_TIMEOUT && s.phase==GWR_FAILED && !s.authorized);
    assert(!gwr_nonzero(s.pmk,32) && !gwr_nonzero(s.ptk,64) && !gwr_nonzero(s.snonce,32));
    initialize(&s,&h);start(&s,&h);n=key_message(&s,frame,2,1,1,0x77,1,1);assert(!gwr_receive(&s,ap,frame,n));h.fail_key=1;
    assert(gwr_tick(&s)==GWR_KEY && s.phase==GWR_FAILED && !s.authorized && !gwr_nonzero(s.ptk,64));
    initialize(&s,&h);start(&s,&h);n=key_message(&s,frame,2,1,1,0x77,1,1);assert(!gwr_receive(&s,ap,frame,n));assert(!gwr_tick(&s));h.fail_ready=1;
    assert(gwr_tick(&s)==GWR_KEY && !s.authorized);
    initialize(&s,&h);start(&s,&h);n=key_message(&s,frame,2,1,1,0x77,1,1);assert(!gwr_receive(&s,ap,frame,n));assert(!gwr_tick(&s));h.now=5000;
    assert(gwr_tick(&s)==GWR_TIMEOUT && !s.authorized);
    initialize(&s,&h);h.fail_send=1;n=message1(frame,1,0);assert(gwr_receive(&s,ap,frame,n)==GWR_TRANSPORT && s.phase==GWR_FAILED);
    puts("RSN rejection: address, lengths, MIC, nonce, replay, RSC, unwrap, RSN binding, plaintext GTK, timeouts and FW/send failure passed");
}
static void test_separate_group_and_rekey(void){
    struct gwr_state s;struct harness h;initialize(&s,&h);start(&s,&h);uint8_t frame[1024];size_t n=key_message(&s,frame,2,1,1,0,0,0);
    assert(!gwr_receive(&s,ap,frame,n));acknowledge_keys(&s,&h,0);assert(s.phase==GWR_WAIT_GROUP && !s.authorized && !h.opened);verify_response(&s,&h,0x030a,2);
    n=key_message(&s,frame,3,0,1,0x77,1,1);assert(!gwr_receive(&s,ap,frame,n));assert(!gwr_tick(&s) && h.installs==2);h.ready=1;
    assert(!gwr_tick(&s) && s.phase==GWR_RUN && h.opened==1);
    n=message1(frame,4,1);assert(!gwr_receive(&s,ap,frame,n) && h.random_calls==2 && s.phase==GWR_WAIT_M3);
    n=key_message(&s,frame,5,1,1,0x77,1,1);assert(!gwr_receive(&s,ap,frame,n) && !s.authorized);
    assert(!gwr_tick(&s) && h.installs==3);h.ready=1;assert(!gwr_tick(&s));assert(!gwr_tick(&s));
    assert(s.phase==GWR_RUN && s.authorized && h.installs==3 && h.opened==2);verify_response(&s,&h,0x030a,5);
    initialize(&s,&h);complete(&s,&h);assert(h.installs==2);
    puts("RSN rekey: separate GTK exchange keeps port closed; pairwise rekey refreshes PTK and preserves existing GTK PN passed");
}
static void test_pending_retries(void){
    struct gwr_state s;struct harness h;initialize(&s,&h);start(&s,&h);uint8_t frame[1024];
    size_t n=key_message(&s,frame,2,1,1,0x77,1,1);assert(!gwr_receive(&s,ap,frame,n));assert(!gwr_tick(&s));
    uint64_t deadline=s.deadline;unsigned sends=h.sends;h.now=1000;
    n=key_message(&s,frame,3,1,1,0x77,1,1);frame[65]=20;sign_frame(s.ptk,frame,n);
    assert(!gwr_receive(&s,ap,frame,n) && s.replay==3 && s.deadline==deadline && h.installs==1 && h.sends==sends);
    assert(s.pending_rsc==9);h.ready=1;assert(!gwr_tick(&s));assert(!gwr_tick(&s) && h.installs==2 && h.last_rsc==9);
    n=key_message(&s,frame,4,1,1,0x77,1,1);assert(!gwr_receive(&s,ap,frame,n) && s.deadline==deadline);
    h.ready=1;assert(!gwr_tick(&s) && s.authorized && h.port_open);verify_response(&s,&h,0x030a,4);
    n=key_message(&s,frame,5,0,2,0x66,1,1);assert(!gwr_receive(&s,ap,frame,n));assert(!gwr_tick(&s));
    assert(!s.authorized && !h.port_open && h.installs==3);deadline=s.deadline;sends=h.sends;h.now=2000;
    assert(!gwr_receive(&s,ap,frame,n) && s.deadline==deadline);
    n=key_message(&s,frame,6,0,2,0x66,1,1);frame[65]=21;sign_frame(s.ptk,frame,n);
    assert(!gwr_receive(&s,ap,frame,n) && s.replay==6 && s.deadline==deadline && s.pending_rsc==9);
    assert(h.installs==3 && h.sends==sends);
    n=key_message(&s,frame,7,0,2,0x55,1,1);assert(gwr_receive(&s,ap,frame,n)==GWR_DROP && s.replay==6);
    n=key_message(&s,frame,7,0,3,0x66,1,1);assert(gwr_receive(&s,ap,frame,n)==GWR_DROP && s.replay==6);
    h.ready=1;assert(!gwr_tick(&s) && s.authorized && h.port_open && h.opened==2);verify_response(&s,&h,0x0302,6);
    n=key_message(&s,frame,7,0,2,0x66,1,1);assert(!gwr_receive(&s,ap,frame,n));assert(!gwr_tick(&s));
    assert(h.installs==3 && h.opened==2 && s.authorized && h.port_open);verify_response(&s,&h,0x0302,7);
    /* A delayed M3 may follow a group exchange; subsequent exact copies must
     * still receive M4 without installing either key again. */
    n=key_message(&s,frame,8,1,1,0x77,1,1);assert(!gwr_receive(&s,ap,frame,n));verify_response(&s,&h,0x030a,8);
    sends=h.sends;assert(!gwr_receive(&s,ap,frame,n) && h.sends==sends+1 && h.installs==3);
    puts("RSN pending retries: M3/GTK replay advances preserve deadline/RSC/PN; GTK ACK restores port; cached keys remain installed once");
}
static void test_install_authorization(void){
    struct gwr_state s;struct harness h;uint8_t frame[1024];initialize(&s,&h);complete(&s,&h);
    /* Reproduce an already-installed identical PTK in the platform. The
     * no-op install must still restore the port closed on receipt of M3. */
    h.random_calls=0;size_t n=message1(frame,3,0);assert(!gwr_receive(&s,ap,frame,n));
    n=key_message(&s,frame,4,1,1,0x77,1,1);assert(!gwr_receive(&s,ap,frame,n) && !h.port_open);
    h.no_op=1;assert(!gwr_tick(&s) && !s.authorized && h.installs==2);
    assert(!gwr_tick(&s) && s.phase==GWR_GTK_WAIT);assert(!gwr_tick(&s));
    assert(s.authorized && h.port_open && h.opened==2 && h.installs==2);verify_response(&s,&h,0x030a,4);
    initialize(&s,&h);complete(&s,&h);n=key_message(&s,frame,3,0,2,0x66,1,1);
    assert(!gwr_receive(&s,ap,frame,n));assert(!gwr_tick(&s) && !h.port_open);
    h.ready=1;h.fail_authorize=1;assert(gwr_tick(&s)==GWR_KEY && s.phase==GWR_FAILED && !s.authorized && !h.port_open);
    assert(!gwr_nonzero(s.pmk,32) && !gwr_nonzero(s.ptk,64) && !gwr_nonzero((const uint8_t *)s.gtk,sizeof(s.gtk)));
    puts("RSN authorization: successful no-op installs restore port; failed reopen erases keys and remains closed");
}
static const uint8_t sae_rsn[28]={48,26,1,0,0,15,172,4,1,0,0,15,172,4,1,0,0,15,172,8,0xc0,0,0,0,0,15,172,6};
static int hmac256(void *v,const uint8_t *key,size_t kn,const uint8_t *p,size_t n,uint8_t out[32]){(void)v;unsigned bytes=32;return HMAC(EVP_sha256(),key,(int)kn,p,n,out,&bytes)&&bytes==32?0:-1;}
static int pmf_random(void *v,uint8_t *p,size_t n){(void)v;memset(p,0x31,n);return 0;}
static int install_igtk(void *v,unsigned index,uint64_t pn,const uint8_t key[16]){struct harness *h=v;if(h->fail_igtk)return -1;h->igtk_installs++;return gpm_install_igtk(&h->pmf,index,pn,key);}
static void cmac_sign(const uint8_t key[16],uint8_t *p,size_t n){CMAC_CTX *ctx=CMAC_CTX_new();size_t bytes=16;uint8_t digest[16];memset(p+81,0,16);
    assert(ctx && CMAC_Init(ctx,key,16,EVP_aes_128_cbc(),NULL)&&CMAC_Update(ctx,p,n)&&CMAC_Final(ctx,digest,&bytes)&&bytes==16);CMAC_CTX_free(ctx);memcpy(p+81,digest,16);}
static void initialize_sae(struct gwr_state *s,struct harness *h){
    initialize(s,h);struct gwr_ops ops=s->ops;struct gwr_security security={1,2,1,hmac256,gpm_crypto_cmac,install_igtk};uint8_t pmk[32];for(unsigned i=0;i<32;i++)pmk[i]=(uint8_t)i;
    struct gpm_ops pmf_ops={h,gpm_crypto_cmac,pmf_random,now_ms};assert(!gpm_init(&h->pmf,&pmf_ops,local,ap));
    assert(!gwr_init_secure(s,&ops,&security,pmk,local,ap,sae_rsn,28,sae_rsn,28));
}
static size_t sae_key_message(struct gwr_state *s,uint8_t frame[1024],uint64_t replay,unsigned pairwise,unsigned has_gtk,unsigned has_igtk,unsigned bad_rsnxe,unsigned key_byte,uint64_t ipn){
    uint8_t plain[128]={0};size_t n=0;if(pairwise){memcpy(plain,sae_rsn,28);n=28;plain[n++]=244;plain[n++]=1;plain[n++]=bad_rsnxe?0:0x20;}
    if(has_gtk){uint8_t *v=plain+n;v[0]=221;v[1]=22;v[3]=15;v[4]=172;v[5]=1;v[6]=1;memset(v+8,0x77,16);n+=24;}
    if(has_igtk){uint8_t *v=plain+n;v[0]=221;v[1]=28;v[3]=15;v[4]=172;v[5]=9;v[6]=4;for(unsigned i=0;i<6;i++)v[8+i]=(uint8_t)(ipn>>(8*i));memset(v+14,key_byte,16);n+=30;}
    if(n<16 || (n&7)){plain[n++]=221;while(n<16 || (n&7))plain[n++]=0;}
    message1(frame,replay,0);memcpy(frame+17,s->anonce,32);gwr_w16(frame+5,pairwise?0x13c8:0x1380);
    AES_KEY aes;assert(!AES_set_encrypt_key(s->ptk+16,128,&aes));assert(AES_wrap_key(&aes,NULL,frame+99,plain,(unsigned)n)==(int)n+8);n+=8;
    gwr_w16(frame+97,(unsigned)n);gwr_w16(frame+2,95+(unsigned)n);cmac_sign(s->ptk,frame,99+n);return 99+n;
}
static void start_sae(struct gwr_state *s,struct harness *h){uint8_t frame[1024];size_t n=message1(frame,1,0);assert(gwr_receive(s,ap,frame,n)==GWR_DROP);
    gwr_w16(frame+5,0x0088);assert(!gwr_receive(s,ap,frame,n));uint8_t expected[48];hex(expected,"2fafa186db813f170ad8f146d7e63fd820934c60568215ee98ba5343602a29069b0c61cfd6cb9ec39c47e1f0609d0208");
    assert(!memcmp(expected,s->ptk,48)&&gwr_be16(h->sent+5)==0x0108&&h->sent_bytes==130&&!memcmp(h->sent+127,"\xf4\x01\x20",3));
    uint8_t copy[512];memcpy(copy,h->sent,h->sent_bytes);cmac_sign(expected,copy,h->sent_bytes);assert(!memcmp(copy+81,h->sent+81,16));
}
static void test_sae_pmf(void){
    struct gwr_state s;struct harness h;uint8_t frame[1024];initialize_sae(&s,&h);start_sae(&s,&h);
    size_t n=sae_key_message(&s,frame,2,1,1,1,1,0x55,9);assert(gwr_receive(&s,ap,frame,n)==GWR_DROP && !h.igtk_installs);
    n=sae_key_message(&s,frame,2,1,1,1,0,0x55,9);frame[81]^=1;assert(gwr_receive(&s,ap,frame,n)==GWR_DROP && !h.igtk_installs);frame[81]^=1;
    assert(!gwr_receive(&s,ap,frame,n)&&!h.igtk_installs);acknowledge_keys(&s,&h,1);
    assert(s.phase==GWR_RUN && s.authorized && h.igtk_installs==1 && h.pmf.ipn[0]==9 && gwr_be16(h.sent+5)==0x0308);
    assert(!gwr_receive(&s,ap,frame,n)&&h.igtk_installs==1);h.pmf.ipn[0]=50;
    n=sae_key_message(&s,frame,3,1,1,1,0,0x55,1);assert(!gwr_receive(&s,ap,frame,n)&&h.igtk_installs==1 && h.pmf.ipn[0]==50);
    n=sae_key_message(&s,frame,4,0,0,1,0,0x55,0);assert(!gwr_receive(&s,ap,frame,n));assert(!gwr_tick(&s)&&s.authorized&&h.pmf.ipn[0]==50);
    n=sae_key_message(&s,frame,5,0,0,1,0,0x66,1);assert(!gwr_receive(&s,ap,frame,n));assert(!gwr_tick(&s)&&h.pmf.ipn[0]==1&&h.pmf.igtk[0][0]==0x66);
    initialize_sae(&s,&h);start_sae(&s,&h);n=sae_key_message(&s,frame,2,1,1,0,0,0,0);assert(!gwr_receive(&s,ap,frame,n));acknowledge_keys(&s,&h,1);
    assert(s.phase==GWR_WAIT_GROUP && !s.authorized && !h.opened);
    n=sae_key_message(&s,frame,3,0,0,1,0,0x55,3);assert(!gwr_receive(&s,ap,frame,n));assert(!gwr_tick(&s)&&s.authorized&&s.phase==GWR_RUN);
    n=sae_key_message(&s,frame,4,0,0,1,0,0x66,1);assert(!gwr_receive(&s,ap,frame,n));h.fail_igtk=1;assert(gwr_tick(&s)==GWR_KEY&&!s.authorized&&!h.port_open);
    puts("SAE RSN: independent SHA256 PRF-384/AES-CMAC, descriptor downgrade and RSNXE rejection, firmware-gated IGTK, IGTK-only rekey, missing-IGTK closed port and IPN retry preservation passed");
}
int main(void){test_crypto();test_handshake();test_rejections();test_separate_group_and_rekey();test_pending_retries();test_install_authorization();test_sae_pmf();return 0;}
