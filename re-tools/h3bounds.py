#!/usr/bin/env python3
"""Map boundaries of a Halo 3 (MCC) map, read from its .map file: kill / safe trigger volumes, soft ceilings
(soft kill, acceleration, slip), invisible collision (player clip) and the structure BSP world bounds, drawn as a
top-down overlay that lines up pixel-for-pixel with an h3bsp.py picture.

    python3 re-tools/h3bounds.py <file.map> <out.png> [--grid <h3bsp out.json>] [--composite <file.png>] [--no-triangles]

--grid        use that h3bsp JSON's pixel grid (x0, y1, px_per_unit, w, h). Without it, h3bsp.py runs first (same
              python, default options) and writes <out-stem>_base.png/.json next to out.png; its grid is used.
--composite   also write the base picture with the overlay alpha-composited on top (dark background behind).
--no-triangles  leave the soft-ceiling triangle lists out of the JSON (counts / bboxes stay).

Writes out.png (RGBA overlay, transparent background, grid size) and out.json (same stem):
    {"map", "grid", "engine_limit", "world_bounds", "kill_volumes", "safe_zones", "trigger_volumes",
     "soft_ceilings", "invisible_surfaces"}
Pixel <-> world exactly as h3bsp.py: px = (x - x0) * px_per_unit, py = (y1 - y) * px_per_unit (top-left origin).
Overlay: cyan = invisible collision (filled = walkable-ish / sloped, solid 2 px lines = near-vertical walls);
soft ceilings red (soft kill) / orange (acceleration) / yellow (slip); kill trigger volumes = red 3 px outline +
red hatch; safe zones = green dashed outline; other trigger volumes = thin grey; world bounds = white.

Read-only, static file work. Layouts (Halo 3 MCC, Assembly Halo3MCC plugins; see shim/docs/h3-map-geometry.md):
  scnr +0x258 trigger volumes (0x44: +0 name sid, +4 s16 object name, +6 s16 node, +8 node name sid, +0xC forward,
       +0x18 up, +0x24 position = a CORNER of the box, +0x30 extents, +0x3C half diagonal, +0x40 s16 kill trigger
       index, +0x42 s16 editor folder). Box = position + a*forward + b*left + c*up, a,b,c in [0, extents],
       left = cross(up, forward).
  scnr +0x484 Scenario Kill Triggers / +0x490 Scenario Safe Zone Triggers (s16 trigger volume index each).
  scnr +0x234 soft ceilings (0xC: u16 flags = ignore bipeds/vehicles/camera/huge vehicles, u16 runtime flags,
       +4 name sid, +8 s16 type 0 acceleration / 1 soft kill / 2 slip surface).
  scnr +0x18 structure BSPs (0x6C: sbsp tagref +0, structure_design tagref +0x10).
  sddt +0x10 soft ceilings (0x14: +0 name sid, +4 s16 type, +8 triangles 0x44 each: plane +0, sphere +0x10,
       vertices +0x20 / +0x2C / +0x38). Matched to the scnr entries by name sid.
  sbsp +0x40 world bounds (x, y, z min/max); +0x28 collision materials (0x18: render method tagref, s16 global
       material +0x10 -> matg +0x464 materials, 0x170 each, name sid +0); collision surface flag 2 = invisible.
  The engine rejects object positions with |coord| > 32768 (object_set_position) -> "engine_limit".
"""
import argparse, json, math, os, struct, subprocess, sys, time

import numpy as np
from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from h3map import H3Map  # noqa: E402
from h3bsp import Resources, rblock, collision_polys, scenario_bsps, sid_name, SF_INVISIBLE  # noqa: E402

ENGINE_LIMIT = 32768.0
SF_SLIP = 0x40
SOFT_TYPES = {0: 'acceleration', 1: 'soft_kill', 2: 'slip_surface'}
SOFT_IGNORES = ('bipeds', 'vehicles', 'camera', 'huge_vehicles')     # scnr soft ceiling flag bits 0..3
VERTICAL_NZ = 0.2        # |normal.z| below this (slope steeper than ~78 deg) = a wall: drawn as its projected outline

# colours (RGB) and alphas of the overlay
C_INVIS = (0, 230, 255)
C_SOFT = {'soft_kill': (255, 40, 40), 'acceleration': (255, 150, 20), 'slip_surface': (255, 235, 40)}
WHERE = {'in': 'in the image', 'partly': 'partly in the image', 'encloses': 'encloses the image',
         'out': 'outside the image'}
C_KILL, C_SAFE, C_OTHER, C_BOUNDS = (255, 40, 40), (40, 230, 70), (170, 170, 170), (255, 255, 255)


# ----------------------------------------------------------------------------------------------- reading
def fin(v, nd=3):
    """round a float for JSON; non-finite -> None (keeps the JSON valid)"""
    v = float(v)
    return round(v, nd) if math.isfinite(v) else None


def vec(a, nd=3):
    return [fin(x, nd) for x in a]


def hull2d(pts):
    """convex hull of 2-D points, counter-clockwise (x right, y up), no repeated first point"""
    p = sorted(set((float(x), float(y)) for x, y in pts))
    if len(p) <= 2:
        return p

    def cross(o, a, b):
        return (a[0] - o[0]) * (b[1] - o[1]) - (a[1] - o[1]) * (b[0] - o[0])
    lo, hi = [], []
    for q in p:
        while len(lo) >= 2 and cross(lo[-2], lo[-1], q) <= 1e-12:
            lo.pop()
        lo.append(q)
    for q in reversed(p):
        while len(hi) >= 2 and cross(hi[-2], hi[-1], q) <= 1e-12:
            hi.pop()
        hi.append(q)
    return lo[:-1] + hi[:-1]


def global_material_names(m):
    g = m.find('matg')
    if not g:
        return []
    return [sid_name(m, struct.unpack_from('<I', e, 0)[0]) for e in m.block(m.meta(g[0], 0x480), 0x464, 0x170)]


def read_trigger_volumes(m, d):
    kills = [struct.unpack('<h', e)[0] for e in m.block(d, 0x484, 2)]
    safes = [struct.unpack('<h', e)[0] for e in m.block(d, 0x490, 2)]
    out = []
    for i, e in enumerate(m.block(d, 0x258, 0x44)):
        nsid, obj, node, node_sid = struct.unpack_from('<IhhI', e, 0)
        fwd, up, pos, ext = (np.array(struct.unpack_from('<3f', e, o), np.float64) for o in (0x0C, 0x18, 0x24, 0x30))
        half_diag, = struct.unpack_from('<f', e, 0x3C)
        kti, folder = struct.unpack_from('<hh', e, 0x40)
        name = sid_name(m, nsid) if nsid else ''
        left = np.cross(up, fwd)
        corners = np.array([pos + a * ext[0] * fwd + b * ext[1] * left + c * ext[2] * up
                            for c in (0, 1) for b in (0, 1) for a in (0, 1)])
        why = []
        if i in kills:
            why.append('scenario_kill_triggers')
        if kti >= 0:
            why.append('kill_trigger_index')
        if name.lower().startswith(('kill', 'playerkill')):
            why.append('name')
        kind = 'kill' if why else 'other'
        if kind == 'other':
            if i in safes:
                why.append('scenario_safe_zone_triggers')
            if name.lower().startswith('safe_zone'):
                why.append('name')
            kind = 'safe' if why else 'other'
        lo, hi = corners.min(0), corners.max(0)
        out.append({'index': i, 'name': name, 'kind': kind, 'reason': why,
                    'position': vec(pos), 'forward': vec(fwd, 4), 'left': vec(left, 4), 'up': vec(up, 4),
                    'extents': vec(ext), 'center': vec(pos + 0.5 * (ext[0] * fwd + ext[1] * left + ext[2] * up)),
                    'z_range': [fin(lo[2]), fin(hi[2])],
                    'aabb': {'x': [fin(lo[0]), fin(hi[0])], 'y': [fin(lo[1]), fin(hi[1])], 'z': [fin(lo[2]), fin(hi[2])]},
                    'footprint': [[fin(x), fin(y)] for x, y in hull2d(corners[:, :2])],
                    'corners': [vec(c) for c in corners],
                    'half_diagonal': fin(half_diag), 'kill_trigger_index': kti, 'editor_folder': folder,
                    'object_name_index': obj, 'node_index': node,
                    'node_name': sid_name(m, node_sid) if node_sid else ''})
    return out, kills, safes


def read_soft_ceilings(m, d, keep_tris=True):
    scn = {}
    for e in m.block(d, 0x234, 0xC):
        fl, rf, nsid, ty = struct.unpack_from('<HHIh', e, 0)
        scn[nsid] = (fl, rf, ty)
    out, seen = [], set()
    for be in m.block(d, 0x18, 0x6C):
        sb, sd = m.tagref(be, 0), m.tagref(be, 0x10)
        if sd < 0:
            continue
        for c in m.block(m.meta(sd, 0x40), 0x10, 0x14):
            nsid, ty = struct.unpack_from('<Ih', c, 0)
            raw = b''.join(m.block(c, 8, 0x44))
            n = len(raw) // 0x44
            tris = np.frombuffer(raw, '<f4').reshape(n, 17)[:, 8:17].reshape(n, 3, 3).astype(np.float64) if n else \
                np.zeros((0, 3, 3))
            fl, rf, sty = scn.get(nsid, (None, None, None))
            seen.add(nsid)
            v = tris.reshape(-1, 3)
            e = {'name': sid_name(m, nsid), 'type': ty, 'type_name': SOFT_TYPES.get(ty, 'type%d' % ty),
                 'flags': fl, 'runtime_flags': rf,
                 'ignores': [] if fl is None else [nm for b, nm in enumerate(SOFT_IGNORES) if fl & (1 << b)],
                 'scenario_type': sty, 'in_scenario': fl is not None, 'bsp': m.names[sb] if sb >= 0 else None,
                 'design': m.names[sd], 'triangles': n,
                 'bbox': vec(np.concatenate([v.min(0), v.max(0)])) if n else None, '_tris': tris}
            if keep_tris:
                e['tris'] = [[vec(p, 2) for p in t] for t in tris]
            out.append(e)
    for nsid, (fl, rf, ty) in scn.items():      # scenario entries without geometry in any structure_design
        if nsid not in seen:
            out.append({'name': sid_name(m, nsid), 'type': ty, 'type_name': SOFT_TYPES.get(ty, 'type%d' % ty),
                        'flags': fl, 'runtime_flags': rf,
                        'ignores': [nm for b, nm in enumerate(SOFT_IGNORES) if fl & (1 << b)],
                        'scenario_type': ty, 'in_scenario': True, 'bsp': None, 'design': None, 'triangles': 0,
                        'bbox': None, '_tris': np.zeros((0, 3, 3))})
    return out


def read_world_bounds(m):
    out = []
    for si in scenario_bsps(m):
        b = struct.unpack_from('<6f', m.meta(si, 0x60), 0x40)
        out.append({'bsp': m.names[si], 'x': [fin(b[0]), fin(b[1])], 'y': [fin(b[2]), fin(b[3])],
                    'z': [fin(b[4]), fin(b[5])]})
    return out


def newell(p):
    """polygon normal (unnormalised) of an Nx3 ring"""
    q = np.roll(p, -1, 0)
    return np.array([np.sum((p[:, 1] - q[:, 1]) * (p[:, 2] + q[:, 2])),
                     np.sum((p[:, 2] - q[:, 2]) * (p[:, 0] + q[:, 0])),
                     np.sum((p[:, 0] - q[:, 0]) * (p[:, 1] + q[:, 1]))])


def read_invisible_surfaces(m, res, log):
    """every collision surface flagged invisible (player clip): structure BSP + placed instances (not render-only)"""
    gmat = global_material_names(m)
    polys_out = []
    for si in scenario_bsps(m):
        d = m.meta(si, 0x3A0)
        mats = []
        for e in m.block(d, 0x28, 0x18):
            ti = m.tagref(e, 0); gm, = struct.unpack_from('<h', e, 0x10)
            mats.append((m.names[ti].split('\\')[-1] if ti >= 0 else None,
                         gmat[gm] if 0 <= gm < len(gmat) else None))
        datum, = struct.unpack_from('<I', d, 0x38C)
        if datum == 0xFFFFFFFF or res.type_name(datum) != 'structure_bsp_tag_resources':
            log('%s: no structure_bsp_tag_resources' % m.names[si]); continue
        defn, base, bufs = res.load(datum)
        if 2 not in bufs:
            log('%s: resource has no data page' % m.names[si]); continue

        def emit(verts, polys, flags, mt, source, xf=None):
            n = 0
            for s in np.nonzero(flags & SF_INVISIBLE)[0].tolist():
                ring = polys[s]
                if len(ring) < 3:
                    continue
                p = verts[ring]
                if xf is not None:
                    p = xf(p)
                k = int(mt[s])
                shader, gname = mats[k] if 0 <= k < len(mats) else (None, None)
                polys_out.append({'source': source, 'flags': int(flags[s]), 'material': gname or shader,
                                  'shader': shader, '_p': p})
                n += 1
            return n

        ns = 0
        for k, large in ((0x00, False), (0x0C, True)):
            cnt, cb, co = rblock(bufs, defn, base + k)
            for i in range(cnt):
                ns += emit(*collision_polys(bufs, cb, co + 0x60 * i, large), 'structure')
        cnt, ib, io = rblock(bufs, defn, base + 0x18)
        defs = [collision_polys(bufs, ib, io + 0xC4 * i + 0x14) for i in range(cnt)]
        ni = 0
        for e in m.block(d, 0x1C0, 0x78):
            scale, = struct.unpack_from('<f', e, 0)
            fwd, left, up, pos = (np.array(struct.unpack_from('<3f', e, o)) for o in (0x04, 0x10, 0x1C, 0x28))
            di, iflags = struct.unpack_from('<hH', e, 0x34)
            if di < 0 or di >= len(defs) or (iflags & 2) or not (defs[di][2] & SF_INVISIBLE).any():
                continue
            b = np.stack([fwd, left, up]) * scale
            nm = sid_name(m, struct.unpack_from('<I', e, 0x54)[0])
            ni += emit(*defs[di], 'instance:%s' % nm,
                       xf=lambda v, b=b, p=pos: v[:, :1] * b[0] + v[:, 1:2] * b[1] + v[:, 2:] * b[2] + p)
        log('%s: invisible surfaces: %d structure, %d instance' % (m.names[si], ns, ni))
    for e in polys_out:
        n = newell(e['_p'])
        ln = float(np.linalg.norm(n))
        e['vertical'] = bool(ln < 1e-9 or abs(n[2]) < VERTICAL_NZ * ln)
    return polys_out


# ----------------------------------------------------------------------------------------------- drawing
class Grid:
    def __init__(self, g):
        self.x0, self.y1, self.ppu = float(g['x0']), float(g['y1']), float(g['px_per_unit'])
        self.w, self.h = int(g['w']), int(g['h'])
        self.y0 = self.y1 - self.h / self.ppu
        self.x1 = self.x0 + self.w / self.ppu

    def px(self, pts):
        pts = np.asarray(pts, np.float64).reshape(-1, np.shape(pts)[-1])
        return [((x - self.x0) * self.ppu, (self.y1 - y) * self.ppu) for x, y in pts[:, :2].tolist()]

    def inside(self, aabb):
        """'in' / 'partly' / 'encloses' / 'out': where an aabb {'x': [lo, hi], 'y': [lo, hi]} is relative to the image
        ('encloses' = it covers the whole image, so none of its edges is visible)"""
        (xa, xb), (ya, yb) = aabb['x'], aabb['y']
        if xb < self.x0 or xa > self.x1 or yb < self.y0 or ya > self.y1:
            return 'out'
        if xa >= self.x0 and xb <= self.x1 and ya >= self.y0 and yb <= self.y1:
            return 'in'
        if xa <= self.x0 and xb >= self.x1 and ya <= self.y0 and yb >= self.y1:
            return 'encloses'
        return 'partly'


def layer(size):
    im = Image.new('RGBA', size, (0, 0, 0, 0))
    return im, ImageDraw.Draw(im)


def ring_line(dr, pts, fill, width):
    if len(pts) >= 2:
        dr.line(list(pts) + [pts[0]], fill=fill, width=width, joint='curve')


def dashed(dr, pts, fill, width=2, dash=12.0, gap=7.0):
    pts = list(pts) + [pts[0]]
    phase = 0.0                    # distance into the current dash+gap period
    for (ax, ay), (bx, by) in zip(pts, pts[1:]):
        seg = math.hypot(bx - ax, by - ay)
        t = 0.0
        while t < seg:
            inper = phase % (dash + gap)
            if inper < dash:
                step = min(dash - inper, seg - t)
                f0, f1 = t / seg, (t + step) / seg
                dr.line([(ax + (bx - ax) * f0, ay + (by - ay) * f0), (ax + (bx - ax) * f1, ay + (by - ay) * f1)],
                        fill=fill, width=width)
            else:
                step = min(dash + gap - inper, seg - t)
            t += step; phase += step


def font(sz):
    try:
        return ImageFont.load_default(size=sz)
    except TypeError:                 # Pillow < 10.1: fixed bitmap font
        return ImageFont.load_default()


def label(dr, xy, text, fnt, fill, W, H, placed):
    """text on a dark box at xy (kept inside the image, moved down past labels already in `placed`)"""
    x, y = xy
    l, t, r, b = dr.textbbox((0, 0), text, font=fnt)
    x = min(max(x, 2), W - (r - l) - 4); y = min(max(y, 2), H - (b - t) - 6)
    for _ in range(50):
        box = (x - 2, y + t - 2, x + r - l + 2, y + b + 2)
        if not any(box[0] < q[2] and q[0] < box[2] and box[1] < q[3] and q[1] < box[3] for q in placed):
            break
        y += b - t + 6
    placed.append(box)
    dr.rectangle(box, fill=(0, 0, 0, 170))
    dr.text((x, y), text, font=fnt, fill=fill)


def draw_overlay(grid, tvs, softs, invis, wbounds):
    W, H = grid.w, grid.h
    size = (W, H)
    out = Image.new('RGBA', size, (0, 0, 0, 0))
    fsz = max(11, min(22, int(round(max(W, H) / 110))))
    fnt = font(fsz)

    # soft ceilings: one fill layer + one edge layer per type (fills inside a layer overwrite, so the tint is uniform)
    for tn, alpha, ealpha in (('soft_kill', 45, 90), ('acceleration', 28, 110), ('slip_surface', 45, 120)):
        sel = [s for s in softs if s['type_name'] == tn and s['triangles']]
        if not sel:
            continue
        col = C_SOFT[tn]
        fl, fd = layer(size); el, ed = layer(size)
        for s in sel:
            for t in s['_tris']:
                p = grid.px(t)
                fd.polygon(p, fill=col + (alpha,))
                ring_line(ed, p, col + (ealpha,), 1)
        out = Image.alpha_composite(out, fl); out = Image.alpha_composite(out, el)

    # invisible collision: sloped / flat = translucent fill + thin edge, near-vertical walls = solid 2 px outline
    fl, fd = layer(size); el, ed = layer(size)
    for e in invis:
        p = grid.px(e['_p'])
        if e['vertical']:
            ring_line(ed, p, C_INVIS + (255,), 2)
        else:
            fd.polygon(p, fill=C_INVIS + (90,))
            ring_line(ed, p, C_INVIS + (200,), 1)
    out = Image.alpha_composite(out, fl); out = Image.alpha_composite(out, el)

    # trigger volumes: other (thin grey) < safe (green dashed) < kill (red 3 px + hatch)
    ol, od = layer(size)
    for tv in tvs:
        if tv['kind'] == 'other':
            ring_line(od, grid.px(tv['footprint']), C_OTHER + (200,), 1)
    out = Image.alpha_composite(out, ol)
    sl, sd = layer(size)
    for tv in tvs:
        if tv['kind'] == 'safe':
            dashed(sd, grid.px(tv['footprint']), C_SAFE + (255,), 2)
    out = Image.alpha_composite(out, sl)
    kills = [tv for tv in tvs if tv['kind'] == 'kill']
    if kills:
        # hatch every kill footprint; the light red fill only goes on volumes that cover less than half the image
        # (the usual "fell off the map" box under the whole level would otherwise tint everything)
        hmask = Image.new('L', size, 0); fmask = Image.new('L', size, 0)
        for tv in kills:
            one = Image.new('L', size, 0)
            ImageDraw.Draw(one).polygon(grid.px(tv['footprint']), fill=255)
            hmask.paste(255, mask=one)
            if np.count_nonzero(np.asarray(one)) < 0.5 * W * H:
                fmask.paste(255, mask=one)
        fill, fd = layer(size)
        fd.rectangle([0, 0, W, H], fill=C_KILL + (40,))
        hatch, hd = layer(size)
        for k in range(-H, W, 14):
            hd.line([(k, H), (k + H, 0)], fill=C_KILL + (85,), width=1)
        empty = Image.new('RGBA', size, (0, 0, 0, 0))
        out = Image.alpha_composite(out, Image.composite(fill, empty, fmask))
        out = Image.alpha_composite(out, Image.composite(hatch, empty, hmask))
        kl, kd = layer(size)
        for tv in kills:
            ring_line(kd, grid.px(tv['footprint']), C_KILL + (255,), 3)
        out = Image.alpha_composite(out, kl)

    # structure BSP world bounds
    bl, bd = layer(size)
    bounds_note = []
    for b in wbounds:
        (xa, ya), (xb, yb) = grid.px([[b['x'][0], b['y'][1]], [b['x'][1], b['y'][0]]])
        bd.rectangle([xa, ya, xb, yb], outline=C_BOUNDS + (230,), width=2)
        bounds_note.append(grid.inside(b))
    out = Image.alpha_composite(out, bl)

    # labels (kill / safe: name + z range; others: name)
    tl, td = layer(size)
    placed = []
    for tv in sorted(tvs, key=lambda t: {'other': 2, 'safe': 1, 'kill': 0}[t['kind']]):
        p = grid.px(tv['footprint'])
        xs = [q[0] for q in p]; ys = [q[1] for q in p]
        if max(xs) < 0 or min(xs) > W or max(ys) < 0 or min(ys) > H:
            continue
        x, y = max(min(xs), 0) + 4, max(min(ys), 0) + 4
        if tv['kind'] == 'other':
            label(td, (x, y), tv['name'], font(max(10, fsz - 3)), C_OTHER + (255,), W, H, placed)
        else:
            col = C_KILL if tv['kind'] == 'kill' else C_SAFE
            label(td, (x, y), '%s  z %.1f..%.1f' % (tv['name'], tv['z_range'][0], tv['z_range'][1]), fnt,
                  col + (255,), W, H, placed)
    out = Image.alpha_composite(out, tl)

    # legend (bottom-left): only what the map has, with counts
    nsoft = {tn: sum(1 for s in softs if s['type_name'] == tn and s['triangles']) for tn in C_SOFT}
    nkind = {k: sum(1 for t in tvs if t['kind'] == k) for k in ('kill', 'safe', 'other')}
    nwall = sum(1 for e in invis if e['vertical'])
    items = [('fill', C_SOFT[tn], 'soft ceiling: %s (%d)' % (tn.replace('_', ' '), nsoft[tn])) for tn in
             ('soft_kill', 'acceleration', 'slip_surface') if nsoft[tn]]
    items += [it for it, n in ((('fill', C_INVIS, 'invisible collision, sloped/flat (%d)' % (len(invis) - nwall)),
                                len(invis) - nwall),
                               (('line', C_INVIS, 'invisible collision, wall (%d)' % nwall), nwall),
                               (('kill', C_KILL, 'kill trigger volume (%d)' % nkind['kill']), nkind['kill']),
                               (('dash', C_SAFE, 'safe zone (%d)' % nkind['safe']), nkind['safe']),
                               (('line1', C_OTHER, 'other trigger volume (%d)' % nkind['other']), nkind['other'])) if n]
    items.append(('rect', C_BOUNDS, 'BSP world bounds' + ('' if 'in' in bounds_note or 'partly' in bounds_note else
                                                          ' (encloses the image)' if 'encloses' in bounds_note else
                                                          ' (outside the image)')))
    lg, ld = layer(size)
    lh = fsz + 6; sw = 2 * fsz
    tw = max(ld.textbbox((0, 0), t, font=fnt)[2] for _, _, t in items)
    bw, bh = sw + tw + 26, lh * len(items) + 12
    x0, y0 = 8, H - bh - 8
    ld.rectangle([x0, y0, x0 + bw, y0 + bh], fill=(0, 0, 0, 225), outline=(255, 255, 255, 120))
    for i, (kind, col, text) in enumerate(items):
        y = y0 + 8 + i * lh; sx0, sx1, sy0, sy1 = x0 + 8, x0 + 8 + sw, y + 2, y + lh - 4
        if kind == 'fill':
            ld.rectangle([sx0, sy0, sx1, sy1], fill=col + (190,), outline=col + (255,))
        elif kind == 'line':
            ld.line([(sx0, (sy0 + sy1) / 2), (sx1, (sy0 + sy1) / 2)], fill=col + (255,), width=2)
        elif kind == 'line1':
            ld.rectangle([sx0, sy0, sx1, sy1], outline=col + (255,), width=1)
        elif kind == 'kill':
            ld.rectangle([sx0, sy0, sx1, sy1], fill=col + (90,), outline=col + (255,), width=3)
        elif kind == 'dash':
            dashed(ld, [(sx0, sy0), (sx1, sy0), (sx1, sy1), (sx0, sy1)], col + (255,), 2, 6, 4)
        elif kind == 'rect':
            ld.rectangle([sx0, sy0, sx1, sy1], outline=col + (255,), width=2)
        ld.text((sx1 + 10, y), text, font=fnt, fill=(255, 255, 255, 255))
    return Image.alpha_composite(out, lg)


# ----------------------------------------------------------------------------------------------- main
def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('map'); ap.add_argument('out')
    ap.add_argument('--grid', help="h3bsp.py output JSON whose pixel grid to use (default: run h3bsp.py first)")
    ap.add_argument('--composite', help='also write the base picture with the overlay on top to this PNG')
    ap.add_argument('--no-triangles', action='store_true', help='leave soft-ceiling triangle lists out of the JSON')
    ap.add_argument('-q', '--quiet', action='store_true')
    opts = ap.parse_args()
    log = (lambda *a: None) if opts.quiet else (lambda *a: print(*a, file=sys.stderr))
    t0 = time.time()
    stem = os.path.splitext(opts.out)[0]

    if opts.grid:
        gpath = opts.grid
    else:
        gpath = stem + '_base.json'
        cmd = [sys.executable, os.path.join(HERE, 'h3bsp.py'), opts.map, stem + '_base.png'] + (['-q'] if opts.quiet else [])
        log('running ' + ' '.join(cmd[1:]))
        subprocess.run(cmd, check=True)
    with open(gpath) as f:
        gj = json.load(f)
    grid = Grid(gj)
    base_png = os.path.splitext(gpath)[0] + '.png'

    m = H3Map(opts.map)
    res = Resources(m)
    sc = m.find('scnr')
    if not sc:
        raise SystemExit('no scnr tag')
    d = m.meta(sc[0], 0x800)
    tvs, kill_idx, safe_idx = read_trigger_volumes(m, d)
    softs = read_soft_ceilings(m, d, keep_tris=not opts.no_triangles)
    wb = read_world_bounds(m)
    invis = read_invisible_surfaces(m, res, log)

    img = draw_overlay(grid, tvs, softs, invis, wb)
    img.save(opts.out)

    if opts.composite:
        if not os.path.exists(base_png):
            raise SystemExit('--composite: base picture %s not found' % base_png)
        base = Image.open(base_png).convert('RGBA')
        if base.size != img.size:
            log('warning: base picture is %dx%d, grid says %dx%d' % (base.size + img.size))
            base = base.resize(img.size)
        comp = Image.new('RGBA', img.size, (22, 22, 26, 255))
        comp = Image.alpha_composite(Image.alpha_composite(comp, base), img)
        comp.convert('RGB').save(opts.composite)

    summarize = lambda tv: {k: tv[k] for k in ('index', 'name', 'aabb', 'center', 'extents', 'reason', 'in_image')}
    for e in tvs + wb:
        e['in_image'] = grid.inside(e if 'x' in e else e['aabb'])
    nstruct = sum(1 for e in invis if e['source'] == 'structure')
    by_flags = {}
    for e in invis:
        by_flags[str(e['flags'])] = by_flags.get(str(e['flags']), 0) + 1
    meta = {
        'map': os.path.splitext(os.path.basename(opts.map))[0],
        'scenario': m.names[sc[0]],
        'grid': {k: gj[k] for k in ('x0', 'y0', 'x1', 'y1', 'px_per_unit', 'w', 'h')},
        'pixel': 'px = (x - x0) * px_per_unit, py = (y1 - y) * px_per_unit; top-left origin; +x right, +y up',
        'engine_limit': ENGINE_LIMIT,
        'world_bounds': wb,
        'scenario_kill_triggers': kill_idx, 'scenario_safe_zone_triggers': safe_idx,
        'kill_volumes': [summarize(tv) for tv in tvs if tv['kind'] == 'kill'],
        'safe_zones': [summarize(tv) for tv in tvs if tv['kind'] == 'safe'],
        'trigger_volumes': tvs,
        'soft_ceilings': [{k: v for k, v in s.items() if k != '_tris'} for s in softs],
        'invisible_surfaces': {
            'count': len(invis), 'structure': nstruct, 'instances': len(invis) - nstruct,
            'vertical': sum(1 for e in invis if e['vertical']), 'by_flags': by_flags,
            'flag_bits': {'1': 'two-sided', '2': 'invisible', '4': 'climbable', '8': 'breakable', '64': 'slip'},
            'polygons': [{'source': e['source'], 'flags': e['flags'], 'material': e['material'], 'shader': e['shader'],
                          'vertical': e['vertical'], 'points': [vec(p, 2) for p in e['_p']]} for e in invis]},
    }
    jpath = stem + '.json'
    with open(jpath, 'w') as f:
        json.dump(meta, f, indent=1, allow_nan=False)

    # ---- human summary
    log('%s (%s): grid x %.1f..%.1f y %.1f..%.1f, %dx%d' % (meta['map'], meta['scenario'], grid.x0, grid.x1, grid.y0,
                                                           grid.y1, grid.w, grid.h))
    for b in wb:
        log('  world bounds %s: x %s y %s z %s (%s)' % (b['bsp'].split('\\')[-1], b['x'], b['y'], b['z'],
                                                        WHERE[b['in_image']]))
    log('  trigger volumes %d: %d kill, %d safe, %d other' % (
        len(tvs), *(sum(1 for t in tvs if t['kind'] == k) for k in ('kill', 'safe', 'other'))))
    for tv in tvs:
        if tv['kind'] != 'other':
            a = tv['aabb']
            log('    %-4s #%-2d %-22s x %8.1f..%-8.1f y %8.1f..%-8.1f z %7.1f..%-7.1f (%s)%s' % (
                tv['kind'], tv['index'], tv['name'], *a['x'], *a['y'], *a['z'], WHERE[tv['in_image']],
                '' if abs(tv['up'][2]) > 0.999 else '  tilted up %s' % tv['up']))
    log('  soft ceilings %d' % len(softs))
    for s in softs:
        log('    %-13s %-16s %5d tris  ignores %-24s bbox %s' % (s['type_name'], s['name'], s['triangles'],
                                                                  ','.join(s['ignores']) or '-', s['bbox']))
    log('  invisible surfaces %d (%d structure, %d instance; %d near-vertical) flags %s' % (
        len(invis), nstruct, len(invis) - nstruct, meta['invisible_surfaces']['vertical'], by_flags))
    log('wrote %s + %s%s (%.1fs)' % (opts.out, jpath, (' + ' + opts.composite) if opts.composite else '',
                                     time.time() - t0))


if __name__ == '__main__':
    main()
