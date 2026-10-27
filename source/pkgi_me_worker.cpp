// PKGi integration for mcidclan's MIT-licensed Media Engine Custom Core.
// Library source/license and pinned revision are shipped separately in vendor/me-core.
#include "pkgi_pair_crypto.h"
#include "pkgi_install_profile.h"
#include <pspkernel.h>
#include <psppower.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#define DISABLE_AVC_CONFIG
#include <me-core-mapper/me-core.h>

#if defined(MBEDTLS_AES_ALT) || defined(MBEDTLS_AES_ENCRYPT_ALT)
#error "ME prototype requires the software mbedTLS AES backend"
#endif

enum { BOOT = 0, READY, JOB, DONE, STOP, STOPPED };
struct alignas(64) Control {
    uint32_t state, length, offset_lo, offset_hi;
    uint8_t iv[16];
    int32_t result;
};
static Control control;
struct alignas(64) KeyBox { mbedtls_aes_context ctx; };
static KeyBox worker_key;
static uint8_t worker_buffer[PKGI_ME_BLOCK_SIZE] __attribute__((aligned(64)));
static uint8_t reference_buffer[PKGI_ME_BLOCK_SIZE] __attribute__((aligned(64)));
static int library_started, running, disabled, power_locked;
static unsigned jobs;
static uint32_t session_start;

static inline volatile Control* shared()
{
    return reinterpret_cast<volatile Control*>(reinterpret_cast<uintptr_t>(&control) | UNCACHED_USER_MASK);
}
static inline void barrier() { asm volatile("sync" ::: "memory"); }
static void log_value(const char* label, int value, const char* mode = "a")
{
    FILE* f = fopen("pkgi-me-log.txt", mode);
    if (f) { fprintf(f, "%s: %d (0x%08X)\n", label, value, (unsigned)value); fclose(f); }
}

// Executed in kernel mode through the library's bridge. Same reset mask as meLibReset.
static int hold_me_reset()
{
    HW_SYS_RESET_ENABLE = SC_HW_RESET;
    barrier();
    return 0;
}
static int restart_me()
{
    meLibReset();
    return 0;
}
static void stop_after_failure()
{
    disabled = 1;
    if (library_started) log_value("hold ME reset", kcall(hold_me_reset, CACHED_KERNEL_MASK));
    running = 0;
}
static int wait_state(unsigned wanted, unsigned timeout_us)
{
    const uint32_t start = sceKernelGetSystemTimeLow();
    while (shared()->state != wanted) {
        if ((uint32_t)(sceKernelGetSystemTimeLow() - start) >= timeout_us) {
            log_value("timeout, worker state", shared()->state);
            stop_after_failure();
            return 0;
        }
        sceKernelDelayThread(100);
    }
    barrier();
    return 1;
}

__attribute__((noinline, aligned(4)))
void meLibOnProcess(void)
{
    // No libc allocation, PSP syscalls, UI, files or key setup on the ME.
    meLibDcacheInvalidateRange((u32)&worker_key, sizeof(worker_key));
    barrier();
    shared()->state = READY;
    barrier();
    for (;;) {
        unsigned state = shared()->state;
        if (state == STOP) {
            barrier();
            shared()->state = STOPPED;
            barrier();
            for (;;) meLibHalt();
        }
        if (state != JOB) { asm volatile("nop"); continue; }
        barrier();
        const size_t size = shared()->length;
        uint8_t iv[16];
        for (int i = 0; i < 16; ++i) iv[i] = shared()->iv[i];
        uint64_t offset = ((uint64_t)shared()->offset_hi << 32) | shared()->offset_lo;
        int ok = 0;
        if (size && size <= PKGI_ME_BLOCK_SIZE) {
            unsigned aligned_size = (size + 63u) & ~63u;
            meLibDcacheInvalidateRange((u32)worker_buffer, aligned_size);
            barrier();
            ok = pkgi_ctr_xor(&worker_key.ctx, iv, offset, worker_buffer, size);
            meLibDcacheWritebackInvalidateRange((u32)worker_buffer, aligned_size);
            barrier();
        }
        shared()->result = ok;
        barrier();
        shared()->state = DONE;
        barrier();
    }
}

extern "C" int pkgi_me_submit(const uint8_t* data, size_t size, const uint8_t iv[16], uint64_t offset)
{
    if (!running || disabled || shared()->state != READY || !size || size > PKGI_ME_BLOCK_SIZE) return 0;
    memcpy(worker_buffer, data, size);
    sceKernelDcacheWritebackInvalidateRange(worker_buffer, (size + 63u) & ~63u);
    for (int i = 0; i < 16; ++i) shared()->iv[i] = iv[i];
    shared()->length = size;
    shared()->offset_lo = (uint32_t)offset;
    shared()->offset_hi = (uint32_t)(offset >> 32);
    shared()->result = 0;
    barrier();
    shared()->state = JOB;
    barrier();
    ++jobs;
    return 1;
}
extern "C" int pkgi_me_wait(uint8_t* output, size_t size)
{
    uint64_t tick = pkgi_profile_now();
    int ready = running && wait_state(DONE, 10000000);
    pkgi_profile_add(PROF_WAIT, tick);
    if (!ready) return 0;
    if (!shared()->result || size != shared()->length) {
        log_value("ME crypto failed", shared()->result);
        stop_after_failure();
        return 0;
    }
    tick = pkgi_profile_now();
    sceKernelDcacheInvalidateRange(worker_buffer, (size + 63u) & ~63u);
    memcpy(output, worker_buffer, size);
    pkgi_profile_add(PROF_COPY_BACK, tick);
    barrier();
    shared()->state = READY;
    barrier();
    return 1;
}

extern "C" void pkgi_me_end(void)
{
    if (running) {
        // There must be no in-flight job when the installer's pair loop returns.
        shared()->state = STOP;
        barrier();
        if (wait_state(STOPPED, 2000000)) {
            log_value("ME stopped", 1);
            // Keep the halted worker in reset until the next explicit start.
            log_value("hold ME reset", kcall(hold_me_reset, CACHED_KERNEL_MASK));
        }
        running = 0;
    }
    if (power_locked) { scePowerUnlock(0); power_locked = 0; }
    log_value("ME jobs (includes self-test)", jobs);
    log_value("session elapsed ms", (sceKernelGetSystemTimeLow() - session_start) / 1000);
}

extern "C" int pkgi_me_begin(const uint8_t key[16])
{
    log_value("PKGi CPU+ME pair prototype v1", 1, "w");
    session_start = sceKernelGetSystemTimeLow();
    jobs = 0;
    if (disabled) { log_value("CPU fallback: ME disabled for this run", 1); return 0; }
    // Prevent normal suspend while this prototype owns ME; restore at install end.
    int rc = scePowerLock(0);
    log_value("scePowerLock", rc);
    if (rc < 0) return 0;
    power_locked = 1;
    mbedtls_aes_init(&worker_key.ctx);
    rc = mbedtls_aes_setkey_enc(&worker_key.ctx, key, 128);
    if (rc) { log_value("AES setup failed", rc); pkgi_me_end(); return 0; }
    // Warm lazy software AES tables on SC before publishing code/data to ME.
    uint8_t warm[16] = {0};
    rc = mbedtls_aes_crypt_ecb(&worker_key.ctx, MBEDTLS_AES_ENCRYPT, warm, warm);
    if (rc) { pkgi_me_end(); return 0; }
    memset(&control, 0, sizeof(control));
    sceKernelDcacheWritebackInvalidateAll();
    sceKernelIcacheInvalidateAll();
    if (!library_started) {
        rc = meLibDefaultInit();
        log_value("meLibDefaultInit table/result", rc);
        if (rc != ME_CORE_T2_IMG_TABLE && rc != ME_CORE_IMG_TABLE) {
            disabled = 1;
            pkgi_me_end();
            log_value("CPU fallback: unsupported core/init failure", rc);
            return 0;
        }
        library_started = 1;
    } else {
        rc = kcall(restart_me, CACHED_KERNEL_MASK);
        log_value("ME restart", rc);
        if (rc < 0) { stop_after_failure(); pkgi_me_end(); return 0; }
    }
    running = 1;
    if (!wait_state(READY, 3000000)) { pkgi_me_end(); return 0; }
    // Compare hardware output to SC, covering full/partial blocks and counter carry.
    static const size_t sizes[] = {16, 31, 4096, PKGI_ME_BLOCK_SIZE};
    static const uint64_t offsets[] = {0, 255, 65535, 0x0fffffffu};
    mbedtls_aes_context reference_key;
    mbedtls_aes_init(&reference_key);
    int ok = mbedtls_aes_setkey_enc(&reference_key, key, 128) == 0;
    for (unsigned t = 0; ok && t < 4; ++t) {
        uint8_t iv[16];
        for (unsigned i = 0; i < 16; ++i) iv[i] = t == 3 ? 255 : (uint8_t)(i * 17 + t);
        for (size_t i = 0; i < sizes[t]; ++i) reference_buffer[i] = (uint8_t)(i * 29 + t);
        ok = pkgi_me_submit(reference_buffer, sizes[t], iv, offsets[t]);
        if (ok) ok = pkgi_ctr_xor(&reference_key, iv, offsets[t], reference_buffer, sizes[t]);
        if (ok) ok = wait_state(DONE, 10000000);
        if (ok) {
            sceKernelDcacheInvalidateRange(worker_buffer, (sizes[t] + 63u) & ~63u);
            ok = shared()->result && memcmp(reference_buffer, worker_buffer, sizes[t]) == 0;
            shared()->state = READY;
            barrier();
        }
        log_value("self-test case", t);
        log_value("self-test pass", ok);
    }
    mbedtls_aes_free(&reference_key);
    if (!ok) { stop_after_failure(); pkgi_me_end(); log_value("CPU fallback: self-test failed", 1); return 0; }
    log_value("ME_ENABLED", 1);
    return 1;
}
