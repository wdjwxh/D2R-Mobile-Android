#!/usr/bin/env python3
"""Android 路线着色器出包：Switch 着色器包 → SPIR-V（dmsh_xlate，Ryujinx 翻译器）。

镜像 dmsh_build.py 的作业构建（记录切分、键、c1、事实表），但止步于 SPIR-V：
不做 spirv-cross / Metal。产物：<输出目录>/<键>.spv 与 <键>.res.json。
用法: python spv_build.py <packs目录> <main映像> <上游dm/gfx/nvn目录> <dmsh_xlate路径> <输出目录> [键...]
"""
import json
import os
import re
import struct
import subprocess
import sys
import tempfile
from collections import Counter

CTRL = bytes.fromhex('34127698')   # 控制段魔数 | control segment magic
CODE_MARK = bytes.fromhex('78563412')


def records(d):
    r = [m.start() for m in re.finditer(b'BTCDBTCD', d)]
    return [d[x:(r[k + 1] if k + 1 < len(r) else len(d))] for k, x in enumerate(r)]


def load_map(nvn_dir):
    MAP = os.path.join(nvn_dir, 'shader_map.tsv')
    loc, stage_of = {}, {}
    for ln in open(MAP, encoding='utf-8'):
        c = ln.rstrip('\n').split('\t')
        if len(c) > 4 and c[0].endswith('.bin'):
            loc.setdefault(c[3], (c[0], int(c[1])))
        if len(c) > 4 and c[0].startswith('main.rodata@'):
            loc.setdefault(c[3], (c[0], int(c[0].split('@')[1], 16)))
            stage_of[c[3]] = {'顶点': 'vs', '像素': 'ps'}[c[2]]
    facts = {}
    ft = os.path.join(nvn_dir, 'shader_facts.tsv')
    for l in open(ft, encoding='utf-8'):
        if l.strip() and not l.startswith('#'):
            k, v = l.rstrip('\n').split('\t', 1)
            facts[k] = json.loads(v)
    return loc, stage_of, facts


def job_for(key, name, rec, stage, facts, workdir):
    import hashlib
    ci = rec.find(CTRL)
    assert ci > 0, (name, key, '找不到控制段')
    ctl = rec[ci:ci + 0x880]
    assert hashlib.sha256(ctl).hexdigest()[:16] == key, (name, key, '控制段哈希与键不符')
    code = rec[0x38:ci]
    c1o, c1t = struct.unpack_from('<2I', rec, ci + 0x700)
    c1 = rec[0x38 + c1o:0x38 + c1t]
    if stage != 'cs':
        code = code[0x30:]
    cp = os.path.join(workdir, key + '.bin')
    open(cp, 'wb').write(code)
    j = {'key': key, 'stage': stage, 'code': cp, 'tex': {}, 'img': {}, 'attr': {}, 'fout': {},
         'c1': c1.hex(), 'debug': True, 'reduced': True}
    if key in facts:
        j.update(facts[key])
    else:
        j['pc_missing'] = True
    return j


def main():
    packs_dir, main_img_path, nvn_dir, xlate, outdir = sys.argv[1:6]
    want = set(sys.argv[6:])
    os.makedirs(outdir, exist_ok=True)
    loc, stage_of, facts = load_map(nvn_dir)
    keys = sorted(want) if want else sorted(loc)
    main_img = open(main_img_path, 'rb').read()
    packs, jobs, stages = {}, [], Counter()
    skipped = Counter()
    for key in keys:
        name, k = loc[key]
        if name.startswith('main.rodata@'):
            o = k
            g = main_img.rfind(CODE_MARK, o - 0x1000, o)
            assert g >= 0 and main_img[o:o + 4] == CTRL, (name, '内嵌标记不符')
            rec = bytes(0x38) + main_img[g:o + 0x880]
        else:
            if name not in packs:
                packs[name] = records(open(os.path.join(packs_dir, os.path.basename(name)), 'rb').read())
            rec = packs[name][k]
        stage = stage_of.get(key, os.path.basename(name)[:2])
        stages[stage] += 1
        if stage not in ('vs', 'ps', 'cs'):
            skipped[stage] += 1
            continue
        jobs.append(job_for(key, name, rec, stage, facts, outdir))
    print('键 %d 个，阶段 %s，跳过 %s' % (len(keys), dict(stages), dict(skipped) or '无'))
    work = tempfile.mkdtemp(prefix='spvjob_')
    CH = 400
    fails = []
    failed_batches = []
    for i in range(0, len(jobs), CH):
        # Do not count stale products from an older translator as new success.
        for job in jobs[i:i + CH]:
            for suffix in ('.spv', '.res.json'):
                product = os.path.join(outdir, job['key'] + suffix)
                if os.path.isfile(product):
                    os.remove(product)
        jp = os.path.join(work, 'job_%d.json' % i)
        json.dump(jobs[i:i + CH], open(jp, 'w'))
        r = subprocess.run([xlate, jp, outdir], capture_output=True, text=True,
                           encoding='utf-8', errors='replace')
        got = sum(1 for j in jobs[i:i + CH] if os.path.exists(os.path.join(outdir, j['key'] + '.spv')))
        bad = [l for l in r.stderr.splitlines() if l.startswith('【红】')]
        fails += bad
        if r.returncode or got != len(jobs[i:i + CH]):
            failed_batches.append(i // CH + 1)
            print(r.stderr[-4000:])
        print('批 %d/%d：%d/%d 出 spv，stderr %d 行红' % (i // CH + 1, (len(jobs) + CH - 1) // CH, got,
              len(jobs[i:i + CH]), len(bad)))
    total = sum(1 for j in jobs if os.path.exists(os.path.join(outdir, j['key'] + '.spv')))
    print('完成：%d/%d 个 .spv → %s' % (total, len(jobs), outdir))
    if fails:
        open(os.path.join(outdir, '_fails.txt'), 'w').write('\n'.join(fails))
        print('失败 %d 个，详见 _fails.txt' % len(fails))
    if fails or failed_batches or total != len(jobs):
        raise SystemExit(1)


if __name__ == '__main__':
    main()
