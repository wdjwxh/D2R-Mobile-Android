#!/usr/bin/env python3
"""从 D2RMobile 的 Guest.framework/Guest 哑库重建 main/sdk 改写后模块映像。

布局依据 d2r-mobile/dm/tools/guest_dylib.py：
  __TEXT 里的节顺序 = 逐模块 (x0, x1)，文件内从 0x4000 排起；x0 = 映像 [0, r16(TEXT_END))，
  x1 = 映像 [PATCH_OFF, r16(映像尾))；__DM<名>W,__img = 映像 [r16(TEXT_END), PATCH_OFF)。
节头里 offset == addr == 文件偏移。边界值用 App 内 dm/<名>.fixups.tsv 交叉核对。
用法: python extract_guest.py <Guest文件> <fixups目录> <输出目录>
"""
import hashlib
import os
import struct
import sys

PG = 0x4000
r16 = lambda x: (x + PG - 1) & ~(PG - 1)


def read_fixups(path):
    h = {}
    for line in open(path, encoding='utf-8'):
        if line.startswith('# ') and '\t' in line:
            k, v = line[2:].split('\t')
            h[k] = int(v.strip(), 16)
    return h


def parse_macho_sections(g):
    magic, cputype = struct.unpack_from('<II', g, 0)
    assert magic == 0xfeedfacf and cputype == 0x0100000c, '不是 arm64 Mach-O'
    _, _, _, ncmd, _, _, _, _ = struct.unpack_from('<IIIIIIII', g, 0)
    segs, sects = {}, {}
    o = 32
    for _ in range(ncmd):
        cmd, size = struct.unpack_from('<II', g, o)
        if cmd == 0x19:  # LC_SEGMENT_64
            seg = g[o + 8:o + 24].rstrip(b'\0').decode()
            _, _, fileoff, filesize = struct.unpack_from('<QQQQ', g, o + 24)
            segs[seg] = (fileoff, filesize)
            nsects = struct.unpack_from('<I', g, o + 64)[0]
            so = o + 72
            for i in range(nsects):
                name = g[so:so + 16].rstrip(b'\0').decode()
                segname = g[so + 16:so + 32].rstrip(b'\0').decode()
                addr, ssize = struct.unpack_from('<QQ', g, so + 32)
                sects['%s,%s' % (segname, name)] = (addr, ssize)
                so += 80
        o += size
    return segs, sects


def main(guest_path, fixdir, outdir, names):
    g = open(guest_path, 'rb').read()
    segs, sects = parse_macho_sections(g)
    os.makedirs(outdir, exist_ok=True)
    for n in names:
        h = read_fixups(os.path.join(fixdir, n + '.fixups.tsv'))
        te, po = r16(h['TEXT_END']), h['PATCH_OFF']
        x0o, x0s = sects['__TEXT,__dm%sx0' % n]
        x1o, x1s = sects['__TEXT,__dm%sx1' % n]
        wo, ws = segs['__DM%sW' % n]
        assert x0s == te, (n, hex(x0s), hex(te))            # x0 覆盖 [0, r16(TEXT_END))
        assert ws == po - te, (n, hex(ws), hex(po - te))    # W 覆盖其后的可写区
        end = po + x1s
        img = bytearray(end)
        img[0:te] = g[x0o:x0o + x0s]
        img[te:po] = g[wo:wo + ws]
        img[po:end] = g[x1o:x1o + x1s]
        out = os.path.join(outdir, n + '.patched.bin')
        open(out, 'wb').write(img)
        print('%s: %d 字节  ELF头=%s  sha256=%s' % (
            out, end, img[:4].hex(), hashlib.sha256(img).hexdigest()[:16]))
        # ELF 结构核对：e_machine=AArch64(183)、e_type=ET_DYN(3)、程序头数
        if img[:4] == b'\x7fELF':
            _, etype, machine = struct.unpack_from('<HHH', img, 16)
            phoff = struct.unpack_from('<Q', img, 32)[0]
            phentsize, phnum = struct.unpack_from('<HH', img, 54)
            pts = []
            for i in range(phnum):
                p_type, p_flags = struct.unpack_from('<II', img, phoff + i * phentsize)
                pts.append((p_type, p_flags))
            print('  e_type=%d e_machine=%d phnum=%d PT_DYNAMIC=%s' % (
                etype, machine, phnum, any(t == 2 for t, _ in pts)))


if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2], sys.argv[3], ['main', 'sdk'])
