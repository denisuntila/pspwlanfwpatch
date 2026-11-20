# pspwlanfwpatch

Patch the firmware of the PSP's WLAN chip (Marvell "voyager", `voyager.bin`, PSP-2000/3000/Go) in RAM and
restart the chip with it. Two halves:

| Part | What |
|---|---|
| `tools/pspwlanfwpatch.py` | Python 3, no dependencies: JSON patch → `.wfp` file (all checksums computed), inspect, apply, diff, C export |
| `psp/` | `libpspwlanfwpatch.a` + `pspwlanfwpatch.h`: kernel-mode static library that loads `.wfp` files and applies them |
| `examples/` | `ssid_filter_off.json/.wfp` (keep BSSes without an SSID element, e.g. an NDS), `plugin/` (minimal kernel plugin) |

## Writing a patch

A patch is JSON, without any checksum:

```json
{
  "format": "pspwlanfwpatch-json/1",
  "name": "SSID_FILTER_OFF",
  "description": "free text",
  "changes": [
    {"addr": "0xC0000B48", "old": "2A D0", "new": "C0 46", "comment": "beq -> nop"},
    {"offset": "0x1BDA", "new": "C0 46"}
  ]
}
```

`addr` is a chip address, `offset` a file offset into the firmware. `old` is optional but recommended: it is
checked against the firmware. Changes must lie inside block data (never a block header or CRC field) and must
not overlap.

```sh
python3 tools/pspwlanfwpatch.py build voyager.bin patch.json [-o patch.wfp] [--keep patched.bin]
python3 tools/pspwlanfwpatch.py info  patch.wfp
python3 tools/pspwlanfwpatch.py apply voyager.bin patch.wfp [-o patched.bin]   # the PSP library's checks
python3 tools/pspwlanfwpatch.py diff  voyager.bin patched.bin [-o patch.json]  # from an image patched elsewhere
python3 tools/pspwlanfwpatch.py ctable patch.wfp [-o patch.h]                  # embed a .wfp in C
```

`build` recomputes the firmware's block CRCs, writes the `.wfp` and re-applies it with the same checks the PSP
library does. New code (code caves) is assembled with any ARM toolchain of your choice; put the resulting bytes
in the JSON, or patch an image and use `diff`.

`voyager.bin` is not included: extract it from your own `wlanfirm_02g.prx`.

## Using it on the PSP

```c
#include "pspwlanfwpatch.h"

wfp_set_t s;
if (wfp_set_load(&s, "ms0:/seplugins/ssid_filter_off.wfp") == 0)
    wfp_activate(&s);          /* patch the image in RAM + restart the chip with it (~6 s) */
...
wfp_deactivate(&s);            /* original firmware again */
wfp_set_free(&s);
```

| Function | |
|---|---|
| `wfp_set_load(s, path)` / `wfp_set_parse(s, buf, len)` / `wfp_set_free(s)` | read and check a `.wfp` (file CRC, header, runs) |
| `wfp_apply(s)` / `wfp_revert(s)` | patch / unpatch every registered image copy in RAM, all or nothing |
| `wfp_reload()` | restart the chip so it loads the image from RAM |
| `wfp_activate(s)` / `wfp_deactivate(s)` | apply / revert + reload in one call |
| `wfp_state(sets, n)` | which set is applied (index), `WFP_ORIGINAL`, or `WFP_E_UNKNOWN` |
| `wfp_copies(addr, max, &size)` | the image copies registered with wlan.prx |

Errors are negative `WFP_E_*` codes (see the header). Before writing anything, `wfp_apply` checks that the
image is the patch's target (size + CRC-32) and that every original byte matches. Afterwards it checks the
result CRC, and on any mismatch it restores the image.

Build: `make` in `psp/` (PSPSDK). Link with `-lpspwlanfwpatch -lpsploadcore_kernel_660`, kernel mode only
(see `examples/plugin/`). Tested against wlan.prx of system software 6.61. `wfp_reload` checks the wlan.prx code
it relies on and returns `WFP_E_WLAN` if it differs.

## .wfp format (little endian)

```
header, 64 bytes
  0  char magic[4]   "WFP1"
  4  u16  version    1
  6  u16  flags      0
  8  char name[32]   NUL-padded
 40  u32  target_size, 44 u32 target_crc   original image: size, CRC-32
 48  u32  result_crc                        CRC-32 after applying
 52  u32  nruns, 56 u32 payload_len         payload = bytes after the header
 60  u32  file_crc                          CRC-32 of the file with this field = 0
runs, each padded to 4 bytes
  u32 off (file offset in the image), u16 len, u16 0, u8 old[len], u8 new[len]
```

CRC-32 = zlib's `crc32`. The runs include the recomputed firmware block CRCs (those use CRC-32 poly
0x04C11DB7, init 0, not reflected, stored big endian).
