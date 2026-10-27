#ifndef PKGI_PAIR_CRYPTO_H
#define PKGI_PAIR_CRYPTO_H
#include <stdint.h>
#include <stddef.h>
#include <mbedtls/aes.h>
#define PKGI_ME_BLOCK_SIZE (256u * 1024u)
#ifdef __cplusplus
extern "C" {
#endif
/* Pure computation. No allocation, file I/O, PSP syscalls or shared mutable state. */
int pkgi_ctr_xor(mbedtls_aes_context* ctx, const uint8_t iv[16],
                 uint64_t block_offset, uint8_t* data, size_t length);
int pkgi_decrypt_pair(mbedtls_aes_context* ctx, const uint8_t iv[16],
                      uint64_t block_offset, uint8_t* data, size_t length, int use_me);
/* begin returns 1 only after the hardware worker's output passes self-tests. */
int pkgi_me_begin(const uint8_t key[16]);
void pkgi_me_end(void);
int pkgi_me_submit(const uint8_t* data, size_t length, const uint8_t iv[16], uint64_t offset);
int pkgi_me_wait(uint8_t* output, size_t length);
#ifdef __cplusplus
}
#endif
#endif
