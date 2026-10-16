#!/usr/bin/env python3
"""把 iOS 改写映像转成 Android 版：mrs tpidrro_el0 → mrs tpidr_el0（只扫指令区）。

Darwin 的 pthread TSD 挂在 tpidrro_el0 上；Android/bionic 的线程指针在 tpidr_el0。
原代码的 tpidr_el0 / tpidrro_el0 直读已被上游改写器全部换成线程槽访问
（经键页取偏移、经 tpidrro_el0 取基址）。只改写指令区：
  [0, TEXT_END) 原代码 + [PATCH_OFF, 文件尾) 补丁区；
rodata/data 是数据，不入扫（内嵌着色器等数据不受扰动）。

用法: python patch_android.py <in.patched.bin> <fixups.tsv> <out.android.bin>
"""
import struct
import sys

MRS_TPIDRRO = 0xD53BD060
MRS_TPIDR = 0xD53BD040


def main(src, tsv, dst):
    text_end = patch_off = None
    for line in open(tsv, encoding='utf-8'):
        if line.startswith('# '):
            k, v = line[2:].split('\t')
            if k == 'TEXT_END': text_end = int(v.strip(), 16)
            elif k == 'PATCH_OFF': patch_off = int(v.strip(), 16)
    assert text_end and patch_off, 'fixups 缺 TEXT_END/PATCH_OFF'
    d = bytearray(open(src, 'rb').read())
    n = 0
    for lo, hi in ((0, text_end), (patch_off, len(d))):
        for off in range(lo, hi - 3, 4):
            w = struct.unpack_from('<I', d, off)[0]
            if (w & 0xFFFFFFE0) == MRS_TPIDRRO:
                struct.pack_into('<I', d, off, MRS_TPIDR | (w & 0x1F))
                n += 1
    open(dst, 'wb').write(d)
    print('%s: 指令区改写 %d 处 tpidrro→tpidr（text=%#x patch=%#x）→ %s' % (src, n, text_end, patch_off, dst))


if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2], sys.argv[3])
