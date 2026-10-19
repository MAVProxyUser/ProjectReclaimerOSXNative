#!/usr/bin/env python3
"""Read-only reader for MCC Halo 3 cache files (.map, header version 13 / "Update 14" builds).

    python3 re-tools/h3map.py <file.map> tags [substring]      list tags (index, group, name)
    python3 re-tools/h3map.py <file.map> hex <tag index> [len]  hex dump of a tag's meta

Layouts follow Assembly (github.com/XboxChaos/Assembly, Blamite/Formats/Halo3MCC/LayoutsU13, ThirdGen*.cs):
tag pointers are 32-bit, expanded as (p << 2); file offset = tag section start + (expanded - virtual base)."""
import struct, sys

BLOCK_MAX_BYTES = 256 << 20   # one tag block read at most (stock maps: < 20 MB); counts in a downloaded map are untrusted


def _cstr(blob, o, cap=256):
    """NUL-terminated string at blob[o] (at most cap bytes; '' for an offset outside the blob)"""
    if not 0 <= o < len(blob):
        return ''
    e = blob.find(b'\0', o, o + cap)
    return blob[o:e if e >= 0 else min(len(blob), o + cap)].decode('latin1')


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
        # counts from the file (a downloaded mod map: untrusted) -- tag indices are 16-bit, groups a few hundred
        if not (0 <= ngroups <= 4096 and 0 <= ntags <= 65536):
            raise ValueError('implausible tag table (%d groups, %d tags)' % (ngroups, ntags))
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
        if not (0 <= cnt <= 65536 and 0 <= size <= 64 << 20):
            raise ValueError('implausible tag name table (%d names, %d bytes)' % (cnt, size))
        self.f.seek((idx + dbg) & 0xFFFFFFFF); offs = struct.unpack('<%di' % cnt, self.f.read(cnt * 4))
        self.f.seek((off + dbg) & 0xFFFFFFFF); blob = self.f.read(size)
        self.names = [_cstr(blob, o) for o in offs]
        # string ids: index = sid & 0x7FFFF, namespace = (sid >> 19) & 0xFF; namespace sizes in the header (+0x40 count,
        # +0x44 offset); strings: +0x30 count, +0x34 data offset, +0x38 size, +0x3C index table (debug-section offsets)
        scount, sdata, ssize, sidx = struct.unpack_from('<4i', h, 0x30); nscount, nsoff = struct.unpack_from('<2i', h, 0x40)
        if not (0 <= scount <= 1 << 20 and 0 <= ssize <= 64 << 20 and 0 <= nscount <= 256):
            raise ValueError('implausible string id table (%d strings, %d bytes, %d namespaces)' % (scount, ssize, nscount))
        self.f.seek((nsoff + dbg) & 0xFFFFFFFF); ns = struct.unpack('<%dI' % nscount, self.f.read(nscount * 4))
        self.ns_start = [0]; [self.ns_start.append(self.ns_start[-1] + c) for c in ns]
        self.f.seek((sidx + dbg) & 0xFFFFFFFF); self.soffs = struct.unpack('<%di' % scount, self.f.read(scount * 4))
        self.f.seek((sdata + dbg) & 0xFFFFFFFF); self.sblob = self.f.read(ssize)
    def sid(self, v):
        if v == 0: return ''
        idx = v & 0x7FFFF; n = (v >> 19) & 0xFF
        g = (self.ns_start[n] + idx) if n < len(self.ns_start) - 1 and n > 0 else idx
        if n == 0 and len(self.ns_start) > 1 and idx >= self.ns_start[1]: g = idx
        if not (0 <= g < len(self.soffs)) or self.soffs[g] < 0: return '<%x>' % v
        return _cstr(self.sblob, self.soffs[g])
    def va_to_off(self, va): return self.tag_off + (va - self.vbase)
    def read_va(self, va, n): self.f.seek(self.va_to_off(va)); return self.f.read(n)
    def p(self, p32): return p32 << 2                      # expand a 32-bit tag pointer
    def read_p(self, p32, n): return self.read_va(self.p(p32), n)
    def meta(self, ti, n): return self.read_p(self.tags[ti][2], n)
    def block(self, data, off, esize):                     # tag block at data+off -> list of element bytes
        cnt, ptr = struct.unpack_from('<iI', data, off)
        if cnt <= 0 or not ptr: return []
        cnt = min(cnt, (BLOCK_MAX_BYTES // max(esize, 1)))   # a crafted count: never more than BLOCK_MAX_BYTES of reads
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
    elif sys.argv[2] == 'locations':   # named / positioned locations of the map's scenario
        sc = m.find('scnr')[0]; d = m.meta(sc, 0x480)
        print('scenario', m.names[sc])
        for e in m.block(d, 0x24C, 0x18):
            x, y, z, yaw = struct.unpack_from('<4f', e, 0); print('  player start    (%8.2f %8.2f %7.2f) facing %4.0f deg' % (x, y, z, yaw * 57.2958))
        for e in m.block(d, 0x428, 0x1C):
            print('  cutscene flag   %-28s (%8.2f %8.2f %7.2f)' % (m.sid(struct.unpack_from('<I', e, 0)[0]),) + struct.unpack_from('<3f', e, 4))
        for e in m.block(d, 0x258, 0x44):
            nm = m.sid(struct.unpack_from('<I', e, 0)[0]); print('  trigger volume  %-28s (%8.2f %8.2f %7.2f)' % ((nm,) + struct.unpack_from('<3f', e, 0x24 if len(e) > 0x30 else 0x14)))
        for e in m.block(d, 0xA8, 0x24):
            print('  object name     %s' % e[:0x20].split(b'\0')[0].decode('latin1'))
    elif sys.argv[2] == 'hex':
        ti = int(sys.argv[3], 0); n = int(sys.argv[4], 0) if len(sys.argv) > 4 else 0x100
        d = m.meta(ti, n)
        for o in range(0, n, 16): print('%04x ' % o + ' '.join('%08x' % struct.unpack_from('<I', d, o + k)[0] for k in range(0, 16, 4)))
