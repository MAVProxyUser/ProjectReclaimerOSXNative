#!/usr/bin/env python3
"""Top-down picture of a Halo 3 (MCC) multiplayer map, rendered from the collision geometry in its .map file.

    python3 re-tools/h3bsp.py <file.map> <out.png> [--px-per-unit N | --size N] [--bounds x0,y0,x1,y1] [...]

Writes out.png (RGBA, transparent where there is no geometry) and out.json:
    {"map", "x0", "y0", "x1", "y1", "px_per_unit", "w", "h", "zmin", "zmax", ...}
Pixel <-> world (orthographic, looking down -Z, world X to the right, world Y up):
    px = (x - x0) * px_per_unit        py = (y1 - y) * px_per_unit        (pixel (0,0) = top-left corner)
x1 = x0 + w / px_per_unit, y0 = y1 - h / px_per_unit. Same world units as object positions in memory.

Read-only, static file work. Format path (see shim/docs/h3-map-geometry.md):
  scnr "Structure BSPs" -> sbsp meta (+0x38C = datum of its structure_bsp_tag_resources resource,
  +0x1C0 = instanced geometry instances) -> zone tag resource gestalt entry (definition bytes in the zone's
  resource info buffer + fixups, segment index) -> play tag segment -> raw page (raw deflate, in the cache
  file's resource section) -> collision BSP (surfaces/edges/vertices) + instanced geometry definitions.
"""
import argparse, json, os, struct, sys, time, zlib

import numpy as np
from PIL import Image, ImageDraw

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from h3map import H3Map, _cstr  # noqa: E402

# ----------------------------------------------------------------------------------------------- resources
ADDR_DEFINITION, ADDR_PRIMARY, ADDR_SECONDARY = 1, 2, 3   # resource address = (type << 29) | offset


class Resources:
    """zone (cache_file_resource_gestalt) + play (cache_file_resource_layout_table), H3 MCC layouts."""

    def __init__(self, m):
        self.m = m
        z = m.meta(m.find('zone')[0], 0x220)
        self.types = m.block(z, 0x04, 0x1C)                 # resource types (guid, name sid at +0x18)
        self.entries = m.block(z, 0x64, 0x40)               # resource table
        isz, = struct.unpack_from('<i', z, 0x148); iaddr, = struct.unpack_from('<I', z, 0x154)
        self.info = m.read_p(iaddr, isz)                    # definition ("resource info") buffer
        play = m.find('play')
        if play and m.tags[play[0]][2]:                     # stock MCC maps: the layout table is the play tag
            pl = m.meta(play[0], 0x60)
            self.ext = m.block(pl, 0x0C, 0x108)             # external cache files (shared.map, ...)
            self.pages = m.block(pl, 0x18, 0x58)            # raw pages
            self.segs = m.block(pl, 0x3C, 0x10)             # raw segments
        else:                                               # maps built with the Halo 3 editing kit (most Workshop
            self.ext = m.block(z, 0x1C, 0x108)              # maps): an empty play tag, the same three tables live in
            self.pages = m.block(z, 0x34, 0x58)             # the zone tag itself
            self.segs = m.block(z, 0x58, 0x10)
        self.raw_off = (m.sections[1][0] + m.masks[1]) & 0xFFFFFFFF   # resource section file offset
        if not (play and m.tags[play[0]][2]): self.raw_off = 0   # editing-kit maps: page offsets are file offsets
        self._pages = {}

    def page(self, idx):
        if idx in self._pages:
            return self._pages[idx]
        salt, flags, codec, shared, _u, off, csz, usz = struct.unpack_from('<HBbhhiii', self.pages[idx], 0)
        if off < 0 or csz <= 0:                             # the empty page (editing-kit maps point unused halves at it)
            self._pages[idx] = b''
            return b''
        if shared != -1:
            raise NotImplementedError('resource page %d lives in external cache file %r' % (
                idx, self.ext[shared][:0x100].split(b'\0')[0]))
        self.m.f.seek(self.raw_off + off)
        raw = self.m.f.read(csz)
        if codec != -1:   # codec 0 = raw deflate; never past the page's own size (a deflate bomb in a downloaded map)
            if not 0 < usz <= PAGE_MAX:
                raise ValueError('resource page %d: implausible size %d' % (idx, usz))
            d = zlib.decompressobj(-15); data = d.decompress(raw, usz)
            if d.unconsumed_tail or len(data) != usz:
                raise ValueError('resource page %d: does not inflate to its size %d' % (idx, usz))
        else:
            data = raw
        self._pages[idx] = data
        return data

    def type_name(self, datum):
        rt, = struct.unpack_from('<b', self.entries[datum & 0xFFFF], 0x12)
        return sid_name(self.m, struct.unpack_from('<I', self.types[rt], 0x18)[0]) if 0 <= rt < len(self.types) else None

    def load(self, datum):
        """-> (definition bytes with fixups applied, offset of the definition struct, {addr type: bytes})"""
        e = self.entries[datum & 0xFFFF]
        ioff, isz = struct.unpack_from('<ii', e, 0x14)
        seg, = struct.unpack_from('<h', e, 0x22); base, = struct.unpack_from('<I', e, 0x24)
        defn = bytearray(self.info[ioff:ioff + isz])
        for f in self.m.block(e, 0x28, 8):                  # resource fixups: (offset in definition, address)
            o, a = struct.unpack('<iI', f)
            struct.pack_into('<I', defn, o, a)
        bufs = {ADDR_DEFINITION: bytes(defn)}
        if seg >= 0:
            pp, sp, po, so = struct.unpack_from('<hhii', self.segs[seg], 0)
            if pp >= 0 and po >= 0:
                bufs[ADDR_PRIMARY] = memoryview(self.page(pp))[po:]
            if sp >= 0 and so >= 0:
                bufs[ADDR_SECONDARY] = memoryview(self.page(sp))[so:]
        return bufs[ADDR_DEFINITION], base & 0x1FFFFFFF, bufs



PAGE_MAX = 256 << 20   # one resource page inflated at most (stock maps: a few MB)
TOOL_VERSION = 2   # 2 = Forge canvases framed by their geometry (2026-10-18 03:25); 1 = no tool_version field

def sid_name(m, v):
    """string id -> text for these builds: 19-bit index, 8-bit namespace at bit 19 (Assembly's MCC stringid layout).
    Namespace 0 indices past namespace 0's count continue after all namespaced strings (h3map.H3Map.sid gets that part
    wrong: e.g. the Warthog's "hull" damage section)."""
    idx, ns = v & 0x7FFFF, (v >> 19) & 0xFF
    if ns == 0:
        g = idx if idx < m.ns_start[1] else idx - m.ns_start[1] + m.ns_start[-1]
    else:
        g = m.ns_start[ns] + idx if ns < len(m.ns_start) else -1
    if not (0 <= g < len(m.soffs)) or m.soffs[g] < 0:
        return '<%x>' % v
    return _cstr(m.sblob, m.soffs[g])


def rblock(bufs, buf, off):
    """resource tag block (count, address, pad) -> (count, buffer, offset)"""
    cnt, addr = struct.unpack_from('<iI', buf, off)
    if cnt <= 0:
        return 0, None, 0
    return cnt, bufs[addr >> 29], addr & 0x1FFFFFFF


# ----------------------------------------------------------------------------------------------- collision
SURF_SMALL = np.dtype([('plane', '<u2'), ('first_edge', '<u2'), ('material', '<i2'), ('bss', '<i2'),
                       ('bs', '<i2'), ('flags', 'u1'), ('bpv', 'u1')])                       # 0xC
SURF_LARGE = np.dtype([('plane', '<i4'), ('first_edge', '<i4'), ('material', '<i2'), ('bss', '<i2'),
                       ('bs', '<i2'), ('flags', 'u1'), ('bpv', 'u1')])                       # 0x10
SF_TWO_SIDED, SF_INVISIBLE, SF_CLIMBABLE, SF_BREAKABLE = 1, 2, 4, 8


def collision_polys(bufs, buf, off, large=False):
    """collision_bsp / large_collision_bsp struct at buf+off -> (vertices Nx3, [vertex index lists], flags, materials).
    Blocks (12 bytes each): 3d nodes, planes, leaves, 2d refs, 2d nodes, surfaces (+0x3C), edges (+0x48), vertices (+0x54)."""
    ns, sb, so = rblock(bufs, buf, off + 0x3C)
    ne, eb, eo = rblock(bufs, buf, off + 0x48)
    nv, vb, vo = rblock(bufs, buf, off + 0x54)
    if not ns or not nv:
        return np.zeros((0, 3), np.float32), [], np.zeros(0, np.uint8), np.zeros(0, np.int16)
    surf = np.frombuffer(sb, SURF_LARGE if large else SURF_SMALL, ns, so)
    edges = np.frombuffer(eb, '<i4' if large else '<u2', ne * 6, eo).reshape(ne, 6).tolist()
    vstride = 5 if large else 4                           # vertex = point3 + first edge + sink (0x14 large / 0x10)
    verts = np.frombuffer(vb, '<f4', nv * vstride, vo).reshape(nv, vstride)[:, :3].astype(np.float64)
    polys = []
    for s, fe in enumerate(surf['first_edge'].tolist()):
        ring, ei = [], fe
        for _ in range(256):                               # walk the surface's edge ring
            st, en, fw, rv, left, _right = edges[ei]
            if left == s:
                ring.append(st); ei = fw
            else:
                ring.append(en); ei = rv
            if ei == fe:
                break
        polys.append(ring)
    return verts, polys, surf['flags'].copy(), surf['material'].copy()


def fan(polys):
    tris = [(p[0], p[i], p[i + 1]) for p in polys if len(p) >= 3 for i in range(1, len(p) - 1)]
    owner = [s for s, p in enumerate(polys) if len(p) >= 3 for i in range(1, len(p) - 1)]
    return np.array(tris, np.int64).reshape(-1, 3), np.array(owner, np.int64)


# ----------------------------------------------------------------------------------------------- level
def scenario_bsps(m):
    """sbsp tag indices the scenario uses (scnr +0x18 "Structure BSPs", 0x6C each, tagref at +0)."""
    out = []
    for sc in m.find('scnr'):
        d = m.meta(sc, 0x20)
        for e in m.block(d, 0x18, 0x6C):
            ti = m.tagref(e, 0)
            if ti >= 0 and ti not in out:
                out.append(ti)
    return out or m.find('sbsp')


def level_triangles(m, res, opts, log=print, with_flags=False):
    """-> world-space triangles (T,3,3) for every structure BSP + placed instance, a per-triangle kind
    (0 = structure BSP, 1 = instanced geometry) and a two-sided mask (+ the surface flags per triangle with
    with_flags). Triangles keep the surface's edge-ring winding, which is counter-clockwise around the surface
    plane normal (checked against the planes block)."""
    tri_list, kinds, two, fl = [], [], [], []
    for si in scenario_bsps(m):
        d = m.meta(si, 0x3A0)
        datum, = struct.unpack_from('<I', d, 0x38C)        # structure_bsp_tag_resources
        if datum == 0xFFFFFFFF:
            log('%s: no structure_bsp_tag_resources' % m.names[si]); continue
        if res.type_name(datum) != 'structure_bsp_tag_resources':
            log('%s: +0x38C resource is %r, not structure_bsp_tag_resources' % (m.names[si], res.type_name(datum)))
            continue
        defn, base, bufs = res.load(datum)
        if ADDR_PRIMARY not in bufs:
            log('%s: resource has no data page' % m.names[si]); continue

        def add(verts, polys, flags, mats, xf=None, kind=0, mirrored=False):
            keep = np.ones(len(polys), bool)
            if not opts.invisible:
                keep &= (flags & SF_INVISIBLE) == 0
            if not opts.unmaterialed:
                keep &= mats >= 0
            idx = np.nonzero(keep)[0]
            t, owner = fan([polys[i] for i in idx])
            if not len(t):
                return 0
            v = verts if xf is None else xf(verts)
            if mirrored:
                t = t[:, ::-1]
            tri_list.append(v[t]); kinds.append(np.full(len(t), kind, np.uint8))
            two.append((flags[idx[owner]] & SF_TWO_SIDED) != 0)
            fl.append(flags[idx[owner]])
            return len(t)

        n = 0
        for k, large in ((0x00, False), (0x0C, True)):     # collision bsps, large collision bsps (0x60 each)
            cnt, cb, co = rblock(bufs, defn, base + k)
            for i in range(cnt):
                n += add(*collision_polys(bufs, cb, co + 0x60 * i, large))
        log('%s: structure collision %d triangles' % (m.names[si], n))

        if opts.no_instances:
            continue
        cnt, ib, io = rblock(bufs, defn, base + 0x18)        # instanced geometry definitions (0xC4 each)
        defs = []
        for i in range(cnt):                                 # +0x14 = inline collision_bsp (0x60)
            defs.append(collision_polys(bufs, ib, io + 0xC4 * i + 0x14))
        ni = nt = skipped = 0
        for e in m.block(d, 0x1C0, 0x78):                    # instanced geometry instances
            scale, = struct.unpack_from('<f', e, 0)
            fwd, left, up, pos = (np.array(struct.unpack_from('<3f', e, o)) for o in (0x04, 0x10, 0x1C, 0x28))
            di, flags = struct.unpack_from('<hH', e, 0x34)
            if di < 0 or di >= len(defs) or ((flags & 2) and not opts.render_only):   # 2 = render only
                skipped += 1; continue
            basis = np.stack([fwd, left, up]) * scale        # p' = scale*(x*fwd + y*left + z*up) + pos
            # (explicit sum instead of v @ basis: Accelerate's matmul raises spurious FP warnings on macOS)
            nt += add(*defs[di], xf=lambda v, b=basis, p=pos: v[:, :1] * b[0] + v[:, 1:2] * b[1] + v[:, 2:] * b[2] + p,
                      kind=1, mirrored=np.linalg.det(basis) < 0); ni += 1
        log('%s: %d instance definitions, %d instances placed (%d skipped), %d triangles' % (
            m.names[si], len(defs), ni, skipped, nt))
    if not tri_list:
        raise SystemExit('no level geometry found')
    if with_flags:
        return np.concatenate(tri_list), np.concatenate(kinds), np.concatenate(two), np.concatenate(fl)
    return np.concatenate(tri_list), np.concatenate(kinds), np.concatenate(two)


# ----------------------------------------------------------------------------------------------- raster
def rasterize(tris, x0, y1, ppu, w, h):
    """highest z per pixel (z-buffer, -inf = empty). Pixel centers sampled; orthographic top-down."""
    zbuf = np.full(w * h, -np.inf)
    P = np.empty_like(tris)
    P[..., 0] = (tris[..., 0] - x0) * ppu
    P[..., 1] = (y1 - tris[..., 1]) * ppu
    P[..., 2] = tris[..., 2]
    ax, ay, az = P[:, 0, 0], P[:, 0, 1], P[:, 0, 2]
    bx, by, bz = P[:, 1, 0], P[:, 1, 1], P[:, 1, 2]
    cx, cy, cz = P[:, 2, 0], P[:, 2, 1], P[:, 2, 2]
    area = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax)
    ok = np.abs(area) > 1e-9                                     # vertical walls project to nothing
    c0 = np.ceil(np.minimum(np.minimum(ax, bx), cx) - 0.5).astype(np.int64)   # pixel centers at +0.5
    c1 = np.floor(np.maximum(np.maximum(ax, bx), cx) - 0.5).astype(np.int64)
    r0 = np.ceil(np.minimum(np.minimum(ay, by), cy) - 0.5).astype(np.int64)
    r1 = np.floor(np.maximum(np.maximum(ay, by), cy) - 0.5).astype(np.int64)
    c0 = np.clip(c0, 0, w); r0 = np.clip(r0, 0, h)
    c1 = np.clip(c1, -1, w - 1); r1 = np.clip(r1, -1, h - 1)
    ok &= (c1 >= c0) & (r1 >= r0)
    bw, bh = c1 - c0 + 1, r1 - r0 + 1
    side = np.maximum(bw, bh)
    idx = np.nonzero(ok)[0]
    # bucket triangles by bounding-box side (powers of two) and rasterize each bucket vectorized
    k = np.ones(len(idx), np.int64)
    while True:
        small = side[idx] > k
        if not small.any():
            break
        k[small] *= 2
    eps = 1e-7
    for kk in np.unique(k):
        sel = idx[k == kk]
        per = max(1, (1 << 21) // (kk * kk))                    # ~2M samples per chunk
        g = np.arange(kk)
        for s in range(0, len(sel), per):
            t = sel[s:s + per]
            px = (c0[t, None, None] + g[None, None, :]) + 0.5   # (n, 1, kk)
            py = (r0[t, None, None] + g[None, :, None]) + 0.5   # (n, kk, 1)
            A = area[t, None, None]
            w0 = ((bx[t, None, None] - px) * (cy[t, None, None] - py) - (by[t, None, None] - py) * (cx[t, None, None] - px)) / A
            w1 = ((cx[t, None, None] - px) * (ay[t, None, None] - py) - (cy[t, None, None] - py) * (ax[t, None, None] - px)) / A
            w2 = 1.0 - w0 - w1
            inside = (w0 >= -eps) & (w1 >= -eps) & (w2 >= -eps)
            inside &= (px < c1[t, None, None] + 1) & (py < r1[t, None, None] + 1)
            z = w0 * az[t, None, None] + w1 * bz[t, None, None] + w2 * cz[t, None, None]
            flat = (py.astype(np.int64) * w + px.astype(np.int64))
            flat, z = np.broadcast_arrays(flat, z)
            np.maximum.at(zbuf, flat[inside], z[inside])
    return zbuf.reshape(h, w)


def shifted(a, dy, dx, fill):
    """a shifted by (dy, dx) pixels, `fill` where nothing moved in (no wrap-around like np.roll)"""
    out = np.full_like(a, fill)
    h, w = a.shape
    out[max(dy, 0):h + min(dy, 0), max(dx, 0):w + min(dx, 0)] = a[max(-dy, 0):h + min(-dy, 0), max(-dx, 0):w + min(-dx, 0)]
    return out


def shade(zmap, ppu, zlo, zhi, zsample=None, ledge=0.3):
    """height (higher = lighter: 35 % linear zlo..zhi, 65 % histogram-equalised over `zsample` so a flat-ish
    floor still shows its relief) x hillshade from the z map gradient (light from the north-west); z steps
    bigger than `ledge` world units get a dark outline on the lower side. -> RGBA uint8, alpha 0 = no geometry"""
    have = np.isfinite(zmap)
    zf = np.where(have, zmap, np.nan)
    filled = np.where(have, zmap, zlo)
    gy, gx = np.gradient(filled, 1.0 / ppu)                        # dz per world unit; image rows go -Y
    gy = -gy
    nbrs = [(0, 1), (0, -1), (1, 0), (-1, 0)]
    inner = have.copy()                                            # all 4 neighbours have geometry (or are off-image)
    for dy, dx in nbrs:
        inner &= shifted(have, dy, dx, True)
    gx = np.where(inner, np.clip(gx, -8, 8), 0.0); gy = np.where(inner, np.clip(gy, -8, 8), 0.0)
    n = np.stack([-gx, -gy, np.ones_like(gx)]); n /= np.linalg.norm(n, axis=0)
    L = np.array([-1.0, 1.0, 1.6]); L /= np.linalg.norm(L)
    lam = np.clip(np.tensordot(L, n, 1), 0, 1) / L[2]                  # 1.0 on flat ground
    lin = np.clip((filled - zlo) / max(zhi - zlo, 1e-6), 0, 1)
    if zsample is not None and len(zsample) > 16:
        qs = np.unique(np.percentile(zsample, np.linspace(0, 100, 65)))
        eq = np.interp(filled, qs, np.linspace(0, 1, len(qs))) if len(qs) > 1 else lin
        t = 0.35 * lin + 0.65 * eq
    else:
        t = lin
    v = (0.16 + 0.78 * t) * (0.50 + 0.50 * lam)
    dmax = np.zeros_like(filled)
    for dy, dx in nbrs:
        nb = shifted(zf, dy, dx, np.nan)
        dmax = np.maximum(dmax, np.where(np.isfinite(nb), nb - zf, 0.0))
    v = np.where(dmax > ledge, v * 0.55, v)
    v = np.clip(v, 0, 1)
    g = (v * 255 + 0.5).astype(np.int32)
    rgba = np.stack([g, g, np.minimum(255, g + 8), np.where(have, 255, 0)], -1).astype(np.uint8)
    edge = have & ~inner                                               # outer edge of the geometry
    rgba[edge, :3] = (rgba[edge, :3] * 0.5).astype(np.uint8)
    return rgba


# ----------------------------------------------------------------------------------------------- play area
# scnr object placement blocks: (block offset, element size, palette offset); every placement has its position at +0x8
PLACEMENTS = {'scenery': (0xB4, 0xB4, 0xC0), 'vehicles': (0xE4, 0xA8, 0xF0), 'equipment': (0xFC, 0x8C, 0x108),
              'weapons': (0x114, 0xA8, 0x120)}


PLAY_PREFIXES = ('objects\\multi\\', 'objects\\weapons\\', 'objects\\vehicles\\', 'objects\\equipment\\')


def play_points(m):
    """world points that mark where multiplayer happens: spawn points and game-mode markers (scenery from
    objects\multi\...), stock weapon / vehicle / equipment placements and player starting locations.
    Level-specific objects (Sandbox's / Sandtrap's out-of-bounds "defender" turrets, wreck scenery, cinematic
    frigates) are left out."""
    pts = []
    for sc in m.find('scnr'):
        d = m.meta(sc, 0x258)
        for kind, (off, esz, pal) in PLACEMENTS.items():
            palette = [m.tagref(e, 0) for e in m.block(d, pal, 0x10)]
            for e in m.block(d, off, esz):
                pi, = struct.unpack_from('<h', e, 0)
                ti = palette[pi] if 0 <= pi < len(palette) else -1
                if ti >= 0 and m.names[ti].startswith(PLAY_PREFIXES[:1] if kind == 'scenery' else PLAY_PREFIXES):
                    pts.append(struct.unpack_from('<3f', e, 8))
        pts += [struct.unpack_from('<3f', e, 0) for e in m.block(d, 0x24C, 0x18)]
    return np.array(pts, np.float64).reshape(-1, 3)


def geometry_core(tris, lo=0.005, hi=0.995):
    """area-weighted lo..hi quantiles of triangle centroids in x and y (drops sparse far-away skyline pieces)"""
    cen = tris.mean(1)
    e1 = tris[:, 1] - tris[:, 0]; e2 = tris[:, 2] - tris[:, 0]
    a = np.abs(e1[:, 0] * e2[:, 1] - e1[:, 1] * e2[:, 0]) + 1e-9
    out = []
    for col in (0, 1):
        o = np.argsort(cen[:, col]); cw = np.cumsum(a[o]); cw /= cw[-1]
        out.append((cen[o[np.searchsorted(cw, lo)], col], cen[o[min(np.searchsorted(cw, hi), len(o) - 1)], col]))
    return np.array([out[0][0], out[1][0]]), np.array([out[0][1], out[1][1]])


def top_z_at(tris, pts, ppu=4.0):
    """highest surface z straight above/below each point's (x, y) (-inf where there is nothing)"""
    lo = pts[:, :2].min(0) - 1; hi = pts[:, :2].max(0) + 1
    sel = ((tris[:, :, 0].max(1) >= lo[0]) & (tris[:, :, 0].min(1) <= hi[0]) &
           (tris[:, :, 1].max(1) >= lo[1]) & (tris[:, :, 1].min(1) <= hi[1]))
    w = int(np.ceil((hi[0] - lo[0]) * ppu)); h = int(np.ceil((hi[1] - lo[1]) * ppu))
    zm = rasterize(tris[sel], lo[0], hi[1], ppu, w, h)
    c = np.clip(((pts[:, 0] - lo[0]) * ppu).astype(int), 0, w - 1)
    r = np.clip(((hi[1] - pts[:, 1]) * ppu).astype(int), 0, h - 1)
    return zm[r, c]


PLAY_RADIUS = 6.0      # world units around play points whose heights set the shading range
ROOF_CLEARANCE = 1.5   # a surface this far above a spawn point / item = it is under a roof (Spartan ~0.7 units tall)
CUT_HEADROOM = 2.5     # auto z cut = highest spawn point / item + this
SAFE_SLACK = 5.0       # placements this far outside the safe zones' box are unreachable leftovers


def auto_zcut(tris, pts, log):
    """Indoor maps (Construct, Foundry, ...): the roof / upper slabs hide every floor. If cutting away everything
    that starts above the highest spawn point / item un-roofs a good share of the points, do it."""
    if len(pts) < 4:
        return None, None
    zt = top_z_at(tris, pts)
    roofed0 = float(np.mean(zt > pts[:, 2] + ROOF_CLEARANCE))
    cut = float(pts[:, 2].max()) + CUT_HEADROOM
    zt1 = top_z_at(tris[tris[:, :, 2].min(1) <= cut], pts)
    roofed1 = float(np.mean(zt1 > pts[:, 2] + ROOF_CLEARANCE))
    log('roofed spawn points / items: %.0f%%; with a z cut at %.2f: %.0f%%' % (100 * roofed0, cut, 100 * roofed1))
    return (cut if roofed0 - roofed1 >= 0.15 else None), (roofed0, roofed1)


# ----------------------------------------------------------------------------------------------- main
def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('map'); ap.add_argument('out')
    ap.add_argument('--px-per-unit', type=float, help='pixels per world unit (overrides --size)')
    ap.add_argument('--size', type=int, default=2048, help='longest image side in pixels (default 2048)')
    ap.add_argument('--bounds', help='x0,y0,x1,y1 world rectangle to render (overrides --crop); write '
                                     '--bounds=-10,-20,30,40 when x0 is negative')
    ap.add_argument('--crop', choices=('play', 'geometry', 'all'), default='play',
                    help='play (default): box around spawn points / weapons / vehicles / mode markers + --margin; '
                         'geometry: all level geometry minus sparse far-off pieces; all: every triangle')
    ap.add_argument('--margin', type=float, default=16.0,
                    help='world units added around the play box: max(this, 25%% of the box\'s longer side) (default 16)')
    ap.add_argument('--no-instances', action='store_true', help='structure BSP only')
    ap.add_argument('--render-only', action='store_true', help='also draw instances flagged render-only')
    ap.add_argument('--invisible', action='store_true', help='also draw invisible collision (player clip)')
    ap.add_argument('--unmaterialed', action='store_true',
                    help='also draw surfaces without a collision material (sky / ceiling blockers)')
    ap.add_argument('--no-cull', action='store_true', help='keep downward-facing surfaces (ceilings) too')
    ap.add_argument('--zcut', default='auto',
                    help="drop triangles whose lowest point is above this z: a number, 'off', or 'auto' (default: "
                         "cut just above the highest spawn point / item when that un-roofs an indoor map)")
    ap.add_argument('-q', '--quiet', action='store_true')
    opts = ap.parse_args()
    log = (lambda *a: None) if opts.quiet else (lambda *a: print(*a, file=sys.stderr))
    t0 = time.time()

    m = H3Map(opts.map)
    res = Resources(m)
    tris, kinds, two = level_triangles(m, res, opts, log)
    if not opts.no_cull:          # drop surfaces that face down (ceilings, undersides): never seen from above
        nrm = np.cross(tris[:, 1] - tris[:, 0], tris[:, 2] - tris[:, 0])
        up = nrm[:, 2] > 0.02 * np.linalg.norm(nrm, axis=1)
        keep = up | two
        log('culled %d of %d triangles that face down / sideways' % ((~keep).sum(), len(tris)))
        tris, kinds = tris[keep], kinds[keep]
    pts = play_points(m)
    roofed = None
    all_tris = tris                      # before the z cut (the teleport height grid wants cliff tops and roofs too)
    if opts.zcut == 'auto':
        zcut, roofed = auto_zcut(tris, pts, log)
    else:
        zcut = None if opts.zcut == 'off' else float(opts.zcut)
    if zcut is not None:
        keep = tris[:, :, 2].min(1) <= zcut
        log('z cut at %.2f: dropped %d triangles' % (zcut, (~keep).sum()))
        tris, kinds = tris[keep], kinds[keep]
    log('%d triangles total (%.1fs)' % (len(tris), time.time() - t0))

    glo, ghi = tris[:, :, :2].reshape(-1, 2).min(0), tris[:, :, :2].reshape(-1, 2).max(0)
    core = (pts[:, :2].min(0), pts[:, :2].max(0)) if len(pts) >= 2 else geometry_core(tris)
    # the play crop also covers the scenario's safe zones = the real playable area (being outside every one kills): on
    # big maps (Sandtrap, Sandbox) players drive far past the spawns and weapons, and the stats page's heatmap and paths
    # ran off the picture's edge (re-tools/h3bounds.py reads the trigger volumes)
    safe_box = None
    try:
        import h3bounds
        sc = m.find('scnr')
        if sc:
            tvs, _, _ = h3bounds.read_trigger_volumes(m, m.meta(sc[0], 0x800))
            boxes = [tv['aabb'] for tv in tvs if tv['kind'] == 'safe']
            if boxes:
                safe_box = (np.array([min(b['x'][0] for b in boxes), min(b['y'][0] for b in boxes)]),
                            np.array([max(b['x'][1] for b in boxes), max(b['y'][1] for b in boxes)]))
    except Exception as e:      # the picture still works without it
        log('safe zones not read: %s' % e)
    if safe_box is not None:
        # placements far outside every safe zone can't be reached (The Pit's stock Warthog sits behind the north wall
        # at y=32): leave them out of the crop and the shading range
        far = ((pts[:, 0] < safe_box[0][0] - SAFE_SLACK) | (pts[:, 0] > safe_box[1][0] + SAFE_SLACK) |
               (pts[:, 1] < safe_box[0][1] - SAFE_SLACK) | (pts[:, 1] > safe_box[1][1] + SAFE_SLACK))
        if far.any() and (~far).sum() >= 2:
            log('%d placement(s) more than %.0f units outside the safe zones left out: %s' % (int(far.sum()), SAFE_SLACK,
                ', '.join('(%.1f, %.1f)' % (x, y) for x, y in pts[far][:, :2])))
            pts = pts[~far]
            core = (pts[:, :2].min(0), pts[:, :2].max(0))
        core = (np.maximum(np.minimum(core[0], safe_box[0]), glo), np.minimum(np.maximum(core[1], safe_box[1]), ghi))
    if safe_box is None and len(pts) >= 2:
        # no safe zones and the placements cover a sliver of the map (a Forge canvas: Forge World's default objects span
        # 26 x 153 units of a ~255 x 460 play area; the stats page's heatmap ran far off the picture): the bulk of the
        # geometry (1st..99th percentile of the vertices, as re-tools/h3nav.py frames its graph) joins the crop
        c = tris[:, :, :2].reshape(-1, 2); plo, phi = np.percentile(c, 1, 0), np.percentile(c, 99, 0)
        if np.prod(core[1] - core[0]) < 0.2 * np.prod(phi - plo):
            log('placements cover %.0f of %.0f square units: framing the bulk of the geometry too' % (np.prod(core[1] - core[0]), np.prod(phi - plo)))
            core = (np.minimum(core[0], plo), np.maximum(core[1], phi))
    if opts.bounds:
        x0, y0, x1, y1 = (float(v) for v in opts.bounds.split(','))
        crop = 'bounds'
    else:
        crop = opts.crop if (opts.crop != 'play' or len(pts) >= 2) else 'geometry'
        if crop == 'all':
            lo, hi = glo, ghi
        elif crop == 'geometry':
            lo, hi = geometry_core(tris)
            lo, hi = lo - 2, hi + 2
        else:
            mg = max(opts.margin, 0.25 * float(np.max(core[1] - core[0])))
            lo = np.maximum(core[0] - mg, glo); hi = np.minimum(core[1] + mg, ghi)
        (x0, y0), (x1, y1) = lo, hi
    ppu = opts.px_per_unit or opts.size / max(x1 - x0, y1 - y0)
    w = int(np.ceil((x1 - x0) * ppu - 1e-6)); h = int(np.ceil((y1 - y0) * ppu - 1e-6))
    x1 = x0 + w / ppu; y0 = y1 - h / ppu                        # exact pixel grid
    log('crop %s: x %.2f..%.2f y %.2f..%.2f, %.3f px/unit -> %dx%d' % (crop, x0, x1, y0, y1, ppu, w, h))

    zmap = rasterize(tris, x0, y1, ppu, w, h)
    have = np.isfinite(zmap)
    if not have.any():
        raise SystemExit('nothing inside the bounds')
    # shade heights over the range of the floors players use: z samples within PLAY_RADIUS of the play points
    # (cliffs / skyline above that range saturate to light grey, which reads as "high, out of the way")
    zc = np.zeros(0)
    if len(pts):
        mask = Image.new('L', (w, h), 0); dr = ImageDraw.Draw(mask); rr = PLAY_RADIUS * ppu
        for x, y in pts[:, :2]:
            px, py = (x - x0) * ppu, (y1 - y) * ppu
            dr.ellipse([px - rr, py - rr, px + rr, py + rr], fill=255)
        zc = zmap[(np.asarray(mask) > 0) & have]
    if zc.size < 100:
        zc = zmap[have]
    zlo, zhi = float(np.percentile(zc, 2)), float(np.percentile(zc, 99))
    if len(pts):      # clamp to the spawn/item heights: pits below go dark, cliffs above go light
        zpmin, zpmax = float(pts[:, 2].min()), float(pts[:, 2].max())
        zlo = min(max(zlo, zpmin - 3.0), zpmin - 0.5); zhi = max(min(zhi, zpmax + 6.0), zpmax + 1.0)
    img = shade(zmap, ppu, zlo, zhi, zc)
    Image.fromarray(img).save(opts.out)
    zv = zmap[have]
    meta = {'map': os.path.splitext(os.path.basename(opts.map))[0],
            'x0': round(x0, 4), 'y0': round(y0, 4), 'x1': round(x1, 4), 'y1': round(y1, 4),
            'px_per_unit': ppu, 'w': w, 'h': h, 'zmin': round(zlo, 3), 'zmax': round(zhi, 3),
            'z_in_image': [round(float(zv.min()), 3), round(float(zv.max()), 3)],
            'play_box': [round(float(v), 3) for v in (core[0][0], core[0][1], core[1][0], core[1][1])],
            'safe_zone_box': None if safe_box is None else [round(float(v), 3) for v in (safe_box[0][0], safe_box[0][1], safe_box[1][0], safe_box[1][1])],
            'geometry_box': [round(float(v), 3) for v in (glo[0], glo[1], ghi[0], ghi[1])],
            'crop': crop, 'zcut': None if zcut is None else round(zcut, 3),
            'roofed_points': None if roofed is None else [round(v, 3) for v in roofed],
            'pixel': 'px = (x - x0) * px_per_unit, py = (y1 - y) * px_per_unit; top-left origin; +x right, +y up; '
                     'brightness = height (zmin dark .. zmax light)',
            'source': 'collision BSP%s' % ('' if opts.no_instances else ' + instanced geometry')}
    # ground-height grid for teleporting (the stats page's "teleport here"): the TOP surface of every upward-facing
    # triangle, no z cut (so cliff tops outside the play area count), 1 world unit per cell, over the geometry box.
    # <map>.z.bin = int16 little-endian, z * 10, -32768 = nothing; row 0 = the north edge (y = gy1), column 0 = gx0.
    gx0, gy0, gx1, gy1 = float(glo[0]), float(glo[1]), float(ghi[0]), float(ghi[1])
    gw, gh = int(np.ceil(gx1 - gx0)), int(np.ceil(gy1 - gy0))
    if 0 < gw * gh <= 4_000_000:
        zg = rasterize(all_tris, gx0, gy1, 1.0, gw, gh)
        zi = np.where(np.isfinite(zg), np.clip(np.round(zg * 10), -32767, 32767), -32768).astype('<i2')
        zpath = os.path.splitext(opts.out)[0] + '.z.bin'
        zi.tofile(zpath)
        meta['zgrid'] = {'file': os.path.basename(zpath), 'x0': round(gx0, 4), 'y1': round(gy1, 4), 'cell': 1.0, 'w': gw, 'h': gh,
                         'format': 'int16 LE z*10, -32768 none; row 0 = y1 (north), col 0 = x0; top surface, no z cut'}
    # bump TOOL_VERSION when the picture's framing or content changes: the loader (stats.c st_maptop_ensure) makes a picture
    # with an older (or no) tool_version again the next time the map is played
    meta['tool_version'] = TOOL_VERSION
    jpath = os.path.splitext(opts.out)[0] + '.json'
    with open(jpath, 'w') as f:
        json.dump(meta, f, indent=1)
    log('wrote %s + %s (%.1fs)' % (opts.out, jpath, time.time() - t0))


if __name__ == '__main__':
    main()
