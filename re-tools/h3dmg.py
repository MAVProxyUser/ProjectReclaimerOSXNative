#!/usr/bin/env python3
"""Damage layout of the vehicles (and bipeds) in a Halo 3 (MCC) .map: where shooting one hurts most.

    python3 re-tools/h3dmg.py <file.map> [name substring] [--bipeds] [--json out.json]
    python3 re-tools/h3dmg.py --table <file.map> [<file.map> ...]     # every vehicle: weak spots, driver exposure (markdown)

Per object definition (vehi/bipd +0x34 model tagref -> hlmt), from the model's "New Damage Info" (Assembly Halo3MCC
hlmt.xml, offsets below):
  body / shield vitality, damage sections (name, % of body vitality, Headshotable / Ignores Shields / Absorbs Body Damage),
  damage seats (seat label, direct damage scale = the share of a hit on the rider that reaches the rider), the model's
  aim targets (marker, damage section, lock-on flags: Headshot / Vulnerable), and which collision-region materials
  feed which damage section (the collision model's surfaces carry a material index; hlmt Materials map it to a section).
Read-only static file work.
"""
import json, os, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from h3map import H3Map  # noqa: E402
from h3bsp import sid_name  # noqa: E402

# hlmt (model)
HLMT_COLL = 0x10            # collision model tagref
HLMT_MATERIALS = 0x7C       # [0x14] +0 name sid, +6 s16 damage section, +0xC global material name sid
HLMT_DAMAGE = 0x88          # New Damage Info [0xA4]
HLMT_TARGETS = 0x94         # [0x20] +0 marker sid, +4 radius, +0xC s16 damage section, +0x18 lock-on flags
HLMT_CREGIONS = 0xA0        # [0x14] +0 name sid, +4 s8 collision region
# New Damage Info element
DI_BODY, DI_SHIELD = 0x0C, 0x20
DI_SECTIONS = 0x68          # [0x2C] +0 name sid, +4 flags, +8 vitality fraction, +0xC instant responses [0x88]
DI_SEATS = 0x8C             # [0x20] +0 seat label sid, +4 direct damage scale, +8 transfer fall-off radius,
                            #        +0xC max transfer scale, +0x10 min transfer scale, +0x14 region-specific [0x2C]
SEC_FLAGS = {0: 'absorbs body damage', 7: 'headshotable', 8: 'ignores shields', 9: 'full damage when shield depleted'}
RESP_FLAGS = {0: 'KILLS', 4: 'drops weapon', 7: 'DESTROYS', 8: 'breaks gun', 9: 'breaks gun 2', 10: 'turn damage',
              11: 'turn damage', 12: 'turn damage', 13: 'turn damage', 14: 'engine damage', 15: 'ENGINE DAMAGE',
              16: 'KILLS (not solo)', 17: 'DETONATES', 22: 'special death', 23: 'only on special death',
              24: 'not on special death'}
RESP_TYPE = {0: 'all', 1: 'area', 2: 'local'}
STATE = {0: 'default', 1: 'minor', 2: 'medium', 3: 'major', 4: 'destroyed'}
TGT_FLAGS = {0: 'human lock', 1: 'plasma lock', 2: 'headshot', 3: 'vulnerable', 4: 'always plasma lock'}
# coll: +0x14 materials [4] (name sid), +0x20 regions [0x10] {+0 name, +4 permutations [0x28] {+0 name, +4 bsps [0x64]}}
# collision bsp element: +0x40 surfaces [0xC] (material s16 at +4; an index into the coll materials)
COLL_MATERIALS, COLL_REGIONS = 0x14, 0x20


def flags(v, names):
    return [n for b, n in names.items() if v & (1 << b)]


def model_of(m, ti):
    d = m.meta(ti, 0x48)
    return m.tagref(d, 0x34)


def surface_materials(m, coll):
    """collision region name -> {collision material index: surface count} (all permutations)"""
    c = m.meta(coll, 0x30)
    out = {}
    for r in m.block(c, COLL_REGIONS, 0x10):
        rn = sid_name(m, struct.unpack_from('<I', r, 0)[0])
        cnt = {}
        for p in m.block(r, 0x04, 0x28):
            for b in m.block(p, 0x04, 0x64):
                for s in m.block(b, 0x40, 0x0C):
                    mat, = struct.unpack_from('<h', s, 4)
                    cnt[mat] = cnt.get(mat, 0) + 1
        out[rn] = cnt
    return out


def describe(m, ti):
    hl = model_of(m, ti)
    if hl < 0 or m.tags[hl][0] != 'hlmt':
        return None
    h = m.meta(hl, 0x100)
    r = {'tag': m.names[ti], 'group': m.tags[ti][0], 'model': m.names[hl]}
    di = m.block(h, HLMT_DAMAGE, 0xA4)
    if not di:
        return r
    d = di[0]
    r['body'], = struct.unpack_from('<f', d, DI_BODY)
    r['shield'], = struct.unpack_from('<f', d, DI_SHIELD)
    secs = []
    for s in m.block(d, DI_SECTIONS, 0x2C):
        nm, fl, vit = struct.unpack_from('<IIf', s, 0)
        resp = []
        for e in m.block(s, 0x0C, 0x88):                        # instant responses (element 0x88)
            ty, = struct.unpack_from('<h', e, 0); rf, thr = struct.unpack_from('<If', e, 8)
            reg, = struct.unpack_from('<I', e, 0x40); st, = struct.unpack_from('<h', e, 0x44)
            seat, = struct.unpack_from('<I', e, 0x78); tot, = struct.unpack_from('<f', e, 0x84)
            resp.append({'type': RESP_TYPE.get(ty, ty), 'threshold': thr, 'flags': sorted(set(flags(rf, RESP_FLAGS))),
                         'region': sid_name(m, reg) if reg else '', 'state': STATE.get(st, st),
                         'ejects': sid_name(m, seat) if seat else '', 'total_threshold': tot})
        secs.append({'name': sid_name(m, nm), 'vitality': vit, 'flags': flags(fl, SEC_FLAGS), 'raw_flags': fl,
                     'responses': resp})
    r['sections'] = secs
    seats = []
    for s in m.block(d, DI_SEATS, 0x20):
        nm, direct, rad, tmax, tmin = struct.unpack_from('<Iffff', s, 0)
        reg = []
        for e in m.block(s, 0x14, 0x2C):
            rn, = struct.unpack_from('<I', e, 0)
            reg.append({'region': sid_name(m, rn), 'scales': [round(x, 3) for x in struct.unpack_from('<9f', e, 8)]})
        seats.append({'seat': sid_name(m, nm), 'direct': direct, 'transfer_radius': rad, 'transfer_max': tmax,
                      'transfer_min': tmin, 'region_specific': reg})
    r['seats'] = seats
    mats = []
    for e in m.block(h, HLMT_MATERIALS, 0x14):
        nm, = struct.unpack_from('<I', e, 0); sec, = struct.unpack_from('<h', e, 6); gm, = struct.unpack_from('<I', e, 0xC)
        mats.append({'name': sid_name(m, nm), 'section': secs[sec]['name'] if 0 <= sec < len(secs) else None,
                     'global': sid_name(m, gm)})
    r['materials'] = mats
    tg = []
    for e in m.block(h, HLMT_TARGETS, 0x20):
        mk, size = struct.unpack_from('<If', e, 0); sec, var = struct.unpack_from('<hh', e, 0xC)
        fl, = struct.unpack_from('<I', e, 0x18)
        tg.append({'marker': sid_name(m, mk), 'radius': size, 'section': secs[sec]['name'] if 0 <= sec < len(secs) else None,
                   'flags': flags(fl, TGT_FLAGS)})
    r['targets'] = tg
    # weak spots, the same rule as the overlay (shim/src/hitbox.c hbg_weak): a section smaller than the body whose
    # response kills / destroys the vehicle, the cheapest section that opens a shielded seat's rider to full hits,
    weak = []
    for s in secs:
        if 0 < s['vitality'] < 0.99 and any(({'KILLS', 'DESTROYS', 'KILLS (not solo)'} & set(x['flags'])) and
                                        'only on special death' not in x['flags'] for x in s['responses']):
            weak.append({'section': s['name'], 'kind': 'kills', 'damage': s['vitality'] * r['body']})
    opens = {x['region'] for st in seats if st['direct'] < 0.5 for x in st['region_specific'] if x['scales'][6] >= 0.9}
    cand = [s for s in secs if any(x['region'] in opens and x['state'] in ('major', 'destroyed') for x in s['responses'])]
    if cand:
        s = min(cand, key=lambda s: s['vitality'])
        if not any(w['section'] == s['name'] for w in weak):
            weak.append({'section': s['name'], 'kind': 'exposes the driver', 'damage': s['vitality'] * r['body']})
    # and sections under half the body whose response disables a gun, the steering or the engine (overlay class 3)
    dis = {'drops weapon', 'breaks gun', 'breaks gun 2', 'turn damage', 'engine damage', 'ENGINE DAMAGE'}
    for s in secs:
        if 0 < s['vitality'] < 0.5 and not any(w['section'] == s['name'] for w in weak) and any(
                (dis & set(x['flags'])) and 'only on special death' not in x['flags'] for x in s['responses']):
            fl = sorted({f for x in s['responses'] for f in x['flags'] if f in dis})
            weak.append({'section': s['name'], 'kind': 'disables (%s)' % ', '.join(fl), 'damage': s['vitality'] * r['body']})
    for w in weak:
        w['materials'] = [x['name'] for x in mats if x['section'] == w['section']]
    r['weak'] = weak
    coll = m.tagref(h, HLMT_COLL)
    if coll >= 0 and m.tags[coll][0] == 'coll':
        c = m.meta(coll, 0x30)
        cm = [sid_name(m, struct.unpack_from('<I', e, 0)[0]) for e in m.block(c, COLL_MATERIALS, 4)]
        byname = {x['name']: x['section'] for x in mats}
        try:
            regions = surface_materials(m, coll)
            r['regions'] = {rn: {cm[k] if 0 <= k < len(cm) else str(k): (n, byname.get(cm[k]) if 0 <= k < len(cm) else None)
                                 for k, n in sorted(cnt.items())} for rn, cnt in regions.items()}
        except Exception as ex:                                # layout guess wrong for this model: say so, keep the rest
            r['regions_error'] = str(ex)
    return r


def show(r):
    print('%s  (model %s)' % (r['tag'], r['model']))
    if 'body' not in r:
        print('   no damage info'); return
    print('   body vitality %g, shield %g' % (r['body'], r['shield']))
    for w in r['weak']:
        print('   WEAK SPOT %s: %s after %.0f damage to it (collision materials %s)' % (
            w['section'], w['kind'], w['damage'], ', '.join(w['materials']) or 'none: cannot be hit directly'))
    for s in r['sections']:
        print('   section %-22s %5.0f%% of body  %s' % (s['name'], s['vitality'] * 100, ', '.join(s['flags'])))
        for x in s['responses']:
            print('            at %4.0f%% left (%s): %s%s%s' % (x['threshold'] * 100, x['type'], ', '.join(x['flags']) or '-',
                  '  %s -> %s' % (x['region'], x['state']) if x['region'] else '',
                  '  ejects %s' % x['ejects'] if x['ejects'] else ''))
    for s in r['seats']:
        print('   seat    %-22s direct damage x%.2f   transfer x%.2f..%.2f within %.2f%s' % (
            s['seat'], s['direct'], s['transfer_min'], s['transfer_max'], s['transfer_radius'],
            ''.join('  [%s %s]' % (x['region'], x['scales']) for x in s['region_specific'])))
    for t in r['targets']:
        print('   target  %-22s r=%.2f section=%s %s' % (t['marker'], t['radius'], t['section'], ', '.join(t['flags'])))
    if r['materials']:
        print('   materials: ' + ', '.join('%s->%s' % (x['name'], x['section']) for x in r['materials']))
    for rn, mm in r.get('regions', {}).items():
        print('   region  %-22s ' % rn + ', '.join('%s:%d->%s' % (k, n, sec) for k, (n, sec) in mm.items()))
    if 'regions_error' in r:
        print('   (collision regions unreadable: %s)' % r['regions_error'])


def table(paths):
    """--table <map> [<map> ...]: one markdown row per vehicle definition (first map it is found in): its weak spots, the
    driver seat's direct damage scale, maps. The overlay's rule, so this is what the overlay draws for every vehicle."""
    rows, where = {}, {}
    for p in paths:
        mn = os.path.basename(p)[:-4]
        try:
            m = H3Map(p)
        except Exception as ex:
            print('<!-- %s: %s -->' % (mn, ex)); continue
        for ti in m.find('vehi', ''):
            if not m.tags[ti][2]:
                continue
            t = m.names[ti].split('\\')[-1]
            where.setdefault(t, []).append(mn)
            if t in rows:
                continue
            try:
                r = describe(m, ti)
            except Exception:
                r = None
            if r:
                rows[t] = r
    print('| Vehicle | Weak spots (damage to that part) | Driver takes | Maps |')
    print('|---|---|---|---|')
    for t in sorted(rows):
        r = rows[t]
        ws = '; '.join('%s: %s (%.0f%s)' % (w['section'], w['kind'], w['damage'],
                       '' if w['materials'] else ', no surface: cannot be hit') for w in r.get('weak', [])) or '-'
        dr = ('%.0f%%' % (r['seats'][0]['direct'] * 100)) if r.get('seats') else '-'
        mp = where.get(t, [])
        print('| %s | %s | %s | %s |' % (t, ws, dr, ', '.join(mp[:4]) + (' +%d' % (len(mp) - 4) if len(mp) > 4 else '')))


def main():
    a = sys.argv[1:]
    if not a:
        print(__doc__); sys.exit(2)
    if a[0] == '--table':
        table(a[1:]); return
    js = None
    if '--json' in a:
        i = a.index('--json'); js = a[i + 1]; del a[i:i + 2]
    groups = ['vehi'] + (['bipd'] if '--bipeds' in a else [])
    a = [x for x in a if x != '--bipeds']
    m = H3Map(a[0]); sub = a[1] if len(a) > 1 else ''
    out = []
    for g in groups:
        for ti in m.find(g, sub):
            if not m.tags[ti][2]:
                continue
            r = describe(m, ti)
            if r:
                out.append(r); show(r); print()
    if js:
        with open(js, 'w') as f:
            json.dump(out, f, indent=1)


if __name__ == '__main__':
    main()
