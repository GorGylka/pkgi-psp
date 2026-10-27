#include "pkgi_native_http.h"
#include "pkgi.h"
#include <pspkernel.h>
#include <psphttp.h>
#include <psppower.h>
#include <psputility_netmodules.h>
#include <stdlib.h>
#include <stdio.h>

#define READ_SIZE (100u * 1024u)
static const int modules[] = {
    PSP_NET_MODULE_PARSEURI, PSP_NET_MODULE_PARSEHTTP, PSP_NET_MODULE_HTTP
};
static unsigned int loaded;
static int initialized;
static void http_trace(const char* step, int code, const char* mode);

void pkgi_native_http_end(void)
{
    if (initialized) sceHttpEnd();
    initialized = 0;
    while (loaded) sceUtilityUnloadNetModule(modules[--loaded]);
}

int pkgi_native_http_init(void)
{
    if (initialized) return 1;
    static const char* names[] = {"Load PARSEURI", "Load PARSEHTTP", "Load HTTP"};
    int rc;
    for (loaded = 0; loaded < sizeof(modules) / sizeof(modules[0]); ++loaded)
    {
        http_trace("Max free block bytes", (int)sceKernelMaxFreeMemSize(), "a");
        http_trace("Total free bytes", (int)sceKernelTotalFreeMemSize(), "a");
        rc = sceUtilityLoadNetModule(modules[loaded]);
        http_trace(names[loaded], rc, "a");
        if (rc < 0) goto fail;
    }
    rc = sceHttpInit(128u * 1024u);
    http_trace("sceHttpInit", rc, "a");
    if (rc < 0) goto fail;
    initialized = 1;
    return 1;
fail:
    pkgi_native_http_end();
    return 0;
}

void pkgi_native_http_close(pkgi_native_http* h)
{
    if (h->request_id >= 0) sceHttpDeleteRequest(h->request_id);
    if (h->connection_id >= 0) sceHttpDeleteConnection(h->connection_id);
    if (h->template_id >= 0) sceHttpDeleteTemplate(h->template_id);
    h->request_id = h->connection_id = h->template_id = -1;
}

// No URLs or credentials are written to this diagnostic log.
static void http_trace(const char* step, int code, const char* mode)
{
    FILE* f = fopen("scehttp-debug.txt", mode);
    if (f)
    {
        fprintf(f, "%s: 0x%08X (%d)\n", step, (unsigned int)code, code);
        fclose(f);
    }
}

int pkgi_native_http_open(pkgi_native_http* h, const char* url)
{
    int status, rc;
    SceULong64 length;
    h->request_id = h->connection_id = h->template_id = -1;
    h->length = -1;
    http_trace("initialized", initialized, "a");
    if (!initialized) return 0;
#define HTTP_STEP(label, expression) do { \
    rc = (expression); \
    http_trace(label, rc, "a"); \
    if (rc < 0) goto fail; \
} while (0)
    HTTP_STEP("CreateTemplate", h->template_id = sceHttpCreateTemplate("Mozilla/5.0 (PLAYSTATION PORTABLE; 1.00)", 1, 1));
    HTTP_STEP("ResolveTimeout", sceHttpSetResolveTimeOut(h->template_id, 3000000));
    HTTP_STEP("ConnectTimeout", sceHttpSetConnectTimeOut(h->template_id, 10000000));
    HTTP_STEP("SendTimeout", sceHttpSetSendTimeOut(h->template_id, 60000000));
    HTTP_STEP("ReceiveTimeout", sceHttpSetRecvTimeOut(h->template_id, 10000000));
    HTTP_STEP("EnableRedirect", sceHttpEnableRedirect(h->template_id));
    HTTP_STEP("CreateConnection", h->connection_id = sceHttpCreateConnectionWithURL(h->template_id, (char*)url, 0));
    HTTP_STEP("CreateRequest", h->request_id = sceHttpCreateRequestWithURL(h->connection_id, PSP_HTTP_METHOD_GET, (char*)url, 0));
    HTTP_STEP("SendRequest", sceHttpSendRequest(h->request_id, NULL, 0));
    HTTP_STEP("GetStatusCode", sceHttpGetStatusCode(h->request_id, &status));
    http_trace("HTTP status", status, "a");
    if (status != 200) goto fail;
    rc = sceHttpGetContentLength(h->request_id, &length);
    http_trace("GetContentLength", rc, "a");
    if (rc >= 0)
    {
        if (length > INT64_MAX) goto fail;
        h->length = (int64_t)length;
    }
#undef HTTP_STEP
    return 1;
fail:
    pkgi_native_http_close(h);
    return 0;
}

int pkgi_native_http_read(pkgi_native_http* h, pkgi_http_writer write,
                          void* userdata, pkgi_http_progress progress)
{
    unsigned char* buffer = malloc(READ_SIZE);
    int64_t received = 0;
    int success = 0;
    if (!buffer || !write) { free(buffer); return 0; }
    for (;;)
    {
        if (pkgi_exit_requested()) break;
        if (progress && progress(NULL, h->length < 0 ? 0 : h->length, received, 0, 0)) break;
        int count = sceHttpReadData(h->request_id, buffer, READ_SIZE);
        if (count < 0) break;
        if (!count)
        {
            success = h->length < 0 || received == h->length;
            break;
        }
        if (received > INT64_MAX - count ||
            (h->length >= 0 && count > h->length - received)) break;
        if (write(buffer, 1, count, userdata) != (size_t)count) break;
        received += count;
        scePowerTick(0);
    }
    free(buffer);
    return success;
}
