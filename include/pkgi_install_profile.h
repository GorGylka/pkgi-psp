#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
enum { PROF_READ, PROF_DECRYPT, PROF_CPU, PROF_SUBMIT, PROF_JOIN,
       PROF_WAIT, PROF_COPY_BACK, PROF_WRITE, PROF_UI, PROF_START, PROF_STOP, PROF_COUNT };
uint64_t pkgi_profile_now(void);
void pkgi_profile_begin(void);
void pkgi_profile_add(int stage, uint64_t start);
void pkgi_profile_bytes(unsigned bytes);
void pkgi_profile_finish(int ok, int me);
#ifdef __cplusplus
}
#endif
