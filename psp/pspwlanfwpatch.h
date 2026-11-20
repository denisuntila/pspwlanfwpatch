/*
 * pspwlanfwpatch - patch the PSP WLAN chip firmware (voyager.bin) in RAM.
 *
 * Kernel mode only (link into a kernel plugin). Patches are .wfp files built
 * with tools/pspwlanfwpatch.py; see ../README.md for the format.
 *
 * Typical use:
 *     wfp_set_t s;
 *     if (wfp_set_load(&s, "ms0:/seplugins/ssid_filter_off.wfp") == 0)
 *         wfp_activate(&s);                // patch + restart the chip with it
 *     ...
 *     wfp_deactivate(&s);                  // back to the original firmware
 *     wfp_set_free(&s);
 *
 * Applying only changes the image in RAM; the chip gets it at the next
 * power-on, which wfp_reload() triggers. Tested with wlan.prx of PSP system
 * software 6.61 (PSP-2000/3000/Go).
 */
#ifndef PSPWLANFWPATCH_H
#define PSPWLANFWPATCH_H

#include <psptypes.h>

#define WFP_E_ARG      (-1)    /* bad argument */
#define WFP_E_FORMAT   (-2)    /* not a valid .wfp (magic, version, lengths, runs) */
#define WFP_E_CRC      (-3)    /* .wfp file CRC mismatch: the file is damaged */
#define WFP_E_IO       (-4)    /* file could not be read */
#define WFP_E_NOMEM    (-5)
#define WFP_E_NOIMAGE  (-6)    /* no firmware image registered with wlan.prx */
#define WFP_E_TARGET   (-7)    /* image is neither the patch's original nor its result */
#define WFP_E_VERIFY   (-8)    /* bytes or result CRC wrong while patching (image restored) */
#define WFP_E_WLAN     (-9)    /* wlan.prx is not the analysed version: no reload */
#define WFP_E_UNKNOWN  (-10)   /* wfp_state: image matches none of the given sets */

#define WFP_ORIGINAL   (-1)    /* wfp_state: original image */
#define WFP_MAX_COPIES 4

typedef struct {
    char name[32];
    u32 target_size;           /* original image: size, CRC-32 */
    u32 target_crc;
    u32 result_crc;            /* CRC-32 after applying */
    u32 nruns;
    const u8 *runs;            /* points into the parsed buffer */
    u32 runs_len;
    void *mem;                 /* buffer allocated by wfp_set_load (else NULL) */
} wfp_set_t;

/* .wfp in memory (kept by the caller while the set is used) or from a file */
int  wfp_set_parse(wfp_set_t *s, const void *buf, u32 len);
int  wfp_set_load(wfp_set_t *s, const char *path);
void wfp_set_free(wfp_set_t *s);

/* firmware image copies registered with wlan.prx: count, addresses, image size */
int  wfp_copies(u32 *addr, int max, u32 *size);

/* which of the sets is applied: index, WFP_ORIGINAL, or WFP_E_UNKNOWN (copies disagree too) */
int  wfp_state(const wfp_set_t *sets, int nsets);

/* apply / remove a patch on every copy, all or nothing, in RAM only.
   Returns the number of copies changed (0: already in that state) or < 0. */
int  wfp_apply(const wfp_set_t *s);
int  wfp_revert(const wfp_set_t *s);

/* restart the chip so it loads the image from RAM (~6 s, call from a thread) */
int  wfp_reload(void);

/* one call: apply / revert, then restart the chip (always, so it surely runs that image) */
int  wfp_activate(const wfp_set_t *s);
int  wfp_deactivate(const wfp_set_t *s);

#endif
