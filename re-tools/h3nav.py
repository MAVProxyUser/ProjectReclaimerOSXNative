#!/usr/bin/env python3
"""Walkable-floor graph of a Halo 3 (MCC) multiplayer map, for scripted test walking and the sparring partner
(backlog 32: walk where the map lets you instead of into walls).

    python3 re-tools/h3nav.py <file.map> <out.nav> [--cell 0.25] [--png preview.png]

Built from the same collision geometry as h3bsp.py (structure BSPs + instanced geometry, invisible player-clip
surfaces included), as a layered height field: the map's play area is cut into square cells; every upward-facing,
not-slippery collision surface over a cell gives that cell a floor level (several where floors overlap: bridges,
rooms under rooms). The scenario's placed scenery and crates (barricades, crates, their own collision models) count as
collision too (--no-objects leaves them out). A floor needs CLEAR_H of free space above it (no collision there). Neighbouring floors (8
directions) connect when their heights differ by at most STEP (both ways) or the step is a drop of up to DROP
(one way down); a diagonal needs both of its straight neighbours. Floors inside a kill trigger volume, or outside
every safe zone when the map has some, are left out.

out.nav (little-endian), read by shim/src/nav.c:
    "RNV1", u32 version 1, f32 x0, y0 (corner of cell (0,0)), f32 cell, i32 w, h, i32 node count,
    f32 STEP, f32 CLEAR_H, 24 bytes reserved                                   (64-byte header)
    u32 first[w*h + 1]: the nodes of cell (ix, iy) = first[iy*w+ix] .. first[iy*w+ix+1]-1 (sorted by z)
    nodes, 40 bytes: f32 z (the floor), u16 component, u8 wall distance (cells to the nearest node with a
    missing neighbour, capped at 15), u8 flags (1 = reached only by dropping), i32 neighbour[8] (-1 = none;
    directions E, NE, N, NW, W, SW, S, SE = (+1,0) (+1,+1) (0,+1) (-1,+1) (-1,0) (-1,-1) (0,-1) (+1,-1))
World units as object positions; a biped's feet stand at the node's z.
Read-only, static file work.
"""
import argparse, collections, math, os, struct, sys, time

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from h3map import H3Map  # noqa: E402
from h3bsp import Resources, level_triangles, fan  # noqa: E402
from h3bounds import read_trigger_volumes  # noqa: E402

SF_SLIP = 0x40
WALK_NZ = 0.7          # floor: surface normal z at least this (slopes up to ~45 degrees)
STEP = 0.35            # height difference walked both ways between neighbouring cells
DROP = 2.0             # drops walked down (one way)
CLEAR_H = 0.6          # free space a floor needs above it (a crouching Spartan is ~0.5 units)
CLEAR_EPS = 0.08       # collision closer than this above the floor counts as the floor itself
LEVEL_GAP = 0.3        # floor samples in one cell further apart than this are separate levels
DIRS = [(1, 0), (1, 1), (0, 1), (-1, 1), (-1, 0), (-1, -1), (0, -1), (1, -1)]


# placed objects: scenery (scnr +0xB4 [0xB4], palette +0xC0) and crates (+0x5BC [0xB0], palette +0x5C8) -- their own
# collision models, not part of the level geometry (Rat's Nest barricades, crates). Placement: +4 flags (bit 3 never
# placed), +8 position, +0x14 yaw / pitch / roll (radians), +0x20 scale. Object definition +0x34 model -> hlmt +0x10 coll,
# hlmt +0xAC nodes [0x5C] (+0x28 default inverse scale, +0x2C/+0x38/+0x44 inverse forward/left/up, +0x50 inverse
# position); coll +0x20 regions [0x10] -> +4 permutations [0x28] (the first) -> +4 bsps [0x64] (+0 node, +0x40 surfaces
# [0xC], +0x4C edges [0xC], +0x58 vertices [0x10]).
OBJ_BLOCKS = ((0xB4, 0xB4, 0xC0), (0x5BC, 0xB0, 0x5C8))


def coll_polys_tag(m, b):
    surfs, edges, verts = m.block(b, 0x40, 0xC), m.block(b, 0x4C, 0xC), m.block(b, 0x58, 0x10)
    if not surfs or not verts:
        return None
    V = np.array([struct.unpack_from('<3f', v, 0) for v in verts], np.float64)
    E = [struct.unpack_from('<6h', e, 0) for e in edges]
    polys, fl = [], []
    for si, sf in enumerate(surfs):
        fe, = struct.unpack_from('<H', sf, 2); ring, ei = [], fe
        for _ in range(256):
            if not 0 <= ei < len(E):
                ring = []; break
            st, en, fw, rv, left, _r = E[ei]
            if left == si:
                ring.append(st); ei = fw
            else:
                ring.append(en); ei = rv
            if ei == fe:
                break
        polys.append(ring); fl.append(sf[0xA])
    return V, polys, np.array(fl, np.uint8)


def object_model_tris(m, ti, cache):
    """an object definition's collision triangles in model space (+ surface flags), first permutation of each region"""
    if ti in cache:
        return cache[ti]
    out = None
    try:
        hl = m.tagref(m.meta(ti, 0x48), 0x34)
        if hl >= 0 and m.tags[hl][0] == 'hlmt' and m.tags[hl][2]:
            h = m.meta(hl, 0xB8); co = m.tagref(h, 0x10)
            nodes = m.block(h, 0xAC, 0x5C)
            if co >= 0 and m.tags[co][0] == 'coll' and m.tags[co][2]:
                tl, fl = [], []
                for r in m.block(m.meta(co, 0x30), 0x20, 0x10):
                    perms = m.block(r, 0x04, 0x28)
                    if not perms:
                        continue
                    for b in m.block(perms[0], 0x04, 0x64):
                        cp = coll_polys_tag(m, b)
                        if cp is None:
                            continue
                        V, polys, sfl = cp
                        node, = struct.unpack_from('<h', b, 0)
                        if 0 <= node < len(nodes):          # node space -> model space (the node's default pose)
                            e = nodes[node]; sc, = struct.unpack_from('<f', e, 0x28)
                            B = np.array([struct.unpack_from('<3f', e, o) for o in (0x2C, 0x38, 0x44)])   # rows f, l, u
                            pos = np.array(struct.unpack_from('<3f', e, 0x50))
                            # inverse pose: p_node = s * (x*f + y*l + z*u) + pos  ->  p_model[k] = (row k) . (p_node - pos) / s
                            V = np.stack([((V - pos) * B[k]).sum(1) for k in range(3)], 1) / (sc if sc else 1.0)
                        t, owner = fan(polys)
                        if len(t):
                            tl.append(V[t]); fl.append(sfl[owner])
                if tl:
                    out = (np.concatenate(tl), np.concatenate(fl))
    except Exception:
        out = None
    cache[ti] = out
    return out


def placed_object_tris(m, d, log):
    """world-space triangles + flags of the scenario's placed scenery and crates"""
    cache, tl, fl, n = {}, [], [], 0
    for blk, esz, pal in OBJ_BLOCKS:
        palette = [m.tagref(e, 0) for e in m.block(d, pal, 0x10)]
        for e in m.block(d, blk, esz):
            pi, = struct.unpack_from('<h', e, 0); pflags, = struct.unpack_from('<I', e, 4)
            if pflags & 8 or not 0 <= pi < len(palette) or palette[pi] < 0:
                continue
            mt = object_model_tris(m, palette[pi], cache)
            if mt is None:
                continue
            pos = np.array(struct.unpack_from('<3f', e, 8)); yaw, pitch, roll = struct.unpack_from('<3f', e, 0x14)
            scale, = struct.unpack_from('<f', e, 0x20); scale = scale if scale > 0 else 1.0
            cy, sy, cp, sp, cr, sr = math.cos(yaw), math.sin(yaw), math.cos(pitch), math.sin(pitch), math.cos(roll), math.sin(roll)
            Rz = np.array([[cy, -sy, 0], [sy, cy, 0], [0, 0, 1]]); Ry = np.array([[cp, 0, -sp], [0, 1, 0], [sp, 0, cp]])
            Rx = np.array([[1, 0, 0], [0, cr, -sr], [0, sr, cr]]); R = Rz @ Ry @ Rx
            t = mt[0]
            w = np.stack([(t * R[k]).sum(-1) for k in range(3)], -1) * scale + pos
            tl.append(w); fl.append(mt[1]); n += 1
    log('%d placed objects with collision (%d definitions), %d triangles' % (
        n, sum(1 for v in cache.values() if v is not None), sum(len(x) for x in tl)))
    if not tl:
        return np.zeros((0, 3, 3)), np.zeros(0, np.uint8)
    return np.concatenate(tl), np.concatenate(fl)


def samples(tris, spacing):
    """points covering each triangle at most `spacing` apart -> (points Nx3, owning triangle N)"""
    e = np.maximum(np.maximum(np.linalg.norm(tris[:, 1] - tris[:, 0], axis=1), np.linalg.norm(tris[:, 2] - tris[:, 1], axis=1)),
                   np.linalg.norm(tris[:, 0] - tris[:, 2], axis=1))
    n = np.maximum(1, np.ceil(e / spacing)).astype(np.int64)
    pts, own = [], []
    for k in np.unique(n):
        idx = np.nonzero(n == k)[0]
        i, j = np.meshgrid(np.arange(k + 1), np.arange(k + 1), indexing='ij')
        keep = (i + j) <= k
        wb, wc = (i[keep] / k)[None, :, None], (j[keep] / k)[None, :, None]
        for c in range(0, len(idx), max(1, 2000000 // keep.sum())):        # bounded chunks for big triangles
            ii = idx[c:c + max(1, 2000000 // keep.sum())]
            a, b, cc = tris[ii, 0][:, None], tris[ii, 1][:, None], tris[ii, 2][:, None]
            p = a + (b - a) * wb + (cc - a) * wc
            pts.append(p.reshape(-1, 3)); own.append(np.repeat(ii, keep.sum()))
    return np.concatenate(pts), np.concatenate(own)


def in_box(p, tv):
    """points (N,3) inside a trigger volume (h3bounds entry: corner + extents along forward / left / up)"""
    pos, ext = np.array(tv['position']), np.array(tv['extents'])
    q = p - pos
    out = np.ones(len(p), bool)
    for axis, e in (('forward', ext[0]), ('left', ext[1]), ('up', ext[2])):
        v = np.array(tv[axis])
        t = q[:, 0] * v[0] + q[:, 1] * v[1] + q[:, 2] * v[2]      # (Accelerate's matmul raises spurious FP warnings)
        out &= (t >= 0) & (t <= e)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('map'); ap.add_argument('out')
    ap.add_argument('--cell', type=float, default=0.25)
    ap.add_argument('--png', help='preview picture (components / wall distance)')
    ap.add_argument('--quiet', action='store_true')
    ap.add_argument('--no-objects', action='store_true', help='level geometry only (no placed scenery / crates)')
    o = ap.parse_args()
    log = (lambda *a: None) if o.quiet else (lambda *a: print(*a, file=sys.stderr))
    t0 = time.time()
    m = H3Map(o.map); res = Resources(m)

    class Opts:
        invisible = True; unmaterialed = True; no_instances = False; render_only = False
    tris, _kinds, two, flags = level_triangles(m, res, Opts, log=log, with_flags=True)
    sc = m.find('scnr')[0]; d = m.meta(sc, 0x4A0)
    tvs, _k, _s = read_trigger_volumes(m, d)
    if not o.no_objects:
        ot, of = placed_object_tris(m, m.meta(sc, 0x5D4), log)
        if len(ot):
            tris = np.concatenate([tris, ot]); two = np.concatenate([two, np.zeros(len(ot), bool)]); flags = np.concatenate([flags, of])
    kills = [t for t in tvs if t['kind'] == 'kill']; safes = [t for t in tvs if t['kind'] == 'safe']
    # play area: the safe zones' extent, else where most of the geometry is
    if safes:
        lo = np.min([[t['aabb']['x'][0], t['aabb']['y'][0]] for t in safes], 0)
        hi = np.max([[t['aabb']['x'][1], t['aabb']['y'][1]] for t in safes], 0)
    else:
        c = tris.reshape(-1, 3)
        lo, hi = np.percentile(c[:, :2], 1, 0), np.percentile(c[:, :2], 99, 0)
    lo, hi = lo - 1.0, hi + 1.0
    cell = o.cell
    w, h = int(np.ceil((hi[0] - lo[0]) / cell)), int(np.ceil((hi[1] - lo[1]) / cell))
    x0, y0 = float(lo[0]), float(lo[1])
    log('area x %.1f..%.1f y %.1f..%.1f: %d x %d cells of %.2f; %d kill volumes, %d safe zones' % (
        lo[0], hi[0], lo[1], hi[1], w, h, cell, len(kills), len(safes)))

    # triangles over the area only
    tlo, thi = tris[:, :, :2].min(1), tris[:, :, :2].max(1)
    keep = (thi[:, 0] >= lo[0]) & (tlo[:, 0] <= hi[0]) & (thi[:, 1] >= lo[1]) & (tlo[:, 1] <= hi[1])
    tris, two, flags = tris[keep], two[keep], flags[keep]
    nrm = np.cross(tris[:, 1] - tris[:, 0], tris[:, 2] - tris[:, 0])
    ln = np.linalg.norm(nrm, axis=1); ok = ln > 1e-9
    tris, two, flags, nrm, ln = tris[ok], two[ok], flags[ok], nrm[ok], ln[ok]
    nz = nrm[:, 2] / ln
    walk = ((nz >= WALK_NZ) | (two & (nz <= -WALK_NZ))) & ((flags & SF_SLIP) == 0)
    log('%d triangles over the area, %d walkable' % (len(tris), walk.sum()))

    pts, own = samples(tris, cell * 0.5)
    ix = np.floor((pts[:, 0] - x0) / cell).astype(np.int64); iy = np.floor((pts[:, 1] - y0) / cell).astype(np.int64)
    inside = (ix >= 0) & (ix < w) & (iy >= 0) & (iy < h)
    pts, own, ix, iy = pts[inside], own[inside], ix[inside], iy[inside]
    cid = iy * w + ix
    log('%d surface samples' % len(pts))
    # solid samples sorted by (cell, z) for the clearance test
    BIG = 1e4
    skey = np.sort(cid * BIG + (pts[:, 2] + 5000.0))
    # floor levels per cell: walkable samples clustered by height (level z = the cluster's top)
    f = walk[own]
    fc, fz = cid[f], pts[f, 2]
    order = np.lexsort((fz, fc)); fc, fz = fc[order], fz[order]
    new = np.ones(len(fc), bool); new[1:] = (fc[1:] != fc[:-1]) | (fz[1:] - fz[:-1] > LEVEL_GAP)
    grp = np.cumsum(new) - 1
    lvl_c = fc[new]; lvl_z = np.full(grp[-1] + 1, -np.inf); np.maximum.at(lvl_z, grp, fz)
    # clearance: no collision in (z + CLEAR_EPS, z + CLEAR_H)
    a = np.searchsorted(skey, lvl_c * BIG + (lvl_z + CLEAR_EPS + 5000.0), 'right')
    b = np.searchsorted(skey, lvl_c * BIG + (lvl_z + CLEAR_H + 5000.0), 'left')
    clear = b <= a
    # deadly places
    cx = x0 + (lvl_c % w + 0.5) * cell; cy = y0 + (lvl_c // w + 0.5) * cell
    P = np.stack([cx, cy, lvl_z + 0.3], 1)
    dead = np.zeros(len(P), bool)
    for t in kills:
        dead |= in_box(P, t)
    if safes:
        ins = np.zeros(len(P), bool)
        for t in safes:
            ins |= in_box(P, t)
        dead |= ~ins
    good = clear & ~dead
    log('%d floor levels: %d clear of collision above, %d in kill / outside safe zones -> %d nodes' % (
        len(lvl_c), clear.sum(), (clear & dead).sum(), good.sum()))
    nc, nzs = lvl_c[good], lvl_z[good]                  # sorted by (cell, z)
    N = len(nc)
    first = np.searchsorted(nc, np.arange(w * h + 1)).astype(np.uint32)
    cnt = np.diff(first.astype(np.int64))
    maxl = int(cnt.max()) if N else 0
    nx, ny = nc % w, nc // w

    def best_in(cells_x, cells_y, z, up, down, valid):
        """per node: the level in cell (cells_x, cells_y) closest to z within [z - down, z + up] (-1 if none)"""
        out = np.full(len(z), -1, np.int64); bd = np.full(len(z), np.inf)
        cc = np.where(valid, cells_y * w + cells_x, 0)
        f0, c0 = first[cc].astype(np.int64), np.where(valid, cnt[cc], 0)
        for k in range(maxl):
            has = valid & (k < c0)
            j = np.where(has, f0 + k, 0)
            dz = nzs[j] - z
            okk = has & (dz <= up) & (dz >= -down) & (np.abs(dz) < bd)
            out[okk] = j[okk]; bd[okk] = np.abs(dz[okk])
        return out

    nbr = np.full((N, 8), -1, np.int64); dropf = np.zeros(N, np.uint8)
    straight = {}
    for di in (0, 2, 4, 6, 1, 3, 5, 7):                 # straight directions first: diagonals check them
        dx, dy = DIRS[di]
        tx, ty = nx + dx, ny + dy
        valid = (tx >= 0) & (tx < w) & (ty >= 0) & (ty < h)
        walkable = best_in(tx, ty, nzs, STEP, STEP, valid)
        if dx and dy:   # diagonal: both straight neighbours must be walkable from here
            walkable[(straight[(dx, 0)] < 0) | (straight[(0, dy)] < 0)] = -1
        else:
            straight[(dx, dy)] = walkable
        drop = best_in(tx, ty, nzs - STEP, 0.0, DROP - STEP, valid)
        if dx and dy:
            drop[(straight[(dx, 0)] < 0) & (straight[(0, dy)] < 0)] = -1
        use_drop = (walkable < 0) & (drop >= 0)
        nbr[:, di] = np.where(walkable >= 0, walkable, drop)
        dropf[drop[use_drop]] |= 1
    # wall distance: BFS (over the undirected graph) from nodes missing any neighbour
    deg = (nbr >= 0).sum(1)
    wd = np.full(N, 15, np.int64); q = collections.deque()
    for i in np.nonzero(deg < 8)[0].tolist():
        wd[i] = 0; q.append(i)
    nl = nbr.tolist()
    while q:
        i = q.popleft()
        if wd[i] >= 15:
            continue
        for j in nl[i]:
            if j >= 0 and wd[j] > wd[i] + 1:
                wd[j] = wd[i] + 1; q.append(j)
    # components (two-way edges only), biggest first
    und = [[] for _ in range(N)]
    for i, row in enumerate(nl):
        for j in row:
            if j >= 0 and i in nl[j]:
                und[i].append(j)
    comp = np.full(N, -1, np.int64); sizes = []
    for s in range(N):
        if comp[s] >= 0:
            continue
        c = len(sizes); comp[s] = c; st = [s]; n = 0
        while st:
            i = st.pop(); n += 1
            for j in und[i]:
                if comp[j] < 0:
                    comp[j] = c; st.append(j)
        sizes.append(n)
    rank = np.argsort(-np.array(sizes)); remap = np.empty(len(sizes), np.int64); remap[rank] = np.arange(len(sizes))
    comp = remap[comp]
    sz = sorted(sizes, reverse=True)
    log('%d nodes, %d edges (%d drop-only nodes); %d components, biggest %s' % (
        N, int((nbr >= 0).sum()), int(dropf.sum()), len(sizes), sz[:6]))

    tmp = o.out + '.tmp'                                 # the loader polls for out: write it whole, then rename
    with open(tmp, 'wb') as fo:
        fo.write(struct.pack('<4sIfffiiiff24x', b'RNV1', 1, x0, y0, cell, w, h, N, STEP, CLEAR_H))
        fo.write(first.astype('<u4').tobytes())
        rec = np.zeros(N, np.dtype([('z', '<f4'), ('comp', '<u2'), ('wd', 'u1'), ('fl', 'u1'), ('nbr', '<i4', 8)]))
        rec['z'] = nzs; rec['comp'] = np.minimum(comp, 65535); rec['wd'] = wd; rec['fl'] = dropf; rec['nbr'] = nbr
        fo.write(rec.tobytes())
    os.replace(tmp, o.out)
    log('wrote %s (%.1f MB) in %.1f s' % (o.out, os.path.getsize(o.out) / 1e6, time.time() - t0))

    if o.png:
        from PIL import Image
        img = np.zeros((h, w, 3), np.uint8)
        top = np.full(w * h, -1, np.int64)
        top[nc] = np.arange(N)                          # the highest node of each cell (nodes sorted by z per cell)
        t = top.reshape(h, w)[::-1]                     # north up
        has = t >= 0
        cm = comp[t[has]]; d_ = wd[t[has]]
        col = np.where(cm[:, None] == 0, np.array([60, 200, 90]), np.array([90, 120, 255]))
        col = (col * (0.45 + 0.55 * np.minimum(d_, 6)[:, None] / 6)).astype(np.uint8)
        img[has] = col
        Image.fromarray(img).save(o.png)
        log('preview %s' % o.png)


if __name__ == '__main__':
    main()
