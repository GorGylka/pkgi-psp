#include <pspkernel.h>
#include <pspctrl.h>
#include <pspsdk.h>
#include <pspintrman.h>
PSP_MODULE_INFO("pkgi_home", 0x1006, 1, 0);
PSP_NO_CREATE_MAIN_THREAD();
static SceUID thread_id = -1;
static volatile unsigned events;
static volatile int stop;
static int poll_thread(SceSize args, void* argp)
{
    unsigned previous = 0;
    SceCtrlData pad;
    if (sceCtrlPeekBufferPositive(&pad, 1) > 0) previous = pad.Buttons;
    while (!stop) {
        if (sceCtrlPeekBufferPositive(&pad, 1) > 0) {
            if ((pad.Buttons & PSP_CTRL_HOME) && !(previous & PSP_CTRL_HOME)) {
                int intr = sceKernelCpuSuspendIntr();
                ++events;
                sceKernelCpuResumeIntr(intr);
            }
            previous = pad.Buttons;
        }
        sceKernelDelayThread(10000);
    }
    return 0;
}
unsigned pkgiHomePoll(void)
{
    int k1 = pspSdkSetK1(0);
    int intr = sceKernelCpuSuspendIntr();
    unsigned value = events;
    events = 0;
    sceKernelCpuResumeIntr(intr);
    pspSdkSetK1(k1);
    return value;
}
int module_start(SceSize args, void* argp)
{
    thread_id = sceKernelCreateThread("pkgi_home_poll", poll_thread, 0x18, 0x1000, 0, NULL);
    if (thread_id < 0) return thread_id;
    int rc = sceKernelStartThread(thread_id, 0, NULL);
    if (rc < 0) { sceKernelDeleteThread(thread_id); thread_id = -1; }
    return rc;
}
int module_stop(SceSize args, void* argp)
{
    stop = 1;
    if (thread_id >= 0) {
        SceUInt timeout = 100000;
        if (sceKernelWaitThreadEnd(thread_id, &timeout) < 0) sceKernelTerminateThread(thread_id);
        sceKernelDeleteThread(thread_id);
    }
    return 0;
}
