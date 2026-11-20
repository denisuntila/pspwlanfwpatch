/*
 * pspwlanfwpatch - see pspwlanfwpatch.h
 *
 * wlanfirm_02g.prx registers voyager.bin with wlan.prx at boot, and wlan.prx
 * sends that RAM image to the chip at every power-on. So: patch the image in
 * RAM, then power-cycle the chip.
 */
#include <pspkernel.h>
#include <pspsysmem_kernel.h>
#include <psploadcore.h>
#include <string.h>
#include "pspwlanfwpatch.h"

/* wlan.prx (sceWlan_Driver, system software 6.61), module-relative */
#define WLAN_OFF_FWREG   0x19394   /* 4 words: registered firmware image pointers */
#define WLAN_OFF_SWCB    0x29DC    /* switch callback sub_29DC(on, ctx) */
#define WLAN_OFF_GETSW   0x3C58    /* sceWlanGetSwitchState (to find the context pointer) */
#define WLAN_OFF_CTXPTR  0x192A0   /* driver context pointer */
#define CTX_FLAGS        0x000     /* bit 0x2000: driver ignores the switch callback */
#define RELOAD_WAIT_US   (3000 * 1000)

#define HDR_SIZE 64
#define RD16(p) ((u32)(p)[0] | (u32)(p)[1] << 8)
#define RD32(p) (RD16(p) | RD16((p) + 2) << 16)

/* CRC-32 as zlib's crc32() */
static u32 crc32(const u8 *p, u32 n, u32 c)
{
    int k;
    c = ~c;
    while (n--) {
        c ^= *p++;
        for (k = 0; k < 8; k++)
            c = c & 1 ? (c >> 1) ^ 0xEDB88320 : c >> 1;
    }
    return ~c;
}

/* ---- patch sets ------------------------------------------------------- */

int wfp_set_parse(wfp_set_t *s, const void *buf, u32 len)
{
    const u8 *b = buf, *r;
    u32 i, plen, n, fcrc, crc;
    static const u8 zero[4];

    if (s == NULL || b == NULL)
        return WFP_E_ARG;
    memset(s, 0, sizeof(*s));
    if (len < HDR_SIZE || memcmp(b, "WFP1", 4) != 0 || RD16(b + 4) != 1)
        return WFP_E_FORMAT;
    plen = RD32(b + 56);
    if (plen != len - HDR_SIZE)
        return WFP_E_FORMAT;
    fcrc = RD32(b + 60);                        /* CRC of the file with this field = 0 */
    crc = crc32(b, 60, 0);
    crc = crc32(zero, 4, crc);
    crc = crc32(b + 64, plen, crc);
    if (crc != fcrc)
        return WFP_E_CRC;

    memcpy(s->name, b + 8, 31);
    s->target_size = RD32(b + 40);
    s->target_crc = RD32(b + 44);
    s->result_crc = RD32(b + 48);
    s->nruns = RD32(b + 52);
    s->runs = b + HDR_SIZE;
    s->runs_len = plen;
    for (i = 0, r = s->runs; i < s->nruns; i++) {   /* every run inside the file and the image */
        if (r + 8 > s->runs + plen)
            return WFP_E_FORMAT;
        n = RD16(r + 4);
        if (RD32(r) + n > s->target_size || r + 8 + 2 * n > s->runs + plen)
            return WFP_E_FORMAT;
        r += (8 + 2 * n + 3) & ~3;
    }
    return r == s->runs + plen ? 0 : WFP_E_FORMAT;
}

int wfp_set_load(wfp_set_t *s, const char *path)
{
    SceUID fd, blk;
    SceOff size;
    void *mem;
    int r;

    if (s == NULL || path == NULL)
        return WFP_E_ARG;
    memset(s, 0, sizeof(*s));
    if ((fd = sceIoOpen(path, PSP_O_RDONLY, 0)) < 0)
        return WFP_E_IO;
    size = sceIoLseek(fd, 0, PSP_SEEK_END);
    sceIoLseek(fd, 0, PSP_SEEK_SET);
    if (size < HDR_SIZE || size > 256 * 1024) {
        sceIoClose(fd);
        return WFP_E_FORMAT;
    }
    blk = sceKernelAllocPartitionMemory(1, "wfp", PSP_SMEM_Low, (SceSize)size, NULL);
    if (blk < 0) {
        sceIoClose(fd);
        return WFP_E_NOMEM;
    }
    mem = sceKernelGetBlockHeadAddr(blk);
    r = sceIoRead(fd, mem, (SceSize)size);
    sceIoClose(fd);
    if (r != size) {
        sceKernelFreePartitionMemory(blk);
        return WFP_E_IO;
    }
    if ((r = wfp_set_parse(s, mem, (u32)size)) < 0) {
        sceKernelFreePartitionMemory(blk);
        return r;
    }
    s->mem = (void *)blk;
    return 0;
}

void wfp_set_free(wfp_set_t *s)
{
    if (s && s->mem)
        sceKernelFreePartitionMemory((SceUID)s->mem);
    if (s)
        memset(s, 0, sizeof(*s));
}

/* ---- firmware image ------------------------------------------------------ */

/* size of the image at a (blocks walked up to the end block), 0 if it is none */
static u32 image_size(u32 a)
{
    u32 o = 0, n;

    if ((a & 3) || a < 0x88000000 || a >= 0x8A000000)
        return 0;
    for (n = 0; n < 1024; n++) {
        u32 cmd, len;
        if (a + o + 16 > 0x8A000000)
            return 0;
        cmd = _lw(a + o);
        len = _lw(a + o + 8);
        if (cmd == 4)
            return o + 16;
        if (cmd != 1 || len < 4 || len > 0x10000)
            return 0;
        o += 16 + len;
    }
    return 0;
}

int wfp_copies(u32 *addr, int max, u32 *size)
{
    SceModule *m = sceKernelFindModuleByName("sceWlan_Driver");
    int i, j, n = 0;

    if (m == NULL)
        return WFP_E_NOIMAGE;
    for (i = 0; i < 4; i++) {
        u32 a = _lw(m->text_addr + WLAN_OFF_FWREG + 4 * i), sz = image_size(a);
        if (sz == 0)
            continue;
        for (j = 0; j < n && addr[j] != a; j++)
            ;
        if (j == n && n < max) {
            addr[n++] = a;
            if (size)
                *size = sz;
        }
    }
    return n > 0 ? n : WFP_E_NOIMAGE;
}

int wfp_state(const wfp_set_t *sets, int nsets)
{
    u32 a[WFP_MAX_COPIES], size;
    int n = wfp_copies(a, WFP_MAX_COPIES, &size), c, i, st = WFP_E_UNKNOWN;

    if (n < 0)
        return n;
    for (c = 0; c < n; c++) {
        u32 crc = crc32((const u8 *)a[c], size, 0);
        int s = WFP_E_UNKNOWN;
        for (i = 0; i < nsets && s == WFP_E_UNKNOWN; i++) {
            if (sets[i].target_size != size)
                continue;
            if (crc == sets[i].result_crc)
                s = i;
            else if (crc == sets[i].target_crc)
                s = WFP_ORIGINAL;
        }
        if (c > 0 && s != st)
            return WFP_E_UNKNOWN;
        st = s;
    }
    return st;
}

/* write one direction of every run (apply: old -> new, revert: new -> old) on one
   image; checks the bytes before writing and the CRC after; restores on failure */
static int patch_image(u8 *img, const wfp_set_t *s, int apply)
{
    const u8 *r;
    u32 i, n, off;

    for (i = 0, r = s->runs; i < s->nruns; i++, r += (8 + 2 * n + 3) & ~3) {
        n = RD16(r + 4);
        if (memcmp(img + RD32(r), r + 8 + (apply ? 0 : n), n) != 0)
            return WFP_E_VERIFY;               /* nothing written yet */
    }
    for (i = 0, r = s->runs; i < s->nruns; i++, r += (8 + 2 * n + 3) & ~3) {
        n = RD16(r + 4);
        off = RD32(r);
        memcpy(img + off, r + 8 + (apply ? n : 0), n);
    }
    if (crc32(img, s->target_size, 0) == (apply ? s->result_crc : s->target_crc))
        return 0;
    for (i = 0, r = s->runs; i < s->nruns; i++, r += (8 + 2 * n + 3) & ~3) {
        n = RD16(r + 4);
        memcpy(img + RD32(r), r + 8 + (apply ? 0 : n), n);
    }
    return WFP_E_VERIFY;
}

static int set_state(const wfp_set_t *s, int apply)
{
    u32 a[WFP_MAX_COPIES], size, from, to;
    int n, c, done = 0, r = 0, todo[WFP_MAX_COPIES];

    if (s == NULL || s->runs == NULL)
        return WFP_E_ARG;
    if ((n = wfp_copies(a, WFP_MAX_COPIES, &size)) < 0)
        return n;
    if (size != s->target_size)
        return WFP_E_TARGET;
    from = apply ? s->target_crc : s->result_crc;
    to = apply ? s->result_crc : s->target_crc;
    for (c = 0; c < n; c++) {                  /* every copy must be in one of the two states */
        u32 crc = crc32((const u8 *)a[c], size, 0);
        if (crc != from && crc != to)
            return WFP_E_TARGET;
        todo[c] = crc == from;
    }
    for (c = 0; c < n; c++) {
        if (!todo[c])
            continue;
        if ((r = patch_image((u8 *)a[c], s, apply)) < 0) {
            while (c-- > 0)                    /* all or nothing: undo the copies done so far */
                if (todo[c])                   /* (the failed one restored itself) */
                    patch_image((u8 *)a[c], s, !apply);
            break;
        }
        done++;
    }
    sceKernelDcacheWritebackInvalidateAll();
    return r < 0 ? r : done;
}

int wfp_apply(const wfp_set_t *s)  { return set_state(s, 1); }
int wfp_revert(const wfp_set_t *s) { return set_state(s, 0); }

/* ---- loading the image into the chip ------------------------------------- */

/*
 * The chip only takes a new firmware at power-on, so we power-cycle it the way
 * the physical WLAN switch does: wlan.prx registers sub_29DC(on, ctx) as
 * syscon's switch callback, and we call it ourselves with on = 0, then 1.
 * on = 0 powers the chip off; on = 1 powers it on, and wlan.prx sends the
 * (patched) image from RAM. The callback and the context pointer are found
 * at fixed offsets and checked against the analysed code first.
 *
 * If someone knows a more elegant way to load it, pls open a PR.
 */
static const u32 sig_swcb[] = { 0x27BDFFF0, 0xAFB00000, 0x00A08021, 0xAFBF0008 };

int wfp_reload(void)
{
    SceModule *m = sceKernelFindModuleByName("sceWlan_Driver");
    const u32 *cb, *gs;
    u8 *ctx;
    unsigned i;

    if (m == NULL)
        return WFP_E_WLAN;
    cb = (const u32 *)(m->text_addr + WLAN_OFF_SWCB);
    gs = (const u32 *)(m->text_addr + WLAN_OFF_GETSW);
    for (i = 0; i < sizeof(sig_swcb) / 4; i++)
        if (cb[i] != sig_swcb[i])
            return WFP_E_WLAN;
    /* sceWlanGetSwitchState = lui v1,hi / lw a1,lo(v1) / lw a0,0x1D0(a1): must load the context pointer */
    if ((gs[0] & 0xFFFF0000) != 0x3C030000 || (gs[1] & 0xFFFF0000) != 0x8C650000 || gs[2] != 0x8CA401D0 ||
        ((gs[0] & 0xFFFF) << 16) + (u32)(s32)(s16)(gs[1] & 0xFFFF) != m->text_addr + WLAN_OFF_CTXPTR)
        return WFP_E_WLAN;
    ctx = *(u8 **)(m->text_addr + WLAN_OFF_CTXPTR);
    if (ctx == NULL || (*(u32 *)(ctx + CTX_FLAGS) & 0x2000))
        return WFP_E_WLAN;

    ((void (*)(int, void *))cb)(0, ctx);      /* switch "off": chip powered down */
    sceKernelDelayThread(RELOAD_WAIT_US);
    ((void (*)(int, void *))cb)(1, ctx);      /* switch "on": wlan.prx sends the image */
    sceKernelDelayThread(RELOAD_WAIT_US);
    return 0;
}

int wfp_activate(const wfp_set_t *s)
{
    int r = wfp_apply(s);
    return r < 0 ? r : wfp_reload();
}

int wfp_deactivate(const wfp_set_t *s)
{
    int r = wfp_revert(s);
    return r < 0 ? r : wfp_reload();
}
