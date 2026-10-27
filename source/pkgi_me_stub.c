#include "pkgi_pair_crypto.h"
int pkgi_me_begin(const uint8_t key[16]) { (void)key; return 0; }
void pkgi_me_end(void) {}
int pkgi_me_submit(const uint8_t* p, size_t n, const uint8_t iv[16], uint64_t o)
{ (void)p; (void)n; (void)iv; (void)o; return 0; }
int pkgi_me_wait(uint8_t* p, size_t n) { (void)p; (void)n; return 0; }
