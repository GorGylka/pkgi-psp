#pragma once
#include <openssl/aes.h>
#include <string.h>
typedef AES_KEY mbedtls_aes_context;
#define MBEDTLS_AES_ENCRYPT 1
static inline void mbedtls_aes_init(mbedtls_aes_context* c) { memset(c,0,sizeof(*c)); }
static inline void mbedtls_aes_free(mbedtls_aes_context* c) { memset(c,0,sizeof(*c)); }
static inline int mbedtls_aes_setkey_enc(mbedtls_aes_context*c,const unsigned char*k,unsigned bits) { return AES_set_encrypt_key(k,bits,c); }
static inline int mbedtls_aes_crypt_ecb(mbedtls_aes_context*c,int mode,const unsigned char*in,unsigned char*out) { (void)mode; AES_encrypt(in,out,c);return 0; }
