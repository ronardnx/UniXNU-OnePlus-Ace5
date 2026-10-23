/* Only the XNU SHA1/AES provider ABI is replaced. Production HMAC, unwrap
 * and AES-CMAC algorithms execute unchanged; SAE uses its pinned C provider. */
#include <openssl/sha.h>
#include <openssl/aes.h>
typedef SHA_CTX SHA1_CTX;
typedef AES_KEY aes_decrypt_ctx;
typedef AES_KEY aes_encrypt_ctx;
static void SHA1Init(SHA1_CTX *s){assert(SHA1_Init(s));}
static void SHA1Update(SHA1_CTX *s,const void *p,size_t n){assert(SHA1_Update(s,p,n));}
static void SHA1Final(void *p,SHA1_CTX *s){assert(SHA1_Final((unsigned char *)p,s));}
static int aes_decrypt_key(const unsigned char *p,int n,aes_decrypt_ctx *s){return AES_set_decrypt_key(p,8*n,s);}
static int aes_decrypt_cbc(const unsigned char *in,const unsigned char *iv,unsigned blocks,unsigned char *out,aes_decrypt_ctx *s){
    unsigned char copy[16];memcpy(copy,iv,16);AES_cbc_encrypt(in,out,16*blocks,s,copy,AES_DECRYPT);return 0;
}
static int aes_encrypt_key(const unsigned char *p,int n,aes_encrypt_ctx *s){return AES_set_encrypt_key(p,8*n,s);}
static int aes_encrypt_cbc(const unsigned char *in,const unsigned char *iv,unsigned blocks,unsigned char *out,aes_encrypt_ctx *s){
    unsigned char copy[16];memcpy(copy,iv,16);AES_cbc_encrypt(in,out,16*blocks,s,copy,AES_ENCRYPT);return 0;
}
#define GWR_CRYPTO_HOST 1
#include "../ios16/giulia_wifi_rsn_crypto.h"
#define GPM_CRYPTO_HOST 1
#include "../ios16/giulia_wifi_pmf_crypto.h"
