#ifndef PKGI_NATIVE_HTTP_H
#define PKGI_NATIVE_HTTP_H
#include <stdint.h>
#include <stddef.h>

typedef struct {
    int template_id, connection_id, request_id;
    int64_t length;
} pkgi_native_http;
typedef size_t (*pkgi_http_writer)(void*, size_t, size_t, void*);
typedef int (*pkgi_http_progress)(void*, int64_t, int64_t, int64_t, int64_t);
int pkgi_native_http_init(void);
void pkgi_native_http_end(void);
int pkgi_native_http_open(pkgi_native_http* h, const char* url);
int pkgi_native_http_read(pkgi_native_http* h, pkgi_http_writer write,
                          void* userdata, pkgi_http_progress progress);
void pkgi_native_http_close(pkgi_native_http* h);
#endif
