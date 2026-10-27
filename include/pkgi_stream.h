#pragma once
#include <stddef.h>
#include <stdint.h>
#include "pkgi_db.h"
/* Reader returns bytes, 0 on EOF, -1 on error/cancellation. finish verifies transport EOF. */
typedef struct {
    void* ctx;
    int (*read)(void*, void*, unsigned);
    int (*finish)(void*);
    int (*cancelled)(void*);
    void (*progress)(void*, uint64_t);
    uint64_t length;
    const char* root;
    char error[160];
    uint64_t me_bytes,cpu_bytes,payload_written;
    unsigned me_jobs;
} pkgi_stream_source;
int pkgi_stream_unpack(pkgi_stream_source* source);
int pkgi_stream_download(const DbItem* item);
