#include "pkgi_pair_crypto.h"
#include "pkgi_install_profile.h"

int pkgi_ctr_xor(mbedtls_aes_context* ctx, const uint8_t iv[16],
                 uint64_t block_offset, uint8_t* data, size_t length)
{
    uint8_t counter[16], stream[16];
    for (int i = 0; i < 16; ++i) counter[i] = iv[i];
    for (int i = 15; i >= 0 && block_offset; --i) {
        unsigned sum = counter[i] + (unsigned)(block_offset & 255u);
        counter[i] = (uint8_t)sum;
        block_offset = (block_offset >> 8) + (sum >> 8);
    }
    while (length) {
        if (mbedtls_aes_crypt_ecb(ctx, MBEDTLS_AES_ENCRYPT, counter, stream)) return 0;
        size_t n = length < 16 ? length : 16;
        for (size_t i = 0; i < n; ++i) data[i] ^= stream[i];
        data += n;
        length -= n;
        for (int i = 15; i >= 0; --i) if (++counter[i]) break;
    }
    return 1;
}

int pkgi_decrypt_pair(mbedtls_aes_context* ctx, const uint8_t iv[16],
                      uint64_t offset, uint8_t* data, size_t length, int use_me)
{
    if (length > 2u * PKGI_ME_BLOCK_SIZE) return 0;
    size_t first = length < PKGI_ME_BLOCK_SIZE ? length : PKGI_ME_BLOCK_SIZE;
    size_t second = length - first;
    uint64_t tick = pkgi_profile_now();
    if (!use_me || !second) {
        int ok = pkgi_ctr_xor(ctx, iv, offset, data, length);
        pkgi_profile_add(PROF_CPU, tick);
        return ok;
    }
    int submitted = pkgi_me_submit(data + first, second, iv, offset + first / 16);
    pkgi_profile_add(PROF_SUBMIT, tick);
    if (!submitted) return 0;
    tick = pkgi_profile_now();
    int cpu_ok = pkgi_ctr_xor(ctx, iv, offset, data, first);
    pkgi_profile_add(PROF_CPU, tick);
    tick = pkgi_profile_now();
    // Always join the worker, even if CPU crypto failed. Never publish half a pair.
    int me_ok = pkgi_me_wait(data + first, second);
    pkgi_profile_add(PROF_JOIN, tick);
    return cpu_ok && me_ok;
}
