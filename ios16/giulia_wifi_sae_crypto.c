/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * IEEE 802.11 SAE group 19 H&P and H2E. H&P always evaluates 40 candidates;
 * candidate selection, square roots, arithmetic and scalar multiplication
 * use fixed-size constant-time operations. H2E is SSWU with Z=-10.
 * Algorithms cross-checked against hostap sae.c (BSD), not copied from GPL.
 */
#include "giulia_wifi_sae_crypto.h"
#include "inner.h"

static void sc_zero(void *p,size_t n){volatile uint8_t *b=p;while(n--)*b++=0;}
static unsigned sc_equal(const void *a,const void *b,size_t n){const uint8_t *x=a,*y=b;unsigned d=0;while(n--)d|=*x++^*y++;return (d-1)>>31;}
static void sc_select(void *out,const void *yes,const void *no,size_t n,unsigned take){uint8_t *d=out;const uint8_t *a=yes,*b=no;uint8_t m=(uint8_t)(0u-take);while(n--){uint8_t x=*a++,y=*b++;*d++=(x&m)|(y&~m);}}
static const uint8_t sc_prime[32]={0xff,0xff,0xff,0xff,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff};
static const uint8_t sc_order[32]={0xff,0xff,0xff,0xff,0,0,0,0,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xbc,0xe6,0xfa,0xad,0xa7,0x17,0x9e,0x84,0xf3,0xb9,0xca,0xc2,0xfc,0x63,0x25,0x51};
static const uint8_t sc_b[32]={0x5a,0xc6,0x35,0xd8,0xaa,0x3a,0x93,0xe7,0xb3,0xeb,0xbd,0x55,0x76,0x98,0x86,0xbc,0x65,0x1d,0x06,0xb0,0xcc,0x53,0xb0,0xf6,0x3b,0xce,0x3c,0x3e,0x27,0xd2,0x60,0x4b};
static const uint8_t sc_sqrt[32]={0x3f,0xff,0xff,0xff,0xc0,0,0,0,0x40,0,0,0,0,0,0,0,0,0,0,0,0x40,0,0,0,0,0,0,0,0,0,0,0};
typedef uint32_t sc_int[10];
static void sc_modadd(sc_int out,const sc_int a,const sc_int b,const sc_int p){memcpy(out,a,sizeof(sc_int));br_i31_add(out,b,1);br_i31_sub(out,p,1u^br_i31_sub(out,p,0));}
static void sc_neg(sc_int out,const sc_int a,const sc_int p){memcpy(out,p,sizeof(sc_int));br_i31_sub(out,a,1);br_i31_sub(out,p,1u^br_i31_sub(out,p,0));}
static void sc_mul(sc_int out,const sc_int a,const sc_int b,const sc_int p){sc_int t;memcpy(t,a,sizeof(t));br_i31_to_monty(t,p);br_i31_montymul(out,t,b,p,br_i31_ninv31(p[1]));sc_zero(t,sizeof(t));}
static void sc_pow(sc_int out,const sc_int a,const uint8_t e[32],const sc_int p){sc_int t1,t2;memcpy(out,a,sizeof(sc_int));br_i31_modpow(out,e,32,p,br_i31_ninv31(p[1]),t1,t2);sc_zero(t1,sizeof(t1));sc_zero(t2,sizeof(t2));}
static void sc_curve(sc_int out,const sc_int x,const sc_int p){sc_int t,u,b;sc_mul(t,x,x,p);sc_mul(u,t,x,p);sc_modadd(t,x,x,p);sc_modadd(b,t,x,p);sc_neg(t,b,p);sc_modadd(b,u,t,p);br_i31_decode_mod(t,sc_b,32,p);sc_modadd(out,b,t,p);sc_zero(t,sizeof(t));sc_zero(u,sizeof(u));sc_zero(b,sizeof(b));}
static unsigned sc_root(sc_int out,const sc_int y2,const sc_int p){sc_int t;sc_pow(out,y2,sc_sqrt,p);sc_mul(t,out,out,p);unsigned ok=sc_equal(t,y2,sizeof(t));sc_zero(t,sizeof(t));return ok;}
static void sc_point(uint8_t out[65],const sc_int x,const sc_int y,unsigned parity,const sc_int p){sc_int neg;uint8_t a[32],b[32];sc_neg(neg,y,p);br_i31_encode(a,32,y);br_i31_encode(b,32,neg);out[0]=4;br_i31_encode(out+1,32,x);sc_select(out+33,a,b,32,1u^((a[31]&1)^parity));sc_zero(neg,sizeof(neg));sc_zero(a,32);sc_zero(b,32);}

int gsa_crypto_hmac(void *v,const uint8_t *key,size_t kn,const uint8_t *data,size_t n,uint8_t out[32]){
    (void)v;if(!key || (!data&&n) || !out)return -1;uint8_t pad[64]={0},inner[32];br_sha256_context s;
    if(kn>64){br_sha256_init(&s);br_sha256_update(&s,key,kn);br_sha256_out(&s,pad);}else memcpy(pad,key,kn);
    for(unsigned i=0;i<64;i++)pad[i]^=0x36;br_sha256_init(&s);br_sha256_update(&s,pad,64);if(n)br_sha256_update(&s,data,n);br_sha256_out(&s,inner);
    for(unsigned i=0;i<64;i++)pad[i]^=0x36^0x5c;br_sha256_init(&s);br_sha256_update(&s,pad,64);br_sha256_update(&s,inner,32);br_sha256_out(&s,out);
    sc_zero(&s,sizeof(s));sc_zero(pad,sizeof(pad));sc_zero(inner,sizeof(inner));return 0;
}
int gsa_crypto_kdf(const uint8_t key[32],const char *label,const uint8_t *context,size_t cn,uint8_t *out,size_t n){
    if(!key||!label||(!context&&cn)||!out||!n||n>128||cn>96)return -1;size_t ln=strlen(label);if(ln>48)return -1;
    uint8_t data[148],digest[32];memcpy(data+2,label,ln);if(cn)memcpy(data+2+ln,context,cn);
    data[2+ln+cn]=(uint8_t)(n*8);data[3+ln+cn]=(uint8_t)(n>>5);
    for(unsigned counter=1;counter<=(n+31)/32;counter++){data[0]=(uint8_t)counter;data[1]=0;gsa_crypto_hmac(NULL,key,32,data,ln+cn+4,digest);size_t z=n-(counter-1)*32;if(z>32)z=32;memcpy(out+(counter-1)*32,digest,z);}
    sc_zero(data,sizeof(data));sc_zero(digest,sizeof(digest));return 0;
}
static void sc_hkdf(const uint8_t key[32],const char *label,uint8_t out[48]){uint8_t data[80],t[32];size_t n=strlen(label);memcpy(data,label,n);data[n]=1;gsa_crypto_hmac(NULL,key,32,data,n+1,t);memcpy(out,t,32);memcpy(data,t,32);memcpy(data+32,label,n);data[32+n]=2;gsa_crypto_hmac(NULL,key,32,data,33+n,t);memcpy(out+32,t,16);sc_zero(data,sizeof(data));sc_zero(t,sizeof(t));}
static void sc_addresses(uint8_t out[12],const uint8_t a[6],const uint8_t b[6]){unsigned higher=memcmp(a,b,6)>0;memcpy(out,higher?a:b,6);memcpy(out+6,higher?b:a,6);}
static int sc_hunt(const uint8_t addrs[12],const uint8_t *password,size_t n,uint8_t out[65]){
    sc_int p,x,y2,y;uint8_t input[65],seed[32],value[32],candidate[65];unsigned found=0;br_i31_decode(p,sc_prime,32);memcpy(input,password,n);memset(out,0,65);
    for(unsigned counter=1;counter<=40;counter++){
        input[n]=(uint8_t)counter;gsa_crypto_hmac(NULL,addrs,12,input,n+1,seed);gsa_crypto_kdf(seed,"SAE Hunting and Pecking",sc_prime,32,value,32);
        unsigned valid=br_i31_decode_mod(x,value,32,p);sc_curve(y2,x,p);valid&=sc_root(y,y2,p);sc_point(candidate,x,y,seed[31]&1,p);
        sc_select(out,candidate,out,65,valid&(found^1));found|=valid;
    }
    sc_zero(x,sizeof(x));sc_zero(y,sizeof(y));sc_zero(y2,sizeof(y2));sc_zero(input,sizeof(input));sc_zero(seed,sizeof(seed));sc_zero(value,sizeof(value));sc_zero(candidate,sizeof(candidate));return found?0:-1;
}
static void sc_swu(uint8_t out[65],const sc_int u,const sc_int p){
    static const uint8_t inv[32]={0xff,0xff,0xff,0xff,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xfd};
    static const uint8_t b3[32]={0x73,0x97,0x67,0x47,0xe3,0x68,0xdb,0xf8,0x3b,0xf9,0x3f,0x1c,0x7c,0xdd,0x82,0x3e,0xcc,0x5f,0x02,0x3b,0x44,0x1b,0xe5,0xa7,0x69,0x44,0xbe,0xbf,0x62,0x9b,0x75,0x6e};
    static const uint8_t b30[32]={0xa5,0x28,0xbd,0x86,0x96,0xbd,0xaf,0x99,0x6c,0x65,0xb9,0x82,0xd9,0x49,0x59,0xd3,0x14,0x6f,0xe6,0xa0,0x20,0x69,0x30,0x90,0xbd,0xba,0x13,0x13,0x23,0x75,0xf2,0x24};
    struct {sc_int z,t,m,one,x1,x2,y1,y2,g1,g2,constant;} w;br_i31_zero(w.z,p[0]);w.z[1]=10;sc_neg(w.t,w.z,p);memcpy(w.z,w.t,sizeof(sc_int));
    sc_mul(w.t,u,u,p);sc_mul(w.m,w.z,w.t,p);memcpy(w.z,w.m,sizeof(sc_int));sc_mul(w.t,w.m,w.m,p);sc_modadd(w.m,w.m,w.t,p);
    br_i31_zero(w.one,p[0]);unsigned zero=sc_equal(w.m,w.one,sizeof(sc_int));w.one[1]=1;sc_pow(w.t,w.m,inv,p);sc_modadd(w.m,w.t,w.one,p);
    br_i31_decode_mod(w.constant,b3,32,p);sc_mul(w.x1,w.constant,w.m,p);br_i31_decode_mod(w.constant,b30,32,p);sc_select(w.x1,w.constant,w.x1,sizeof(sc_int),zero);
    sc_mul(w.x2,w.z,w.x1,p);sc_curve(w.g1,w.x1,p);sc_curve(w.g2,w.x2,p);unsigned qr=sc_root(w.y1,w.g1,p);(void)sc_root(w.y2,w.g2,p);
    sc_select(w.x1,w.x1,w.x2,sizeof(sc_int),qr);sc_select(w.y1,w.y1,w.y2,sizeof(sc_int),qr);sc_point(out,w.x1,w.y1,u[1]&1,p);sc_zero(&w,sizeof(w));
}
static int sc_h2e(const uint8_t addrs[12],const uint8_t *password,size_t n,const uint8_t *ssid,size_t sn,uint8_t out[65]){
    uint8_t seed[32],value[48],other[65],scalar[32],zero[32]={0};sc_int p,u,order,one;static const uint8_t unit[1]={1};int result=-1;
    br_i31_decode(p,sc_prime,32);gsa_crypto_hmac(NULL,ssid,sn,password,n,seed);sc_hkdf(seed,"SAE Hash to Element u1 P1",value);br_i31_decode_reduce(u,value,48,p);sc_swu(out,u,p);
    sc_hkdf(seed,"SAE Hash to Element u2 P2",value);br_i31_decode_reduce(u,value,48,p);sc_swu(other,u,p);
    if(!br_ec_p256_m31.muladd(out,other,65,unit,1,unit,1,23))goto done;
    gsa_crypto_hmac(NULL,zero,32,addrs,12,seed);br_i31_decode(order,sc_order,32);br_i31_zero(one,order[0]);one[1]=1;br_i31_sub(order,one,1);
    br_i31_decode_reduce(u,seed,32,order);br_i31_add(u,one,1);br_i31_encode(scalar,32,u);if(br_ec_p256_m31.mul(out,65,scalar,32,23))result=0;
done:sc_zero(seed,32);sc_zero(value,48);sc_zero(other,65);sc_zero(scalar,32);sc_zero(u,sizeof(u));if(result)sc_zero(out,65);return result;
}
int gsa_crypto_pwe(const uint8_t a[6],const uint8_t b[6],const uint8_t *password,size_t n,const uint8_t *ssid,size_t sn,unsigned h2e,uint8_t out[65]){
    if(!a||!b||!password||!n||n>64||!out||h2e>1||(h2e&&(!ssid||!sn||sn>32)))return -1;uint8_t addresses[12];sc_addresses(addresses,a,b);return h2e?sc_h2e(addresses,password,n,ssid,sn,out):sc_hunt(addresses,password,n,out);
}
int gsa_crypto_scalar(const uint8_t value[32]){if(!value)return 0;sc_int n,x;uint8_t low[32]={0};low[31]=1;br_i31_decode(n,sc_order,32);unsigned valid=br_i31_decode_mod(x,value,32,n);valid&=!sc_equal(value,low,32);low[31]=0;valid&=!sc_equal(value,low,32);sc_zero(x,sizeof(x));return (int)valid;}
int gsa_crypto_add(const uint8_t a[32],const uint8_t b[32],uint8_t out[32]){sc_int n,x,y,z;br_i31_decode(n,sc_order,32);unsigned ok=br_i31_decode_mod(x,a,32,n)&br_i31_decode_mod(y,b,32,n);sc_modadd(z,x,y,n);br_i31_encode(out,32,z);sc_zero(x,sizeof(x));sc_zero(y,sizeof(y));sc_zero(z,sizeof(z));return ok?0:-1;}
int gsa_crypto_commit(const uint8_t pwe[65],const uint8_t rand[32],const uint8_t mask[32],uint8_t scalar[32],uint8_t element[65]){
    if(!gsa_crypto_scalar(rand)||!gsa_crypto_scalar(mask)||gsa_crypto_add(rand,mask,scalar)||!gsa_crypto_scalar(scalar))return -1;
    memcpy(element,pwe,65);if(!br_ec_p256_m31.mul(element,65,mask,32,23))return -1;sc_int p,y,z;br_i31_decode(p,sc_prime,32);br_i31_decode_mod(y,element+33,32,p);sc_neg(z,y,p);br_i31_encode(element+33,32,z);sc_zero(y,sizeof(y));sc_zero(z,sizeof(z));return 0;
}
int gsa_crypto_shared(const uint8_t pwe[65],const uint8_t rand[32],const uint8_t peer_scalar[32],const uint8_t peer_element[65],uint8_t k[32]){
    static const uint8_t one[1]={1};uint8_t point[65];int result=-1;if(!gsa_crypto_scalar(rand)||!gsa_crypto_scalar(peer_scalar))return -1;
    memcpy(point,pwe,65);if(br_ec_p256_m31.muladd(point,peer_element,65,peer_scalar,32,one,1,23)&&br_ec_p256_m31.mul(point,65,rand,32,23)){memcpy(k,point+1,32);result=0;}sc_zero(point,65);if(result)sc_zero(k,32);return result;
}
