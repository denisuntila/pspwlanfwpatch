/*
 * Example kernel plugin: loads ms0:/seplugins/ssid_filter_off.wfp, applies it
 * and restarts the WLAN chip with it. Result in ms0:/pspwlanfwpatch.log.
 */
#include <pspkernel.h>
#include <stdio.h>
#include <string.h>
#include "pspwlanfwpatch.h"

PSP_MODULE_INFO("wfp_example", PSP_MODULE_KERNEL, 1, 0);

#define PATCH "ms0:/seplugins/ssid_filter_off.wfp"
#define LOG   "ms0:/pspwlanfwpatch.log"

static void log_line(const char *s)
{
    SceUID fd = sceIoOpen(LOG, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0777);
    if (fd >= 0) {
        sceIoWrite(fd, s, strlen(s));
        sceIoClose(fd);
    }
}

static int main_thread(SceSize args, void *argp)
{
    wfp_set_t s;
    char line[96];
    int r;

    sceKernelDelayThread(5 * 1000 * 1000);     /* let wlan.prx finish its start-up */
    r = wfp_set_load(&s, PATCH);
    if (r == 0)
        r = wfp_activate(&s);
    snprintf(line, sizeof(line), "%s: %s -> %d (state %d)\n", PATCH, s.name, r, wfp_state(&s, 1));
    log_line(line);
    wfp_set_free(&s);
    return 0;
}

int module_start(SceSize args, void *argp)
{
    SceUID th = sceKernelCreateThread("wfp_example", main_thread, 0x20, 0x2000, 0, NULL);
    if (th >= 0)
        sceKernelStartThread(th, 0, NULL);
    return 0;
}

int module_stop(SceSize args, void *argp)
{
    return 0;
}
