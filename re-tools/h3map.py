#!/usr/bin/env python3
"""Read-only reader for MCC Halo 3 cache files (.map, header version 13 / "Update 14" builds).

    python3 re-tools/h3map.py <file.map> tags [substring]      list tags (index, group, name)
    python3 re-tools/h3map.py <file.map> hex <tag index> [len]  hex dump of a tag's meta

Layouts follow Assembly (github.com/XboxChaos/Assembly, Blamite/Formats/Halo3MCC/LayoutsU13, ThirdGen*.cs):
tag pointers are 32-bit, expanded as (p << 2); file offset = tag section start + (expanded - virtual base)."""
import struct, sys

class H3Map:
    def __init__(self, path):
        self.f = open(path, 'rb'); h = self.f.read(0x4000); self.h = h
        assert h[:4] == b'daeh', 'not a cache file'
        self.tag_buf, self.vsize = struct.unpack_from('<II', h, 0x10)
        self.vbase, self.index_addr = struct.unpack_from('<QQ', h, 0x2E0)
        self.masks = struct.unpack_from('<4I', h, 0x4CC)
        self.sections = [struct.unpack_from('<II', h, 0x4DC + 8 * i) for i in range(4)]   # debug, resource, tag, locale
        self.tag_off = (self.sections[2][0] + self.masks[2]) & 0xFFFFFFFF
        ix = self.read_va(self.index_addr, 0x48)
        ngroups, = struct.unpack_from('<i', ix, 0); gaddr, = struct.unpack_from('<Q', ix, 8)
        ntags, = struct.unpack_from('<i', ix, 0x10); taddr, = struct.unpack_from('<Q', ix, 0x18)
        g = self.read_va(gaddr, ngroups * 16)
        self.groups = [g[i*16:i*16+4][::-1].decode('latin1') for i in range(ngroups)]
        t = self.read_va(taddr, ntags * 8)
        self.tags = []
        for i in range(ntags):
            gi, salt, addr = struct.unpack_from('<hHI', t, i * 8)
            self.tags.append((self.groups[gi] if gi >= 0 else None, salt, addr))
        # tag names: debug-section file table
        cnt, off, size, idx = struct.unpack_from('<4i', h, 0x20)
        dbg = self.masks[0]
        self.f.seek((idx + dbg) & 0xFFFFFFFF); offs = struct.unpack('<%di' % cnt, self.f.read(cnt * 4))
        self.f.seek((off + dbg) & 0xFFFFFFFF); blob = self.f.read(size)
        self.names = [blob[o:blob.index(b'\0', o)].decode('latin1') if o >= 0 else '' for o in offs]
    def va_to_off(self, va): return self.tag_off + (va - self.vbase)
    def read_va(self, va, n): self.f.seek(self.va_to_off(va)); return self.f.read(n)
    def p(self, p32): return p32 << 2                      # expand a 32-bit tag pointer
    def read_p(self, p32, n): return self.read_va(self.p(p32), n)
    def meta(self, ti, n): return self.read_p(self.tags[ti][2], n)
    def block(self, data, off, esize):                     # tag block at data+off -> list of element bytes
        cnt, ptr = struct.unpack_from('<iI', data, off)
        if cnt <= 0 or not ptr: return []
        raw = self.read_p(ptr, cnt * esize); return [raw[i*esize:(i+1)*esize] for i in range(cnt)]
    def tagref(self, data, off):                           # -> tag index or -1
        grp, = struct.unpack_from('<I', data, off); di, = struct.unpack_from('<I', data, off + 12)
        return -1 if di == 0xFFFFFFFF else di & 0xFFFF
    def find(self, group, sub=''):
        return [i for i, t in enumerate(self.tags) if t[0] == group and sub in self.names[i]]

if __name__ == '__main__':
    m = H3Map(sys.argv[1])
    if sys.argv[2] == 'tags':
        sub = sys.argv[3] if len(sys.argv) > 3 else ''
        for i, (g, s, a) in enumerate(m.tags):
            if g and sub in m.names[i] or sub == g: print(i, g, hex(a << 2), m.names[i])
    elif sys.argv[2] == 'hex':
        ti = int(sys.argv[3], 0); n = int(sys.argv[4], 0) if len(sys.argv) > 4 else 0x100
        d = m.meta(ti, n)
        for o in range(0, n, 16): print('%04x ' % o + ' '.join('%08x' % struct.unpack_from('<I', d, o + k)[0] for k in range(0, 16, 4)))
