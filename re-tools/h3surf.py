#!/usr/bin/env python3
"""What a biped stands on at (x, y) on a Halo 3 (MCC) map: every collision surface and soft ceiling straight above /
below the point, read from the .map file (static; nothing from a running game).

    python3 re-tools/h3surf.py <file.map> x,y [x,y ...]
    python3 re-tools/h3surf.py <file.map> --slip          # summary of slip-flagged collision surfaces

Per point, highest first: z, source (structure BSP / instance), collision surface flags (two-sided 0x1,
invisible 0x2, climbable 0x4, breakable 0x8, slip 0x40), material index, slope; then each soft ceiling
triangle there (name, type, z). A visible surface with the slip flag (0x40) is never ground for a biped on a
multiplayer map (halo3+0x219c1b, shim/docs/map-boundaries.md "Soft ceilings as ground"), so the biped slides there
and cannot walk. Uses h3bsp.py / h3bounds.py (numpy + Pillow).
"""
import os, struct, sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from h3map import H3Map  # noqa: E402
from h3bsp import Resources, rblock, collision_polys, scenario_bsps  # noqa: E402
from h3bounds import read_soft_ceilings  # noqa: E402

FLAG_NAMES = ((0x1, 'two-sided'), (0x2, 'invisible'), (0x4, 'climbable'), (0x8, 'breakable'), (0x10, 'invalid'),
              (0x20, 'conveyor'), (0x40, 'slip'))


def surfaces(m, res):
    """-> list of (triangle 3x3, surface flags, material, 'bsp' | 'instance') for every structure BSP + instance"""
    out = []
    for si in scenario_bsps(m):
        d = m.meta(si, 0x3A0)
        datum, = struct.unpack_from('<I', d, 0x38C)
        if datum == 0xFFFFFFFF:
            continue
        defn, base, bufs = res.load(datum)

        def add(verts, polys, flags, mats, xf=None, kind='bsp'):
            v = verts if xf is None else xf(verts)
            for s, p in enumerate(polys):
                for i in range(1, len(p) - 1):
                    out.append((v[[p[0], p[i], p[i + 1]]], int(flags[s]), int(mats[s]), kind))

        for k, large in ((0x00, False), (0x0C, True)):
            cnt, cb, co = rblock(bufs, defn, base + k)
            for i in range(cnt):
                add(*collision_polys(bufs, cb, co + 0x60 * i, large))
        cnt, ib, io = rblock(bufs, defn, base + 0x18)
        defs = [collision_polys(bufs, ib, io + 0xC4 * i + 0x14) for i in range(cnt)]
        for e in m.block(d, 0x1C0, 0x78):
            scale, = struct.unpack_from('<f', e, 0)
            fwd, left, up, pos = (np.array(struct.unpack_from('<3f', e, o)) for o in (0x04, 0x10, 0x1C, 0x28))
            di, fl = struct.unpack_from('<hH', e, 0x34)
            if di < 0 or di >= len(defs) or (fl & 2):          # 2 = render only
                continue
            b = np.stack([fwd, left, up]) * scale
            add(*defs[di], xf=lambda v, b=b, p=pos: v[:, :1] * b[0] + v[:, 1:2] * b[1] + v[:, 2:] * b[2] + p,
                kind='instance')
    return out


def z_at(t, x, y):
    (x1, y1, z1), (x2, y2, z2), (x3, y3, z3) = t
    den = (y2 - y3) * (x1 - x3) + (x3 - x2) * (y1 - y3)
    if abs(den) < 1e-9:
        return None
    a = ((y2 - y3) * (x - x3) + (x3 - x2) * (y - y3)) / den
    b = ((y3 - y1) * (x - x3) + (x1 - x3) * (y - y3)) / den
    if min(a, b, 1 - a - b) < -1e-6:
        return None
    return a * z1 + b * z2 + (1 - a - b) * z3


def flag_text(f):
    return '|'.join(n for b, n in FLAG_NAMES if f & b) or '-'


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    m = H3Map(sys.argv[1])
    surf = surfaces(m, Resources(m))
    if sys.argv[2] == '--slip':
        sl = [(t, f, mt, k) for t, f, mt, k in surf if f & 0x40]
        print('%d collision triangles, %d with the slip flag (0x40)' % (len(surf), len(sl)))
        by = {}
        for t, f, mt, k in sl:
            lo, hi = t.min(0), t.max(0)
            key = (f, mt, k)
            n, a, b = by.get(key, (0, lo, hi))
            by[key] = (n + 1, np.minimum(a, lo), np.maximum(b, hi))
        for (f, mt, k), (n, lo, hi) in sorted(by.items()):
            print('  flags 0x%02x (%s) material %d %s: %d triangles, x %.1f..%.1f y %.1f..%.1f z %.1f..%.1f' % (
                f, flag_text(f), mt, k, n, lo[0], hi[0], lo[1], hi[1], lo[2], hi[2]))
        return
    sc = m.find('scnr')
    softs = read_soft_ceilings(m, m.meta(sc[0], 0x800), keep_tris=False) if sc else []
    for arg in sys.argv[2:]:
        x, y = (float(v) for v in arg.split(','))
        rows = []
        for t, f, mt, k in surf:
            z = z_at(t, x, y)
            if z is None:
                continue
            n = np.cross(t[1] - t[0], t[2] - t[0]); n /= (np.linalg.norm(n) or 1)
            rows.append((z, 'surface %-8s flags 0x%02x %-22s material %3d slope %4.1f deg%s' % (
                k, f, flag_text(f), mt, np.degrees(np.arccos(min(1.0, abs(n[2])))), '' if n[2] > 0 else ' (faces down)')))
        for s in softs:
            for t in s['_tris']:
                z = z_at(t, x, y)
                if z is not None:
                    rows.append((z, 'soft ceiling %r (%s%s)' % (s['name'], s['type_name'],
                                                                ', ignores ' + '/'.join(s['ignores']) if s['ignores'] else '')))
        print('== (%g, %g)' % (x, y))
        for z, txt in sorted(rows, key=lambda r: -r[0]):
            print('  z %9.3f  %s' % (z, txt))


if __name__ == '__main__':
    main()
