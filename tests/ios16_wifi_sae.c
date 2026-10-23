#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <openssl/ec.h>
#include <openssl/obj_mac.h>
#include <openssl/hmac.h>
#include "../ios16/giulia_wifi_sae.h"
#include "sae_vectors.inc"
static void oracle_hmac(const uint8_t *key,size_t kn,const uint8_t *data,size_t n,uint8_t out[32]){unsigned len=32;assert(HMAC(EVP_sha256(),key,(int)kn,data,n,out,&len)&&len==32);}
static void oracle_kdf(const uint8_t key[32],const char *label,const uint8_t *context,size_t cn,uint8_t *out,size_t n){uint8_t p[256],digest[32];size_t ln=strlen(label);memcpy(p+2,label,ln);memcpy(p+2+ln,context,cn);p[2+ln+cn]=(uint8_t)(8*n);p[3+ln+cn]=(uint8_t)(n>>5);for(unsigned i=0;i<(n+31)/32;i++){p[0]=(uint8_t)(i+1);p[1]=0;oracle_hmac(key,32,p,ln+cn+4,digest);size_t z=n-i*32;if(z>32)z=32;memcpy(out+i*32,digest,z);}}
static void test_vectors(void){
    uint8_t pwe[65],scalar[32],element[65],k[32],seed[32],sum[32],keys[64],zero[32]={0},peer[65]={4};
    const uint8_t password[]="mekmitasdigoat";
    assert(!gsa_crypto_pwe(v_addr1,v_addr2,password,sizeof(password)-1,(const uint8_t *)"byteme",6,0,pwe));
    assert(!gsa_crypto_commit(pwe,v_local_rand,v_local_mask,scalar,element));
    assert(!memcmp(scalar,v_local_commit+2,32)&&!memcmp(element+1,v_local_commit+34,64));
    memcpy(peer+1,v_peer_commit+34,64);assert(!gsa_crypto_shared(pwe,v_local_rand,v_peer_commit+2,peer,k));
    oracle_hmac(zero,32,k,32,seed);assert(!gsa_crypto_add(scalar,v_peer_commit+2,sum));oracle_kdf(seed,"SAE KCK and PMK",sum,32,keys,64);
    assert(!memcmp(keys,v_kck,32)&&!memcmp(keys+32,v_pmk,32)&&!memcmp(sum,v_pmkid,16));
    /* hostap's H2E fixture has a password identifier: the HKDF input is
     * password || identifier. This tests the published PWE, without claiming
     * that the bounded station protocol implements the identifier element. */
    const uint8_t h2e_password[]="mekmitasdigoatpsk4internet";
    assert(!gsa_crypto_pwe(v_addr1b,v_addr2b,h2e_password,sizeof(h2e_password)-1,(const uint8_t *)"byteme",6,1,pwe));
    assert(!memcmp(pwe+1,v_pwe_19_x,32)&&!memcmp(pwe+33,v_pwe_19_y,32));
    assert(!gsa_crypto_scalar(zero));zero[31]=1;assert(!gsa_crypto_scalar(zero));zero[31]=2;assert(gsa_crypto_scalar(zero));
    memset(peer,0,sizeof(peer));assert(gsa_crypto_shared(pwe,v_local_rand,v_peer_commit+2,peer,k));
    puts("SAE crypto: IEEE 802.11 Annex J.10 commit/KCK/PMK/PMKID and hostap H2E PWE vectors passed");
}
struct harness {uint64_t now;unsigned random_calls,sends,busy,failed;uint8_t sent[GSA_AUTH_MAX];size_t bytes;};
static int random_bytes(void *v,uint8_t *p,size_t n){struct harness *h=v;assert(n==32);memcpy(p,h->random_calls++&1?v_local_mask:v_local_rand,32);return 0;}
static int send_auth(void *v,const uint8_t *p,size_t n){struct harness *h=v;if(h->failed)return -1;if(h->busy)return 1;memcpy(h->sent,p,n);h->bytes=n;h->sends++;return 0;}
static uint64_t now(void *v){return ((struct harness *)v)->now;}
static const uint8_t local[6]={2,0,0,0,0,1},ap[6]={2,0,0,0,0,2};
static void initialize(struct gsa_state *s,struct harness *h,unsigned h2e){memset(h,0,sizeof(*h));struct gsa_ops ops={h,random_bytes,send_auth,now};assert(!gsa_init(s,&ops,local,ap,(const uint8_t *)"correct password",16,(const uint8_t *)"lab",3,h2e));assert(!gsa_start(s));}
/* Independently build AP commit/shared secret with OpenSSL EC/BIGNUM. */
static void oracle_ap(struct gsa_state *s,uint8_t commit[104],uint8_t confirm[40],uint8_t pmk[32]){
    EC_GROUP *group=EC_GROUP_new_by_curve_name(NID_X9_62_prime256v1);BN_CTX *ctx=BN_CTX_new();
    BIGNUM *rand=BN_new(),*mask=BN_new(),*order=BN_new(),*scalar=BN_new(),*peer=BN_bin2bn(s->scalar,32,NULL),*sum=BN_new();
    EC_POINT *pwe=EC_POINT_new(group),*element=EC_POINT_new(group),*point=EC_POINT_new(group),*other=EC_POINT_new(group);
    assert(BN_set_word(rand,0x1234567)&&BN_set_word(mask,0x7654321)&&EC_GROUP_get_order(group,order,ctx));
    assert(EC_POINT_oct2point(group,pwe,s->pwe,65,ctx));assert(BN_mod_add(scalar,rand,mask,order,ctx));
    assert(EC_POINT_mul(group,element,NULL,pwe,mask,ctx)&&EC_POINT_invert(group,element,ctx));
    memset(commit,0,104);gsa_w16(commit,3);gsa_w16(commit+2,1);gsa_w16(commit+4,s->h2e?126:0);gsa_w16(commit+6,19);
    assert(BN_bn2binpad(scalar,commit+8,32)==32);uint8_t encoded[65];assert(EC_POINT_point2oct(group,element,POINT_CONVERSION_UNCOMPRESSED,encoded,65,ctx)==65);memcpy(commit+40,encoded+1,64);
    assert(EC_POINT_mul(group,point,NULL,pwe,peer,ctx)&&EC_POINT_oct2point(group,other,s->element,65,ctx));
    assert(EC_POINT_add(group,point,point,other,ctx)&&EC_POINT_mul(group,point,NULL,point,rand,ctx));
    assert(EC_POINT_point2oct(group,point,POINT_CONVERSION_UNCOMPRESSED,encoded,65,ctx)==65);
    uint8_t zero[32]={0},seed[32],context[32],keys[64],message[194];oracle_hmac(zero,32,encoded+1,32,seed);
    assert(BN_mod_add(sum,scalar,peer,order,ctx)&&BN_bn2binpad(sum,context,32)==32);oracle_kdf(seed,"SAE KCK and PMK",context,32,keys,64);memcpy(pmk,keys+32,32);
    memset(confirm,0,40);gsa_w16(confirm,3);gsa_w16(confirm+2,2);gsa_w16(confirm+6,1);
    memcpy(message,confirm+6,2);memcpy(message+2,commit+8,96);memcpy(message+98,s->scalar,32);memcpy(message+130,s->element+1,64);oracle_hmac(keys,32,message,194,confirm+8);
    BN_clear_free(rand);BN_clear_free(mask);BN_free(order);BN_free(scalar);BN_free(peer);BN_free(sum);BN_CTX_free(ctx);EC_POINT_free(pwe);EC_POINT_free(element);EC_POINT_free(point);EC_POINT_free(other);EC_GROUP_free(group);
}
static void test_state(unsigned h2e){
    struct gsa_state s;struct harness h;initialize(&s,&h,h2e);assert(h.bytes==104&&h.sends==1&&s.phase==GSA_COMMITTED);
    uint8_t commit[104],confirm[40],pmk[32];oracle_ap(&s,commit,confirm,pmk);
    assert(gsa_receive(&s,ap,h.sent,h.bytes)==GSA_DROP);commit[40]^=1;assert(gsa_receive(&s,ap,commit,104)==GSA_DROP);commit[40]^=1;
    assert(!gsa_receive(&s,ap,commit,104)&&s.phase==GSA_CONFIRMED&&h.bytes==40&&!memcmp(s.pmk,pmk,32));
    confirm[8]^=1;assert(gsa_receive(&s,ap,confirm,40)==GSA_DROP);confirm[8]^=1;
    assert(!gsa_receive(&s,ap,confirm,40)&&s.phase==GSA_ACCEPTED&&!memcmp(s.pmk,pmk,32));
    uint8_t zero[65]={0};assert(!memcmp(s.rand,zero,32)&&!memcmp(s.pwe,zero,65)&&!memcmp(s.kck,zero,32));
    unsigned sends=h.sends;assert(!gsa_receive(&s,ap,confirm,40)&&h.sends==sends+1);confirm[6]=2;assert(gsa_receive(&s,ap,confirm,40)==GSA_DROP);
    gsa_clear(&s);assert(!memcmp(s.pmk,zero,32));initialize(&s,&h,h2e);h.now=10000;assert(gsa_tick(&s)==GSA_TIMEOUT&&!memcmp(s.rand,zero,32));
    initialize(&s,&h,h2e);uint8_t token[43]={3,0,1,0,76,0,19,0};size_t n=40;
    if(h2e){token[8]=255;token[9]=33;token[10]=93;n=43;}memset(token+(h2e?11:8),0xa5,32);
    assert(!gsa_receive(&s,ap,token,n)&&s.token_bytes==32&&h.bytes==(h2e?139u:136u));
    assert(s.deadline==10000&&h.random_calls==2);h.busy=1;h.now=1000;assert(!gsa_tick(&s)&&s.pending);h.busy=0;assert(!gsa_tick(&s)&&!s.pending);h.failed=1;h.now=2000;assert(gsa_tick(&s)==GSA_TRANSPORT);
}
int main(void){test_vectors();test_state(0);test_state(1);puts("SAE station: independent OpenSSL AP commit/confirm, H&P/H2E, reflection/off-curve rejection, token, retry, busy and secret clearing passed");return 0;}
