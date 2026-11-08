#!/usr/bin/env python3
"""Learn how real players move and fight from shim/tools/match-recorder.py's recordings (2026-11-08), for the walkers (nbot.c).
    python3 shim/tools/walker-model.py [recordings dir] [--out model.json] [--report]
Default dir ~/Library/Application Support/ReclaimerMac/tracks, default out <dir>/walker-model.json (nbot.c reads it:
NHOOK_BOT_MODEL). The observer's own player ("you") is left out. Per player and sample (10 a second): position, look (yaw /
pitch), alive, shield, health, weapon, vehicle. Measured:
  move   on foot, out of fights: speed (median / 90th percentile), share of time standing, jumps a minute, turn rate
  fight  an ENGAGEMENT = i alive and on foot, j alive, within 40 units, i's look within 10 deg of the direction to j for 0.3 s
         or more: distance (quartiles), strafe (the speed across the line to j: RMS, and how long between direction changes),
         closing speed (towards / away), backing off when shield < 0.35, jumps a minute, aim error (look vs the direction to
         j: median / 75th percentile), reaction (from j first in reach + within 30 deg to within 5 deg), fire (projectiles
         in flight going up while i engages, as a rough rate)
  maps   per map: a flow field (2-unit cells: the mean direction players walk through it, out of fights) and a heat map
         (where they are), for roaming like players
"""
import gzip, json, math, os, sys, glob, struct, statistics as st
from collections import defaultdict

def arg(k, d):
    a = sys.argv; return a[a.index(k) + 1] if k in a else d
DIR = next((a for a in sys.argv[1:] if not a.startswith('--') and os.path.isdir(a)), os.path.expanduser('~/Library/Application Support/ReclaimerMac/tracks'))
OUT = arg('--out', os.path.join(DIR, 'walker-model.json')); CELL = 2.0
R_FIGHT, AIM_ON, MIN_ENG = 40.0, 10.0, 0.3

def look_vec(yaw, pitch):
    y, p = math.radians(yaw), math.radians(pitch); return (math.cos(p) * math.cos(y), math.cos(p) * math.sin(y), math.sin(p))
def ang(a, b):
    d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; na = math.sqrt(a[0] ** 2 + a[1] ** 2 + a[2] ** 2); nb = math.sqrt(b[0] ** 2 + b[1] ** 2 + b[2] ** 2)
    return math.degrees(math.acos(max(-1, min(1, d / (na * nb))))) if na > 1e-6 and nb > 1e-6 else 180.0
def pct(v, q):
    if not v: return None
    v = sorted(v); k = (len(v) - 1) * q; f = int(k); c = min(f + 1, len(v) - 1); return v[f] + (v[c] - v[f]) * (k - f)

def load(path):
    head, samples, events = None, [], []
    try:
        with gzip.open(path, 'rt') as f:
            for l in f:
                try: o = json.loads(l)
                except Exception: break          # (a file still being written: its last line may be cut)
                if 'start' in o: head = o
                elif 'p' in o: samples.append(o)
                elif 'ev' in o: events.append(o)
    except (EOFError, OSError): pass
    return head, samples, events

def main():
    files = sorted(glob.glob(os.path.join(DIR, '*.jsonl.gz')))
    M = defaultdict(list); maps = {}; n_samples = 0; players = set(); eng_count = 0; fight_secs = 0.0; roam_secs = 0.0; kills = []
    for path in files:
        head, S, E = load(path)
        if not head or len(S) < 50: continue
        mp = head.get('map') or '?'; n_samples += len(S)
        flow = maps.setdefault(mp, {'flow': defaultdict(lambda: [0.0, 0.0, 0]), 'heat': defaultdict(int)})
        # per slot series (alive, on foot, not the observer)
        ser = defaultdict(list)
        for s in S:
            you = s.get('you', -1)
            for r in s['p']:
                sl, team, alive, x, y, z, yaw, pitch, sh, hp, wpn, veh = r[:12]
                if sl == you: continue
                ser[sl].append((s['t'], alive, x, y, z, yaw, pitch, sh, hp, wpn, veh, team, r[14] if len(r) > 15 else 0, r[15] if len(r) > 15 else 0))
        for sl in ser: players.add((path, sl))
        # velocities (0.3 s central differences inside continuous alive stretches)
        vel = {}
        for sl, a in ser.items():
            v = [None] * len(a)
            for i in range(3, len(a) - 3):
                p0, p1 = a[i - 3], a[i + 3]
                if not (p0[1] and p1[1] and a[i][1]) or p0[10] or p1[10] or a[i][10]: continue
                dt = p1[0] - p0[0]
                if dt <= 0 or dt > 1.0: continue
                vx, vy, vz = (p1[2] - p0[2]) / dt, (p1[3] - p0[3]) / dt, (p1[4] - p0[4]) / dt
                if math.hypot(vx, vy) > 12: continue        # (a respawn / teleport)
                v[i] = (vx, vy, vz)
            vel[sl] = v
        # index samples by time for pairing: per sample index the rows (series are aligned with S by construction only per slot)
        times = sorted({s['t'] for s in S}); tindex = {t: k for k, t in enumerate(times)}
        at = defaultdict(dict)   # time index -> slot -> (series index)
        for sl, a in ser.items():
            for i, e in enumerate(a): at[tindex[e[0]]][sl] = i
        engaged = defaultdict(set)   # slot -> time indices in a fight
        engs = []                    # (slot, engagement) of this file
        # engagements
        for sl, a in ser.items():
            cur = None   # {target, rows, pre}
            for i, e in enumerate(a):
                t, alive, x, y, z, yaw, pitch, sh, hp, wpn, veh, team = e[:12]
                if not alive or veh or (yaw == 0 and pitch == 0):
                    if cur: engs.append((sl, cur)); cur = None
                    continue
                lv = look_vec(yaw, pitch); best = None
                for sj, j in at[tindex[t]].items():
                    if sj == sl: continue
                    o = ser[sj][j]
                    if not o[1] or (team >= 0 and o[11] == team): continue   # (a teammate; in a free-for-all every player has a team of its own)
                    d = (o[2] - x, o[3] - y, (o[4] + 0.45) - (z + 0.6)); dist = math.sqrt(d[0] ** 2 + d[1] ** 2 + d[2] ** 2)
                    if dist > R_FIGHT or dist < 0.5: continue
                    er = ang(lv, d)
                    if best is None or er < best[1]: best = (sj, er, dist, d)
                if best and best[1] < AIM_ON:
                    if cur and cur['tgt'] == best[0] and t - cur['rows'][-1]['t'] < 0.35: cur['rows'].append({'t': t, 'i': i, 'err': best[1], 'dist': best[2], 'd': best[3], 'sh': sh})
                    else:
                        if cur: engs.append((sl, cur))
                        cur = {'tgt': best[0], 'rows': [{'t': t, 'i': i, 'err': best[1], 'dist': best[2], 'd': best[3], 'sh': sh}], 'pre': None}
                        # reaction: look back up to 3 s for when the target came within reach + 30 deg
                        k = i
                        while k > 0 and a[k][0] > t - 3:
                            k -= 1; ek = a[k]; jj = at[tindex[ek[0]]].get(best[0])
                            if jj is None or not ek[1]: break
                            o = ser[best[0]][jj]; d2 = (o[2] - ek[2], o[3] - ek[3], (o[4] + 0.45) - (ek[4] + 0.6))
                            if math.sqrt(sum(c * c for c in d2)) > R_FIGHT or ang(look_vec(ek[5], ek[6]), d2) > 30: break
                        cur['pre'] = t - a[k][0]
                elif cur and t - cur['rows'][-1]['t'] > 0.35:
                    engs.append((sl, cur)); cur = None
            if cur: engs.append((sl, cur))
        # per engagement measures
        for (sl, c) in engs:
            rows = c['rows']
            if rows[-1]['t'] - rows[0]['t'] < MIN_ENG: continue
            eng_count += 1; fight_secs += rows[-1]['t'] - rows[0]['t']
            if c['pre'] is not None: M['reaction'].append(c['pre'])
            a0, a1 = ser[sl][rows[0]['i']], ser[sl][rows[-1]['i']]; dur = rows[-1]['t'] - rows[0]['t']
            if a1[12] >= a0[12]: M['shots_fight'].append((a1[12] - a0[12], dur)); M['hits_fight'].append((a1[13] - a0[13], a1[12] - a0[12]))
            lat, rad, rev, last = [], [], [], None
            for r in rows:
                engaged[sl].add(tindex[r['t']]); M['dist'].append(r['dist']); M['aim_err'].append(r['err'])
                v = vel[sl][r['i']]
                if v is None: continue
                dx, dy = r['d'][0], r['d'][1]; h = math.hypot(dx, dy)
                if h < 0.1: continue
                ux, uy = dx / h, dy / h; lv = -v[0] * uy + v[1] * ux; rv = v[0] * ux + v[1] * uy
                lat.append(lv); rad.append(rv); M['strafe'].append(abs(lv)); M['close'].append(rv)
                if r['sh'] < 0.35: M['close_low'].append(rv)
                else: M['close_high'].append(rv)
                sgn = 1 if lv > 0.5 else -1 if lv < -0.5 else 0
                if sgn and last is not None and sgn != last[0]: rev.append(r['t'] - last[1])
                if sgn and (last is None or sgn != last[0]): last = (sgn, r['t'])
                if v[2] > 2.5: M['jump_samples_fight'].append(1)
            M['strafe_rev'].extend(rev)
        # out of fights: speed, standing, flow, heat, jumps
        for sl, a in ser.items():
            v = vel[sl]
            for i, e in enumerate(a):
                if v[i] is None: continue
                cx, cy = round(e[2] / CELL), round(e[3] / CELL); flow['heat'][(cx, cy, round(e[4] / 1.5))] += 1   # (with a floor level: multi-storey maps)
                if tindex[e[0]] in engaged[sl]: continue
                sp = math.hypot(v[i][0], v[i][1]); M['speed'].append(sp); roam_secs += 0.1
                if v[i][2] > 2.5: M['jump_samples_roam'].append(1)
                if sp > 1.0: f = flow['flow'][(cx, cy)]; f[0] += v[i][0] / sp; f[1] += v[i][1] / sp; f[2] += 1
        for ev in E:
            if ev.get('ev') == 'kill': kills.append(ev.get('how') or '')
    sp = M['speed']; moving = [x for x in sp if x > 1.0]
    model = {'version': 1, 'source': {'files': len(files), 'samples': n_samples, 'players': len(players), 'engagements': eng_count,
                                      'fight_seconds': round(fight_secs, 1), 'roam_seconds': round(roam_secs, 1), 'kills': len(kills)},
             'move': {'speed_median': pct(moving, 0.5), 'speed_p90': pct(moving, 0.9), 'stand_share': (len(sp) - len(moving)) / len(sp) if sp else None,
                      'jumps_per_min': len(M['jump_samples_roam']) / 3.0 / (roam_secs / 60) if roam_secs else None},
             'fight': {'dist_p25': pct(M['dist'], 0.25), 'dist_median': pct(M['dist'], 0.5), 'dist_p75': pct(M['dist'], 0.75),
                       'strafe_rms': math.sqrt(st.mean([x * x for x in M['strafe']])) if M['strafe'] else None, 'strafe_median': pct(M['strafe'], 0.5),
                       'strafe_switch_s': pct(M['strafe_rev'], 0.5), 'close_median': pct(M['close'], 0.5),
                       'close_low_shield': pct(M['close_low'], 0.5), 'close_high_shield': pct(M['close_high'], 0.5),
                       'aim_err_median': pct(M['aim_err'], 0.5), 'aim_err_p75': pct(M['aim_err'], 0.75), 'reaction_s': pct(M['reaction'], 0.5),
                       'jumps_per_min': len(M['jump_samples_fight']) / 3.0 / (fight_secs / 60) if fight_secs else None,
                       'shots_per_s': sum(a for a, d in M['shots_fight']) / sum(d for a, d in M['shots_fight']) if M['shots_fight'] and sum(d for a, d in M['shots_fight']) > 0 else None,
                       'hit_share': sum(h for h, n in M['hits_fight']) / sum(n for h, n in M['hits_fight']) if M['hits_fight'] and sum(n for h, n in M['hits_fight']) > 0 else None},
             'weapons_in_kills': {w: kills.count(w) for w in sorted(set(kills), key=lambda w: -kills.count(w))[:12]},
             'maps': {}}
    # the stats page's footprint heatmaps (ReclaimerMac/heatmaps/<map>.bin, RHM2: every match the user played on a map, loader era
    # included -- 0.5-unit cells, a count each; no height: z = -9999 means "every floor in that spot"; the death records' names are
    # not read). They add to the recorded heat, so maps never recorded still get the players' lanes.
    hdir = os.path.expanduser(arg('--heatmaps', '~/Library/Application Support/ReclaimerMac/heatmaps'))
    for hp in glob.glob(os.path.join(hdir, '*.bin')):
        mp = os.path.basename(hp)[:-4]
        try:
            b = open(hp, 'rb').read()
            if b[:4] != b'RHM2': continue
            cell, nc = struct.unpack_from('<fI', b, 4); agg = defaultdict(int)
            for i in range(nc):
                x, y, n = struct.unpack_from('<iiI', b, 20 + 12 * i); agg[(round(x * cell / CELL), round(y * cell / CELL), -6666)] += n
            fl = maps.setdefault(mp, {'flow': defaultdict(lambda: [0.0, 0.0, 0]), 'heat': defaultdict(int)})
            for k, n in agg.items(): fl['heat'][k] += n
        except Exception as e: print('heatmap %s: %s' % (hp, e))
    for mp, fl in maps.items():
        fcells = [[k[0] * CELL, k[1] * CELL, round(v[0] / v[2], 3), round(v[1] / v[2], 3), v[2]] for k, v in fl['flow'].items() if v[2] >= 3]
        hcells = [[k[0] * CELL, k[1] * CELL, round(k[2] * 1.5, 1), n] for k, n in fl['heat'].items() if n >= 3]   # x, y, z (-9999: any floor), samples
        model['maps'][mp] = {'cell': CELL, 'flow': fcells, 'heat': hcells}
    json.dump(model, open(OUT, 'w'), indent=1)
    if '--report' in sys.argv or True:
        print(json.dumps({k: v for k, v in model.items() if k != 'maps'}, indent=1))
        print('maps:', {m: (len(v['flow']), len(v['heat'])) for m, v in model['maps'].items()}, '->', OUT)

if __name__ == '__main__':
    main()
