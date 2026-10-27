#include "pkgi_install_profile.h"
#include "pkgi.h"
#include <stdio.h>
#include <string.h>
#ifdef PKGI_PROFILE_HOST
#include <time.h>
#else
#include <pspkernel.h>
#endif
static uint64_t times[PROF_COUNT], counts[PROF_COUNT], started, bytes;
static int active;
uint64_t pkgi_profile_now(void) {
#ifdef PKGI_PROFILE_HOST
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
#else
    return (uint64_t)sceKernelGetSystemTimeWide();
#endif
}
void pkgi_profile_begin(void) {
    memset(times, 0, sizeof(times)); memset(counts, 0, sizeof(counts));
    bytes = 0; active = 1; started = pkgi_profile_now();
}
void pkgi_profile_add(int stage, uint64_t start) {
    if (active && stage >= 0 && stage < PROF_COUNT) {
        times[stage] += pkgi_profile_now() - start; ++counts[stage];
    }
}
void pkgi_profile_bytes(unsigned n) { bytes += n; }
void pkgi_profile_finish(int ok, int me) {
    uint64_t elapsed = pkgi_profile_now() - started;
    active = 0;
#ifdef PKGI_ME_PAIR_PROTOTYPE
    const char* path = "pkgi-install-me-profile.txt";
#else
    const char* path = "pkgi-install-cpu-profile.txt";
#endif
    FILE* f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "PKGi install profile v1\nbuild: %s\ninstall_ok: %d\nME_ENABLED: %d\npayload_bytes: %llu\nsession_us: %llu\n", PKGI_VERSION, ok, me,
        (unsigned long long)bytes, (unsigned long long)elapsed);
    static const char* labels[PROF_COUNT] = {
        "payload_read", "decrypt_total", "cpu_crypto", "me_submit_copy_cache",
        "me_join_total", "me_wait_only", "me_copy_back_cache", "payload_write",
        "progress_ui", "me_start_selftests", "me_shutdown" };
    for (int i = 0; i < PROF_COUNT; ++i)
        fprintf(f, "%s_us: %llu; calls: %llu\n", labels[i],
            (unsigned long long)times[i], (unsigned long long)counts[i]);
    uint64_t measured = times[PROF_READ] + times[PROF_DECRYPT] + times[PROF_WRITE]
        + times[PROF_UI] + times[PROF_START] + times[PROF_STOP];
    fprintf(f, "other_us: %llu\n", (unsigned long long)(elapsed > measured ? elapsed - measured : 0));
    fprintf(f, "Nested: decrypt_total includes cpu_crypto + me_submit_copy_cache + me_join_total.\n"
        "me_join_total includes me_wait_only + me_copy_back_cache. Do not sum nested values.\n"
        "ME wait is CPU residual wait, not total ME computation time.\n"
        "Session starts after metadata table parsing; includes ME startup/selftests and cleanup.\n"
        "Read/write measure payload calls only; other includes filenames, file open/close and directory operations.\n"
        "Write time is API latency, not guaranteed physical media flush time.\n");
    fclose(f);
}
