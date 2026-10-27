#include "pkgi_home.h"
#include <pspkernel.h>
#include <pspsdk.h>
#include <pspimpose.h>
#include <stdio.h>
extern const unsigned char _binary_pkgi_home_prx_start[], _binary_pkgi_home_prx_end[];
extern unsigned pkgiHomePoll(void);
static SceUID module_id = -1;
static int old_popup = -1;
void pkgi_home_init(void)
{
    const char* path = "pkgi_home.prx";
    unsigned size = _binary_pkgi_home_prx_end - _binary_pkgi_home_prx_start;
    SceUID fd = sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    int rc = fd;
    if (fd >= 0) {
        rc = sceIoWrite(fd, _binary_pkgi_home_prx_start, size);
        sceIoClose(fd);
        if (rc == (int)size) {
            module_id = pspSdkLoadStartModule(path, PSP_MEMORY_PARTITION_KERNEL);
            rc = module_id;
        }
    }
    if (module_id >= 0) {
        old_popup = sceImposeGetHomePopup();
        rc = old_popup < 0 ? old_popup : sceImposeSetHomePopup(0);
        if (rc < 0) pkgi_home_end();
    }
    FILE* f = fopen("pkgi-home-log.txt", "w");
    if (f) {
        fprintf(f, "home module: %d\nhome init result: %d\nprevious popup: %d\n", module_id, rc, old_popup);
        fclose(f);
    }
}
unsigned pkgi_home_poll(void) { return module_id >= 0 ? pkgiHomePoll() : 0; }
void pkgi_home_end(void)
{
    if (module_id >= 0) {
        int status;
        int rc = sceKernelStopModule(module_id, 0, NULL, &status, NULL);
        if (rc >= 0) sceKernelUnloadModule(module_id);
        module_id = -1;
    }
    if (old_popup >= 0) { sceImposeSetHomePopup(old_popup); old_popup = -1; }
}
