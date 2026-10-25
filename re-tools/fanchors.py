#!/usr/bin/env python3
"""What identifies a function across builds: the texts it and its callers load, its callees, its distinctive constants.
For choosing sigport.py anchors (shim/docs/porting.md).
    python3 re-tools/fanchors.py <exe> <function rva>
"""
import sys, os, re, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pelib import Image, cx86   # noqa: E402


def calls_in(im, f):
    out = []
    for x in im.insns(f[0], f[1]):
        if x.mnemonic == 'call' and x.operands and x.operands[0].type == cx86.X86_OP_IMM: out.append(x.operands[0].imm)
    return out


def callers(im, target):
    out = []
    for va, vs in im.exec:
        i = im.b.find(b'\xe8', va, va + vs)
        while i >= 0:
            if i + 5 + im.i32(i + 1) == target: out.append(i)
            i = im.b.find(b'\xe8', i + 1, va + vs)
    return out


def texts_in(im, f):
    out = set()
    for x in im.insns(f[0], f[1]):
        for op in x.operands:
            if op.type == cx86.X86_OP_MEM and op.mem.base == cx86.X86_REG_RIP:
                t = x.address + x.size + op.mem.disp
                if 0 <= t < im.size and not im.is_exec(t):
                    m = re.match(rb'[\x20-\x7e]{6,}', im.b[t:t + 80])
                    if m: out.add(m.group(0)[:60].decode())
    return out


if __name__ == '__main__':
    im = Image(sys.argv[1]); a = int(sys.argv[2], 16); f = im.func_of(a)
    print(f'function {f[0]:#x}..{f[1]:#x} ({f[1] - f[0]} bytes)')
    print('  texts:', sorted(texts_in(im, f))[:20])
    cs = callers(im, f[0]); print(f'  callers: {len(cs)}')
    for c in cs[:12]:
        g = im.func_of(c); print(f'    {c:#x} in {g[0]:#x}' if g else f'    {c:#x}', sorted(texts_in(im, g))[:6] if g else '')
    ce = collections.Counter(calls_in(im, f)); print(f'  callees: {len(ce)}', [hex(x) for x, n in ce.most_common(12)])
