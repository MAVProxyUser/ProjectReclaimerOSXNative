#!/usr/bin/env python3
"""String references and disassembly for an arm64 Mach-O (the native macOS Reclaimer 0.10 app), the counterpart of refs.py
for PE files. Needs numpy (scan) and capstone (dis: re-tools/angrenv/bin/python has it).
    machoref.py strref <binary> <text>            every adrp+add / adrp+ldr pair whose target lies inside an occurrence of <text>
                                                  (Rust concatenates literals: the target may be anywhere inside it), with the
                                                  function that holds the reference (LC_FUNCTION_STARTS)
    machoref.py dis <binary> <vmaddr> [<end>]     disassemble a function (from its start to the next one) or a range, adrp+add
                                                  targets annotated with the text found there
    machoref.py func <binary> <vmaddr>            the function containing <vmaddr>
    machoref.py calls <binary> <vmaddr>           every bl to <vmaddr> (callers)"""
import sys, struct, bisect

def load(path):
    d = open(path, 'rb').read()
    if struct.unpack_from('>I', d, 0)[0] == 0xcafebabe:              # universal binary: take the arm64 slice
        for i in range(struct.unpack_from('>I', d, 4)[0]):
            ct, cs, fo, fz, al = struct.unpack_from('>iiIII', d, 8 + i * 20)
            if ct == 0x0100000c: d = d[fo:fo + fz]; break
    magic, cpu, sub, ft, ncmds, sizeofcmds, flags, _ = struct.unpack_from('<IiiIIIII', d, 0)
    assert magic == 0xfeedfacf, 'not a 64-bit Mach-O (thin arm64 expected; lipo -thin arm64 first)'
    segs = []; secs = []; fstarts = []; off = 32; text_vm = None
    for _ in range(ncmds):
        cmd, sz = struct.unpack_from('<II', d, off)
        if cmd == 0x19:                                              # LC_SEGMENT_64
            name = d[off + 8:off + 24].rstrip(b'\0').decode(); vm, vms, fo, fs = struct.unpack_from('<QQQQ', d, off + 24)
            segs.append((name, vm, vms, fo, fs)); nsec = struct.unpack_from('<I', d, off + 64)[0]
            if name == '__TEXT': text_vm = vm
            for i in range(nsec):
                s = off + 72 + i * 80
                sn = d[s:s + 16].rstrip(b'\0').decode(); a, z = struct.unpack_from('<QQ', d, s + 32); o = struct.unpack_from('<I', d, s + 48)[0]
                secs.append((name, sn, a, z, o))
        elif cmd == 0x26:                                            # LC_FUNCTION_STARTS
            do, ds = struct.unpack_from('<II', d, off + 8); p = do; a = text_vm; v = 0; sh = 0
            while p < do + ds:
                b = d[p]; p += 1; v |= (b & 0x7f) << sh; sh += 7
                if b & 0x80: continue
                if v == 0: break
                a += v; fstarts.append(a); v = 0; sh = 0
        off += sz
    return d, segs, secs, fstarts

def vm2off(segs, vm):
    for n, a, z, fo, fs in segs:
        if a <= vm < a + min(z, fs): return fo + vm - a
    return None

def off2vm(segs, o):
    for n, a, z, fo, fs in segs:
        if fo <= o < fo + fs and fs: return a + o - fo
    return None

def text_sec(secs):
    for seg, sn, a, z, o in secs:
        if seg == '__TEXT' and sn == '__text': return a, z, o

def func_of(fstarts, vm):
    i = bisect.bisect_right(fstarts, vm) - 1
    return (fstarts[i], fstarts[i + 1] if i + 1 < len(fstarts) else None) if i >= 0 else (None, None)

def adrp_targets(d, secs):
    """(code vmaddr, target vmaddr) of every adrp followed within 6 instructions by add/ldr using the same base register."""
    import numpy as np
    ta, tz, to = text_sec(secs)
    w = np.frombuffer(d, dtype='<u4', count=tz // 4, offset=to)
    idx = np.nonzero((w & 0x9F000000) == 0x90000000)[0]
    out = []
    for i in idx.tolist():
        ins = int(w[i]); rd = ins & 31
        imm = ((ins >> 29) & 3) | (((ins >> 5) & 0x7FFFF) << 2)
        if imm & (1 << 20): imm -= 1 << 21
        pc = ta + i * 4; page = (pc & ~0xFFF) + (imm << 12)
        for j in range(i + 1, min(i + 7, len(w))):
            n = int(w[j])
            if (n & 0xFF800000) == 0x91000000 and ((n >> 5) & 31) == rd:          # add xd, xn, #imm12 (64-bit, no shift)
                out.append((ta + j * 4, page + ((n >> 10) & 0xFFF))); break
            if (n & 0xFFC00000) == 0xF9400000 and ((n >> 5) & 31) == rd:          # ldr xt, [xn, #imm12*8]
                out.append((ta + j * 4, page + ((n >> 10) & 0xFFF) * 8)); break
    return out

def cstr_at(d, segs, vm, n=80):
    o = vm2off(segs, vm)
    if o is None: return None
    s = d[o:o + n]
    t = s.split(b'\0')[0]
    if len(t) < 4 or any(c < 0x20 and c not in (9, 10) or c > 0x7e for c in t[:24]): return None
    return t.decode('latin1')

def main():
    cmd, path = sys.argv[1], sys.argv[2]
    d, segs, secs, fstarts = load(path)
    if cmd == 'strref':
        needle = sys.argv[3].encode(); hits = []; p = d.find(needle)
        while p >= 0:
            vm = off2vm(segs, p)
            if vm is not None: hits.append((vm, vm + len(needle)))
            p = d.find(needle, p + 1)
        print(f'{len(hits)} occurrence(s): ' + ', '.join(f'{a:#x}' for a, _ in hits))
        # a reference can point at the start of the concatenated literal that holds the text: take a window before it
        for code, tgt in adrp_targets(d, secs):
            for a, b in hits:
                if a - 512 <= tgt < b:
                    fs, fe = func_of(fstarts, code)
                    print(f'  {code:#x} -> {tgt:#x} (text {tgt - a:+d})  in function {fs:#x}' + (f'..{fe:#x}' if fe else ''))
    elif cmd == 'func':
        fs, fe = func_of(fstarts, int(sys.argv[3], 16)); print(f'{fs:#x}..{fe:#x} ({fe - fs} bytes)')
    elif cmd == 'calls':
        import numpy as np
        tgt = int(sys.argv[3], 16); ta, tz, to = text_sec(secs)
        w = np.frombuffer(d, dtype='<u4', count=tz // 4, offset=to)
        bl = np.nonzero((w & 0xFC000000) == 0x94000000)[0]
        imm = (w[bl] & 0x3FFFFFF).astype(np.int64); imm = np.where(imm & (1 << 25), imm - (1 << 26), imm)
        dest = ta + bl.astype(np.int64) * 4 + imm * 4
        for i in bl[dest == tgt].tolist():
            pc = ta + i * 4; fs, _ = func_of(fstarts, pc); print(f'  {pc:#x}  in function {fs:#x}')
    elif cmd == 'dis':
        import capstone
        a = int(sys.argv[3], 16)
        if len(sys.argv) > 4: e = int(sys.argv[4], 16)
        else:
            fs, fe = func_of(fstarts, a); a, e = fs, fe
        md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM); md.detail = False
        o = vm2off(segs, a); pages = {}
        for ins in md.disasm(d[o:o + (e - a)], a):
            note = ''
            if ins.mnemonic == 'adrp':
                r = ins.op_str.split(',')[0]; pages[r] = int(ins.op_str.split('#')[-1], 16)
            elif ins.mnemonic == 'add' and '#' in ins.op_str:
                ops = [x.strip() for x in ins.op_str.split(',')]
                if len(ops) == 3 and ops[1] in pages:
                    t = pages[ops[1]] + int(ops[2].lstrip('#'), 16); s = cstr_at(d, segs, t)
                    note = f'   ; {t:#x}' + (f' "{s[:70]}"' if s else '')
            print(f'{ins.address:#x}  {ins.mnemonic:8s} {ins.op_str}{note}')

if __name__ == '__main__':
    main()
