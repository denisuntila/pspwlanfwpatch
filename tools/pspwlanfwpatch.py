#!/usr/bin/env python3
"""pspwlanfwpatch - build, inspect and apply patches for the PSP WLAN chip firmware
(voyager.bin, Marvell "fwheader" block format), no ARM toolchain needed.

Patch source = JSON, no checksums in it (the tool computes them):

    {
      "format": "pspwlanfwpatch-json/1",
      "name": "SSID_FILTER_OFF",                  (<= 31 ASCII characters)
      "description": "free text",                 (optional)
      "changes": [
        {"addr": "0xC0000B48", "old": "2A D0", "new": "C0 46", "comment": "..."},
        {"offset": "0x1BDA", "new": "C0 46"}      ("offset" = file offset instead of chip address;
      ]                                            "old" optional but recommended: checked)
    }

Binary patch file (.wfp, little endian) = what the PSP library reads:

    header, 64 bytes
      0  char  magic[4]       "WFP1"
      4  u16   version        1
      6  u16   flags          0
      8  char  name[32]       NUL-padded
     40  u32   target_size    size of the original image
     44  u32   target_crc     CRC-32 (zlib) of the original image
     48  u32   result_crc     CRC-32 (zlib) of the image after applying
     52  u32   nruns
     56  u32   payload_len    bytes after the header
     60  u32   file_crc       CRC-32 (zlib) of the whole file with this field = 0
    runs (nruns times), each padded to a multiple of 4 bytes
      u32 off  (file offset in the image), u16 len, u16 0, u8 old[len], u8 new[len]

The runs include the recomputed block CRC bytes, so applying them byte by byte yields
a firmware image whose blocks all check out.

Commands:
    build  FIRMWARE PATCH.json [-o OUT.wfp] [--keep PATCHED.bin]
    diff   ORIGINAL PATCHED [-o PATCH.json] [--name NAME]     (block CRC bytes left out)
    info   FILE.wfp
    apply  FIRMWARE FILE.wfp [-o PATCHED.bin]                 (same checks as the PSP side)
    ctable FILE.wfp [-o FILE.h] [--symbol NAME]               (the .wfp as a C byte array)
"""
import argparse
import json
import struct
import sys
import zlib

MAGIC = b'WFP1'
VERSION = 1
HDR = struct.Struct('<4sHH32sIIIIII')       # 64 bytes
RUN = struct.Struct('<IHH')
assert HDR.size == 64


class WfpError(Exception):
    pass


# ---- firmware image (Marvell fwheader blocks) ----------------------------------------

def _make_msb_table():
    t = []
    for i in range(256):
        c = i << 24
        for _ in range(8):
            c = ((c << 1) ^ 0x04C11DB7) & 0xFFFFFFFF if c & 0x80000000 else (c << 1) & 0xFFFFFFFF
        t.append(c)
    return t


_MSB = _make_msb_table()


def block_crc(data):
    """CRC-32 of the firmware blocks: poly 0x04C11DB7, init 0, no reflection, no xorout."""
    c = 0
    for b in data:
        c = ((c << 8) & 0xFFFFFFFF) ^ _MSB[((c >> 24) ^ b) & 0xFF]
    return c


class Image:
    """voyager.bin: blocks {u32 cmd (1 data / 4 end), u32 chip address, u32 len, u32 header CRC (BE)}
    + data[len] whose last 4 bytes are the data CRC (BE)."""

    def __init__(self, data):
        self.d = bytearray(data)
        self.blocks = []                    # (file offset of the header, chip address, data length incl. CRC)
        o = 0
        while True:
            if o + 16 > len(self.d):
                raise WfpError('truncated block header at 0x%X' % o)
            cmd, addr, length, _ = struct.unpack_from('<IIII', self.d, o)
            if cmd == 4:
                self.end = o
                break
            if cmd != 1 or length < 4 or o + 16 + length > len(self.d):
                raise WfpError('bad block at 0x%X (cmd %d, len %d)' % (o, cmd, length))
            self.blocks.append((o, addr, length))
            o += 16 + length

    @classmethod
    def load(cls, path):
        with open(path, 'rb') as f:
            return cls(f.read())

    def crc_errors(self):
        bad = []
        for o, addr, length in self.blocks:
            if block_crc(self.d[o:o + 12]) != struct.unpack_from('>I', self.d, o + 12)[0]:
                bad.append('header @0x%X' % o)
            e = o + 16 + length - 4
            if block_crc(self.d[o + 16:e]) != struct.unpack_from('>I', self.d, e)[0]:
                bad.append('data of block @0x%X (chip 0x%08X)' % (o, addr))
        return bad

    def offset_of(self, addr, n=1):
        """file offset of chip address addr (n bytes, inside one block's data, not its CRC)"""
        for o, a, length in self.blocks:
            if a <= addr and addr + n <= a + length - 4:
                return o + 16 + addr - a
        raise WfpError('chip address 0x%08X (+%d) is not inside a block\'s data' % (addr, n))

    def check_data_range(self, off, n):
        """off..off+n must lie inside one block's data (not a header, not a CRC field)"""
        for o, a, length in self.blocks:
            if o + 16 <= off and off + n <= o + 16 + length - 4:
                return o
        raise WfpError('file range 0x%X..0x%X is not inside a block\'s data' % (off, off + n))

    def fix_crcs(self):
        for o, addr, length in self.blocks:
            e = o + 16 + length - 4
            self.d[e:e + 4] = struct.pack('>I', block_crc(self.d[o + 16:e]))

    def bytes(self):
        return bytes(self.d)


# ---- patch files -----------------------------------------------------------------------

def _hex(s):
    try:
        return bytes.fromhex(s.replace(' ', '').replace(':', ''))
    except ValueError:
        raise WfpError('bad hex string %r' % s)


def _int(v):
    return v if isinstance(v, int) else int(str(v), 0)


def runs_from_images(orig, new, merge_gap=8):
    """[(off, old, new)] covering every differing byte; runs closer than merge_gap merged"""
    diff = [i for i in range(len(orig)) if orig[i] != new[i]]
    runs = []
    for i in diff:
        if runs and i - (runs[-1][0] + runs[-1][1]) < merge_gap:
            runs[-1][1] = i - runs[-1][0] + 1
        else:
            runs.append([i, 1])
    return [(o, bytes(orig[o:o + n]), bytes(new[o:o + n])) for o, n in runs]


def encode_wfp(name, orig, new):
    nm = name.encode('ascii')
    if len(nm) > 31:
        raise WfpError('name longer than 31 characters')
    if len(orig) != len(new):
        raise WfpError('image size changed')
    body = bytearray()
    runs = runs_from_images(orig, new)
    for off, old, nw in runs:
        if len(old) > 0xFFFF:
            raise WfpError('run too long')
        body += RUN.pack(off, len(old), 0) + old + nw
        body += b'\0' * (-len(body) % 4)
    hdr = HDR.pack(MAGIC, VERSION, 0, nm, len(orig), zlib.crc32(orig), zlib.crc32(new),
                   len(runs), len(body), 0)
    fcrc = zlib.crc32(hdr + body)
    return HDR.pack(MAGIC, VERSION, 0, nm, len(orig), zlib.crc32(orig), zlib.crc32(new),
                    len(runs), len(body), fcrc) + bytes(body)


def decode_wfp(blob):
    """-> dict(name, target_size, target_crc, result_crc, runs=[(off, old, new)]); checks the file"""
    if len(blob) < HDR.size:
        raise WfpError('file too short')
    magic, ver, flags, nm, tsize, tcrc, rcrc, nruns, plen, fcrc = HDR.unpack_from(blob)
    if magic != MAGIC:
        raise WfpError('not a .wfp file (magic %r)' % magic)
    if ver != VERSION:
        raise WfpError('unsupported version %d' % ver)
    if HDR.size + plen != len(blob):
        raise WfpError('payload length %d does not match the file (%d bytes)' % (plen, len(blob)))
    zeroed = bytearray(blob)
    zeroed[60:64] = b'\0\0\0\0'
    if zlib.crc32(zeroed) != fcrc:
        raise WfpError('file CRC mismatch: the file is damaged')
    runs, o = [], HDR.size
    for _ in range(nruns):
        if o + RUN.size > len(blob):
            raise WfpError('truncated run')
        off, n, _ = RUN.unpack_from(blob, o)
        o += RUN.size
        if o + 2 * n > len(blob):
            raise WfpError('truncated run data')
        runs.append((off, bytes(blob[o:o + n]), bytes(blob[o + n:o + 2 * n])))
        o += 2 * n
        o += -o % 4
    if o != len(blob):
        raise WfpError('trailing bytes after the runs')
    return dict(name=nm.rstrip(b'\0').decode('ascii'), target_size=tsize, target_crc=tcrc,
                result_crc=rcrc, flags=flags, runs=runs)


def apply_wfp(image_bytes, p):
    """same checks as the PSP library: target size + CRC, every old byte, result CRC, block CRCs"""
    d = bytearray(image_bytes)
    if len(d) != p['target_size'] or zlib.crc32(d) != p['target_crc']:
        raise WfpError('this patch is for another firmware image (size/CRC mismatch)')
    for off, old, new in p['runs']:
        if off + len(old) > len(d) or d[off:off + len(old)] != old:
            raise WfpError('original bytes differ at 0x%X' % off)
        d[off:off + len(new)] = new
    if zlib.crc32(d) != p['result_crc']:
        raise WfpError('result CRC mismatch')
    bad = Image(d).crc_errors()
    if bad:
        raise WfpError('patched image has bad block CRCs: %s' % ', '.join(bad))
    return bytes(d)


# ---- JSON patch source ------------------------------------------------------------------

def build_from_json(fw_bytes, spec):
    if spec.get('format', 'pspwlanfwpatch-json/1') != 'pspwlanfwpatch-json/1':
        raise WfpError('unknown JSON format %r' % spec.get('format'))
    name = spec.get('name')
    if not name:
        raise WfpError('"name" missing')
    img = Image(fw_bytes)
    bad = img.crc_errors()
    if bad:
        raise WfpError('input firmware has bad block CRCs: %s' % ', '.join(bad))
    used = {}
    for k, ch in enumerate(spec.get('changes', [])):
        new = _hex(ch['new'])
        if not new:
            raise WfpError('change %d: empty "new"' % k)
        if 'addr' in ch:
            off = img.offset_of(_int(ch['addr']), len(new))
        elif 'offset' in ch:
            off = _int(ch['offset'])
            img.check_data_range(off, len(new))
        else:
            raise WfpError('change %d: needs "addr" or "offset"' % k)
        for i in range(off, off + len(new)):
            if i in used:
                raise WfpError('change %d overlaps change %d at file offset 0x%X' % (k, used[i], i))
            used[i] = k
        if 'old' in ch:
            old = _hex(ch['old'])
            if len(old) != len(new):
                raise WfpError('change %d: "old" and "new" differ in length' % k)
            if bytes(img.d[off:off + len(old)]) != old:
                raise WfpError('change %d: firmware has %s at 0x%X, expected %s'
                               % (k, img.d[off:off + len(old)].hex(' '), off, old.hex(' ')))
        img.d[off:off + len(new)] = new
    if not used:
        raise WfpError('no changes')
    img.fix_crcs()
    assert img.crc_errors() == []
    return name, img.bytes()


def json_from_images(orig, new, name):
    io, im = Image(orig), Image(new)
    if [b[1:] for b in io.blocks] != [b[1:] for b in im.blocks]:
        raise WfpError('block layout differs')
    changes = []
    for o, a, length in io.blocks:          # data bytes only (headers and CRC fields left out),
        start = o + 16                      # contiguous differences grouped within one block
        end = o + 16 + length - 4
        i = start
        while i < end:
            if orig[i] == new[i]:
                i += 1
                continue
            j = i
            while j < end and orig[j] != new[j]:
                j += 1
            changes.append({'addr': '0x%08X' % (a + i - start), 'old': orig[i:j].hex(' ').upper(),
                            'new': new[i:j].hex(' ').upper()})
            i = j
    if any(orig[i] != new[i] for i in range(io.end, len(orig))):
        raise WfpError('the images differ after the end block')
    return {'format': 'pspwlanfwpatch-json/1', 'name': name, 'changes': changes}


# ---- command line -------------------------------------------------------------------------

def _read(path):
    with open(path, 'rb') as f:
        return f.read()


def _write(path, data):
    with open(path, 'wb') as f:
        f.write(data)


def cmd_build(a):
    fw = _read(a.firmware)
    with open(a.patch) as f:
        spec = json.load(f)
    name, new = build_from_json(fw, spec)
    blob = encode_wfp(name, fw, new)
    out = a.output or a.patch.rsplit('.', 1)[0] + '.wfp'
    _write(out, blob)
    p = decode_wfp(blob)
    apply_wfp(fw, p)                        # round trip with the PSP-side checks
    print('%s: "%s", %d runs, %d bytes; target CRC %08X, result CRC %08X'
          % (out, name, len(p['runs']), len(blob), p['target_crc'], p['result_crc']))
    if a.keep:
        _write(a.keep, new)
        print('%s: patched firmware' % a.keep)


def cmd_diff(a):
    spec = json_from_images(_read(a.original), _read(a.patched), a.name)
    text = json.dumps(spec, indent=2) + '\n'
    if a.output:
        with open(a.output, 'w') as f:
            f.write(text)
        print('%s: %d changes' % (a.output, len(spec['changes'])))
    else:
        sys.stdout.write(text)


def cmd_info(a):
    p = decode_wfp(_read(a.file))
    n = sum(len(r[1]) for r in p['runs'])
    print('name        %s' % p['name'])
    print('target      %d bytes, CRC-32 %08X' % (p['target_size'], p['target_crc']))
    print('result      CRC-32 %08X' % p['result_crc'])
    print('runs        %d (%d bytes)' % (len(p['runs']), n))
    for off, old, new in p['runs']:
        print('  0x%05X  %3d  %s -> %s' % (off, len(old), old.hex()[:32], new.hex()[:32]))


def cmd_apply(a):
    out = apply_wfp(_read(a.firmware), decode_wfp(_read(a.file)))
    if a.output:
        _write(a.output, out)
    print('ok: target, original bytes, result CRC and block CRCs check out'
          + ('; written to %s' % a.output if a.output else ''))


def cmd_ctable(a):
    blob = _read(a.file)
    p = decode_wfp(blob)
    sym = a.symbol or 'wfp_' + ''.join(c if c.isalnum() else '_' for c in p['name'].lower())
    lines = ['/* %s - generated by pspwlanfwpatch.py ctable from %s; parse with wfp_set_parse() */'
             % (sym, a.file.split('/')[-1]),
             'static const unsigned char %s[%d] __attribute__((aligned(4))) = {' % (sym, len(blob))]
    for i in range(0, len(blob), 16):
        lines.append('    ' + ' '.join('0x%02X,' % b for b in blob[i:i + 16]))
    lines.append('};')
    text = '\n'.join(lines) + '\n'
    if a.output:
        with open(a.output, 'w') as f:
            f.write(text)
        print('%s: %s (%d bytes)' % (a.output, sym, len(blob)))
    else:
        sys.stdout.write(text)


def main(argv=None):
    ap = argparse.ArgumentParser(prog='pspwlanfwpatch', description=__doc__.split('\n')[0])
    sub = ap.add_subparsers(dest='cmd', required=True)
    s = sub.add_parser('build', help='JSON patch -> .wfp (checksums computed)')
    s.add_argument('firmware'); s.add_argument('patch')
    s.add_argument('-o', '--output'); s.add_argument('--keep', metavar='PATCHED.bin',
                                                     help='also write the patched firmware')
    s.set_defaults(fn=cmd_build)
    s = sub.add_parser('diff', help='original + patched image -> JSON patch')
    s.add_argument('original'); s.add_argument('patched')
    s.add_argument('-o', '--output'); s.add_argument('--name', default='PATCH')
    s.set_defaults(fn=cmd_diff)
    s = sub.add_parser('info', help='show a .wfp file')
    s.add_argument('file'); s.set_defaults(fn=cmd_info)
    s = sub.add_parser('apply', help='apply a .wfp to a firmware image (PSP-side checks)')
    s.add_argument('firmware'); s.add_argument('file'); s.add_argument('-o', '--output')
    s.set_defaults(fn=cmd_apply)
    s = sub.add_parser('ctable', help='.wfp -> C byte array')
    s.add_argument('file'); s.add_argument('-o', '--output'); s.add_argument('--symbol')
    s.set_defaults(fn=cmd_ctable)
    a = ap.parse_args(argv)
    try:
        a.fn(a)
    except (WfpError, OSError, KeyError, json.JSONDecodeError) as e:
        print('error: %s' % e, file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
