#!/usr/bin/env python3
"""离线读取 D2R Switch 静态 CASC 存储（不依赖 build_manifest.psv）。

用覆盖层的 .build.config 找到全量 TVFS（vfs-1），解析路径树，
按逻辑路径直接从 .data 档案解码取文件。基础档案与覆盖层档案可分布在多个目录。

用法:
  python tvfs_get.py list <overlay目录> <基础data目录...> [模式]
  python tvfs_get.py get  <overlay目录> <基础data目录...> <模式> <输出目录>
模式支持 * 与 ?（fnmatch，路径整体匹配）。
"""
import fnmatch
import os
import pathlib
import struct
import sys
import zlib

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), 'd2r-mobile', 'dm', 'tools'))
from casc_overlay import read_data_file, walk_paths, config_values


class Storage:
    def __init__(self, overlay_dir, base_dirs):
        # .build.config 与 07 号覆盖档案在 overlay 目录；其余档案按 EKey 档号在所有目录里找
        self.dirs = [os.path.join(overlay_dir, 'data')] + list(base_dirs)
        lines, values = config_values(pathlib.Path(overlay_dir) / 'data' / '.build.config')
        ckey, ekey = values['vfs-1'].split()
        dec, enc = map(int, values['vfs-1-size'].split())   # 顺序：解码后 解码前
        espec = values.get('vfs-1-espec', 'b:256K*=z').encode()
        tvfs = bytearray(self._decode(bytes.fromhex(ekey), enc, espec, dec))
        assert len(tvfs) == dec and tvfs[:4] == b'TVFS', 'vfs-1 无效'
        self.tvfs = tvfs
        self.path_off, self.path_size, self.vfs_off, self.vfs_size, self.cft_off, self.cft_size = \
            struct.unpack('>IIIIII', tvfs[12:36])
        self.est_off, self.est_size = struct.unpack('>II', tvfs[38:46])
        self._idx_width = 1 if self.est_size <= 0xff else 2 if self.est_size <= 0xffff else 4
        self.est = bytes(tvfs[self.est_off:self.est_off + self.est_size]).split(b'\0')
        self.files = {}   # 路径(bytes) → 文件表偏移
        for _, path, value, folder, _ in walk_paths(tvfs, self.path_off, self.path_off + self.path_size):
            if folder is None:
                self.files[path] = value

    def _read(self, ekey, enc_size):
        chunk, archive = ekey[9], ekey[10]
        name = '%02d-%08d.data' % (chunk, archive)
        last = None
        for d in self.dirs:
            p = os.path.join(d, name)
            if os.path.isfile(p):
                with open(p, 'rb') as f:
                    f.seek(int.from_bytes(ekey[11:16], 'big'))
                    return f.read(enc_size)
            last = p
        raise FileNotFoundError(name + '（在 ' + ', '.join(self.dirs) + ' 都没有）')

    def _decode(self, ekey, enc_size, espec, want_size):
        raw = self._read(ekey, enc_size)
        espec = espec.decode('latin-1')
        if espec.startswith('b:'):
            if raw[:4] != b'BLTE':
                raise ValueError('期望 BLTE')
            hs = struct.unpack('>I', raw[4:8])[0]
            n = int.from_bytes(raw[9:12], 'big')
            pos, out = hs, bytearray()
            for i in range(n):
                enc, dec = struct.unpack('>II', raw[12 + i * 24:20 + i * 24])
                blk = raw[pos:pos + enc]
                out.extend(blk[1:] if blk[:1] == b'N' else zlib.decompress(blk[1:]) if blk[:1] == b'Z' else (_ for _ in ()).throw(ValueError('块模式 %r' % blk[:1])))
                pos += enc
            data = bytes(out)
        elif espec == 'z':
            data = zlib.decompress(raw)
        elif espec == 'n':
            data = raw
        else:
            raise ValueError('不认识的 espec: ' + espec)
        if want_size and len(data) != want_size:
            raise ValueError('解码长度 %d != 期望 %d' % (len(data), want_size))
        return data

    def lookup(self, path_bytes):
        """路径 → (内容 bytes, EKey hex)"""
        v = self.files.get(path_bytes)
        if v is None:
            raise KeyError(path_bytes.decode())
        e = self.vfs_off + v
        flags = self.tvfs[e]
        assert flags == 1, '文件表 flags=%#x' % flags
        _zero, content_size = struct.unpack('>II', self.tvfs[e + 1:e + 9])
        w = self._idx_width
        # 文件表条目尾部的 CFT 偏移字段宽度与 _idx_width 同（规格序号宽度的推导同样基于表大小）；
        # vfs-1 的 cft_size 约 13 万条 × 25B → 4 字节宽
        cw = max(w, 4 if self.cft_size > 0xffffff else 3 if self.cft_size > 0xffff else 2 if self.cft_size > 0xff else 1)
        cft = int.from_bytes(self.tvfs[e + 9:e + 9 + cw], 'big')
        c = self.cft_off + cft
        ekey = bytes(self.tvfs[c:c + 16])
        enc_size = struct.unpack('>I', self.tvfs[c + 16:c + 20])[0]
        espec_idx = int.from_bytes(self.tvfs[c + 20:c + 20 + w], 'big')
        c_size = struct.unpack('>I', self.tvfs[c + 20 + w:c + 24 + w])[0]
        espec = self.est[espec_idx] if espec_idx < len(self.est) else b'n'
        return self._decode(ekey, enc_size, espec, c_size or content_size), ekey.hex()

    def match(self, pattern):
        return sorted(p for p in self.files if fnmatch.fnmatch(p.decode('latin-1'), pattern))


if __name__ == '__main__':
    cmd, overlay, bases = sys.argv[1:4]
    rest = sys.argv[4:]
    st = Storage(overlay, [b for b in bases.split(';') if b])
    if cmd == 'list':
        pat = rest[0] if rest else '*'
        for p in st.match(pat):
            print(p.decode('latin-1'))
        print('# %d 条' % len(st.match(pat)), file=sys.stderr)
    elif cmd == 'get':
        pat, outdir = rest[0], rest[1]
        os.makedirs(outdir, exist_ok=True)
        hits = st.match(pat)
        for i, p in enumerate(hits):
            data, ekey = st.lookup(p)
            out = os.path.join(outdir, os.path.basename(p.decode('latin-1')))
            open(out, 'wb').write(data)
            if (i + 1) % 20 == 0 or i + 1 == len(hits):
                print('已取 %d/%d' % (i + 1, len(hits)), file=sys.stderr)
        print('# 共 %d 个文件 → %s' % (len(hits), outdir), file=sys.stderr)
