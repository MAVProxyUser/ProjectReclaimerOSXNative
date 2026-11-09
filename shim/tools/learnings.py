#!/usr/bin/env python3
"""What the game is learning (2026-11-08; the user: "add one more service that tells what the game is learning about everything from
these stats? best lane to not get killed on a map, best weapon to have overall, best weapon per map, best tactics in each situation
1:1 1:3, distance shots with hitscan weapons like sniper or laser, grenade tactics, jumping tactics, strafe tactics, team cohesion,
best way to survive vs specific weapons (stay certain distance? strafe? jump? catch the player reloading?), etc. ... you should be
able to derive more stats simply because you know the dataset better").

Reads the engagement library (tracks/engagements: duels and the rest, engage-extract.py), the lanes (heatmaps/<map>.lanes), the
deaths heatmaps (heatmaps/<map>.bin) and the spawn points (heatmaps/<map>.spawns), and writes plain-language findings with their
sample sizes to tracks/learnings.json (the work board's "What the game is learning" panel) and prints them. Infection is left out
of the fighting findings (zombies rush with swords), kept in nothing but its own lines. Every number says how many cases it rests on;
a finding needs MIN cases (default 30) or it is listed as "not enough data yet".
    python3 shim/tools/learnings.py [--min 30] [--quiet]"""
import glob, gzip, json, math, os, struct, sys, time, importlib.util
from collections import defaultdict, Counter

SUP = os.path.expanduser('~/Library/Application Support/ReclaimerMac'); TR = os.path.join(SUP, 'tracks'); HM = os.path.join(SUP, 'heatmaps')
def arg(k, d):
    a = sys.argv; return a[a.index(k) + 1] if k in a else d
MIN = int(arg('--min', '30')); QUIET = '--quiet' in sys.argv
HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location('li', os.path.join(HERE, 'lanes-import.py')); li = importlib.util.module_from_spec(spec); _a = sys.argv; sys.argv = ['x']; spec.loader.exec_module(li); sys.argv = _a
H3 = {'armory': "Rat's Nest", 'bunkerworld': 'Standoff', 'chill': 'Narrows', 'chillout': 'Cold Storage', 'construct': 'Construct', 'cyberdyne': 'The Pit',
      'deadlock': 'High Ground', 'descent': 'Assembly', 'docks': 'Longshore', 'fortress': 'Citadel', 'ghosttown': 'Ghost Town', 'guardian': 'Guardian',
      'isolation': 'Isolation', 'lockout': 'Blackout', 'midship': 'Heretic', 'riverworld': 'Valhalla', 'salvation': 'Epitaph', 'sandbox': 'Sandbox',
      'shrine': 'Sandtrap', 'sidewinder': 'Avalanche', 'snowbound': 'Snowbound', 'spacecamp': 'Orbital', 'warehouse': 'Foundry', 'zanzibar': 'Last Resort'}
def mname(m): return H3.get(m, m)
def pc(x): return '%.0f%%' % (100 * x)
def wname(w):   # the HUD's weapon names, readable
    w = (w or '').lower(); return {'excavator': 'Mauler', 'smg': 'SMG', 'covenant carbine': 'Carbine'}.get(w, w.title() if w else '?')
def wcls(w):
    w = (w or '').upper()
    for c, keys in (('sniper', ('SNIPER', 'BEAM RIFLE')), ('laser', ('SPARTAN LASER',)), ('rocket', ('ROCKET', 'FUEL ROD', 'MISSILE')), ('shotgun', ('SHOTGUN', 'EXCAVATOR', 'MAULER')),
                    ('sword', ('ENERGY BLADE', 'ENERGY SWORD', 'HAMMER')), ('precision', ('BATTLE RIFLE', 'CARBINE', 'MAGNUM')), ('auto', ('ASSAULT RIFLE', 'SMG', 'PLASMA RIFLE', 'SPIKER', 'NEEDLER', 'BRUTE SHOT'))):
        if any(k in w for k in keys): return c
    return 'other'

def load():
    R = []
    for p in sorted(glob.glob(os.path.join(TR, 'engagements', '*.eng.jsonl.gz'))):
        try:
            with gzip.open(p, 'rt') as f:
                for l in f:
                    try: R.append(json.loads(l))
                    except Exception: break
        except (EOFError, OSError): pass
    return R

def rate(cases, ok):
    n = len(cases); return (sum(1 for c in cases if ok(c)) / n, n) if n else (None, 0)
def duel_features(e):
    """per duel: both sides' strafing (RMS of the speed across), jumps, closing, fire pauses, from the rows after the first attack"""
    rows = [r for r in e['rows'] if r[0] >= e['attack_t'] - 1e-6]
    def rms(i): v = [r[i] for r in rows if r[i] is not None]; return math.sqrt(sum(x * x for x in v) / len(v)) if v else None
    def jumps(i): return sum(1 for r in rows if r[i] is not None and r[i] > 2.5)
    def mean(i): v = [r[i] for r in rows if r[i] is not None]; return sum(v) / len(v) if v else None
    gap = 0; last = None   # the attacker's longest pause in firing (s) while the fight lasts (a reload, a lost aim)
    for r in rows:
        if r[12]:
            if last is not None: gap = max(gap, r[0] - last)
            last = r[0]
    return {'a_strafe': rms(2), 'b_strafe': rms(4), 'a_jumps': jumps(14), 'b_jumps': jumps(15), 'a_close': mean(3), 'b_close': mean(5), 'a_gap': gap, 'dur': rows[-1][0] - rows[0][0] if rows else 0}

def main():
    R = load(); now = time.time()
    D = [e for e in R if e.get('kind', 'duel') == 'duel' and e['class'] != 'infection' and not e['ctx']['a_veh'] and not e['ctx']['b_veh']]
    dec = [e for e in D if e['out']['win'] in ('a', 'b')]
    for e in dec: e['f'] = duel_features(e)
    S = []   # sections
    def sec(topic, items): S.append({'topic': topic, 'items': [i for i in items if i]})
    def item(title, text, n, mp=None, weak=False): return {'title': title, 'text': text, 'n': n, 'map': mp, 'weak': weak or n < MIN}
    meta = [r for r in R if r.get('kind') == 'meta']
    src = {'recordings': len(meta), 'minutes': round(sum(r['seconds'] for r in meta) / 60), 'duels': len(D), 'decided': len(dec), 'maps': len({r['map'] for r in meta})}

    # ---- the fundamentals
    f, n = rate(dec, lambda e: e['out']['win'] == 'a')
    away = [e for e in dec if e['ctx']['b_err'] > 60]; fa, na_ = rate(away, lambda e: e['out']['win'] == 'b')
    face = [e for e in dec if e['ctx']['b_err'] <= 20]; ff, nf = rate(face, lambda e: e['out']['win'] == 'b')
    lead = [e for e in dec if (e['ctx']['a_sh'] + e['ctx']['a_hp']) - (e['ctx']['b_sh'] + e['ctx']['b_hp']) > 0.5]; fl, nl = rate(lead, lambda e: e['out']['win'] == 'a')
    behind = [e for e in dec if (e['ctx']['a_sh'] + e['ctx']['a_hp']) - (e['ctx']['b_sh'] + e['ctx']['b_hp']) < -0.5]; fb, nb = rate(behind, lambda e: e['out']['win'] == 'a')
    above = [e for e in dec if -e['ctx']['dz'] > 2]; fab, nab = rate(above, lambda e: e['out']['win'] == 'a'); level = [e for e in dec if abs(e['ctx']['dz']) <= 0.5]; flv, nlv = rate(level, lambda e: e['out']['win'] == 'a')
    sec('The fundamentals', [
        item('Shoot first', 'Whoever opens fire wins %s of the duels that end in a kill.' % pc(f), n) if n else None,
        item('Never get caught looking away', 'A defender looking more than 60 deg away when shot wins only %s; one already facing the attacker wins %s.' % (pc(fa), pc(ff)), min(na_, nf)) if na_ and nf else None,
        item('Pick fights with your shields up', 'Attacking with a lead of half a bar or more wins %s; attacking from behind by as much wins %s.' % (pc(fl), pc(fb)), min(nl, nb)) if nl and nb else None,
        item('Take the high ground', 'Attacking from 2+ units above wins %s, on level ground %s.' % (pc(fab), pc(flv)), min(nab, nlv)) if nab and nlv else None])

    # ---- weapons: overall and per map (held time, kills, deaths from the "weapons" records; duels for win rates)
    W = [r for r in R if r.get('kind') == 'weapons' and r['class'] != 'infection']
    def wtable(rs, min_min):
        held, kh, dh = Counter(), Counter(), Counter()
        for r in rs: held.update(r['held_s']); kh.update(r['kills_holding']); dh.update(r['deaths_holding'])
        out = []
        for w, s_ in held.items():
            mn = s_ / 60.0
            if mn < min_min: continue
            out.append((w, kh[w] / mn, kh[w] / max(1, dh[w]), kh[w], dh[w], mn))
        return out
    wt = wtable(W, 10)
    items = []
    if wt:
        by_kd = sorted(wt, key=lambda x: -x[2]); by_kpm = sorted(wt, key=lambda x: -x[1])
        items.append(item('Best weapon overall (K/D)', ', '.join('%s %.2f' % (wname(w), kd) for w, kpm, kd, k, d, mn in by_kd[:4]) + ' -- kills per death of the players holding it.', sum(x[3] + x[4] for x in by_kd[:4])))
        items.append(item('Fastest killers (kills a minute held)', ', '.join('%s %.1f' % (wname(w), kpm) for w, kpm, kd, k, d, mn in by_kpm[:4]) + '.', sum(x[3] for x in by_kpm[:4])))
        worst = by_kd[-2:]; items.append(item('Weakest in hand', ', '.join('%s %.2f K/D' % (wname(w), kd) for w, kpm, kd, k, d, mn in worst) + ' -- swap them when you can.', sum(x[3] + x[4] for x in worst)))
    for mp in sorted({r['map'] for r in W}):
        t2 = wtable([r for r in W if r['map'] == mp], 3)
        t2 = [x for x in t2 if x[3] + x[4] >= 15]
        if len(t2) >= 2:
            b = max(t2, key=lambda x: x[2]); items.append(item('Best weapon on %s' % mname(mp), '%s: K/D %.2f, %.1f kills a minute held (%d kills, %d deaths).' % (wname(b[0]), b[2], b[1], b[3], b[4]), b[3] + b[4], mp))
    sec('Weapons', items)

    # ---- range: precision and hitscan shots by distance (the attacker's weapon class)
    items = []
    for c, label in (('sniper', 'Sniper'), ('laser', 'Spartan Laser'), ('precision', 'Battle Rifle / Carbine / Magnum'), ('auto', 'Assault Rifle / SMG / automatics'), ('shotgun', 'Shotgun / Mauler')):
        sub = [e for e in dec if wcls(e['ctx']['a_wpn']) == c]
        if len(sub) < 10: continue
        bands = [(0, 8), (8, 16), (16, 26), (26, 45)]; parts = []
        for lo, hi in bands:
            b = [e for e in sub if lo <= e['ctx']['dist'] < hi]
            if len(b) >= 8: parts.append('%d-%d u: %s (%d)' % (lo, hi, pc(sum(e['out']['win'] == 'a' for e in b) / len(b)), len(b)))
        if parts:
            best = max(((lo, hi, sum(e['out']['win'] == 'a' for e in [e for e in sub if lo <= e['ctx']['dist'] < hi]) / max(1, len([e for e in sub if lo <= e['ctx']['dist'] < hi])), len([e for e in sub if lo <= e['ctx']['dist'] < hi])) for lo, hi in bands), key=lambda x: (x[3] >= 8, x[2]))
            items.append(item('%s: where it wins' % label, 'Attacker wins by distance -- ' + ', '.join(parts) + '. Best at %d-%d units.' % (best[0], best[1]), len(sub)))
    sec('Range and hitscan', items)

    # ---- outnumbered (1:1 vs 1:2+) and team cohesion
    P = [r for r in R if r.get('kind') == 'pressure' and r['class'] != 'infection']
    items = []
    one = [p for p in P if p['level'] == 1]; two = [p for p in P if p['level'] == 2]
    if one and two:
        items.append(item('1v1 vs outnumbered', 'Under one attacker players die %s of the time; under two or more %s -- and still get %.2f kills per stretch (vs %.2f).' % (
            pc(sum(p['died'] for p in one) / len(one)), pc(sum(p['died'] for p in two) / len(two)), sum(p['kills'] for p in two) / len(two), sum(p['kills'] for p in one) / len(one)), min(len(one), len(two))))
        ret = [p for p in two if p['retreat_share'] > 0.3]; std = [p for p in two if p['retreat_share'] <= 0.3]
        if ret and std:
            rr, rs = sum(p['died'] for p in ret) / len(ret), sum(p['died'] for p in std) / len(std)
            verdict = 'no real difference' if abs(rr - rs) < 0.02 else ('retreating does better' if rr < rs else 'standing your ground does better')
            items.append(item('Outnumbered: run or stand?', 'Retreating under 2+ attackers: %s die; standing: %s -- %s.' % (pc(rr), pc(rs), verdict), min(len(ret), len(std))))
        wm = [p for p in two if p.get('mates', 0) > 0]; al = [p for p in two if p.get('mates', 0) <= 0]
        if wm and al: items.append(item('Outnumbered: have a teammate near', 'Under 2+ attackers with a teammate within 15 units %s die; alone %s.' % (pc(sum(p['died'] for p in wm) / len(wm)), pc(sum(p['died'] for p in al) / len(al))), min(len(wm), len(al))))
    AD = [r for r in R if r.get('kind') == 'aid' and r['class'] != 'infection']
    if AD:
        ai = [r for r in AD if r['aided']]; na = [r for r in AD if not r['aided']]
        if ai and na: items.append(item('Teammates save lives', 'Attacked with a teammate helping within 3 s: %s dead 5 s later; nobody helping: %s. Help came %s of the time, %s of it by shooting the attacker.' % (
            pc(sum(r['died5'] for r in ai) / len(ai)), pc(sum(r['died5'] for r in na) / len(na)), pc(len(ai) / len(AD)), pc(sum(r['aid_kind'] == 'fire' for r in ai) / len(ai))), min(len(ai), len(na))))
    TM = [r for r in R if r.get('kind') == 'team']
    if TM:
        for lab, sub in (('small team games (12 players or fewer)', [r for r in TM if r.get('players', 99) <= 12]), ('large team games', [r for r in TM if r.get('players', 0) > 12])):
            if sub:
                w = sum(r['samples'] for r in sub); m = sum(r['near_median'] * r['samples'] for r in sub) / w; w15 = sum(r['within15'] * r['samples'] for r in sub) / w
                fo = [r for r in sub if r.get('focus_share') is not None]
                items.append(item('Cohesion in %s' % lab, 'The nearest teammate is %.1f units away (median); one is within 15 units %s of the time%s.' % (m, pc(w15), '; %s of attackers share their target with a teammate' % pc(sum(r['focus_share'] for r in fo) / len(fo)) if fo else ''), len(sub), weak=len(sub) < 3))
    sec('Outnumbered and teamwork', items)

    # ---- movement in fights: strafe, jump, close in
    items = []
    def split_rate(cases, key, side, win_side):
        v = [e for e in cases if e['f'][key] is not None]
        if len(v) < 2 * MIN: return None
        v.sort(key=lambda e: e['f'][key]); lo, hi = v[:len(v) // 3], v[-len(v) // 3:]
        r_lo = sum(e['out']['win'] == win_side for e in lo) / len(lo); r_hi = sum(e['out']['win'] == win_side for e in hi) / len(hi)
        return r_lo, r_hi, lo[-1]['f'][key], hi[0]['f'][key], len(v)
    s_ = split_rate(dec, 'b_strafe', 'b', 'b')
    if s_: items.append(item('Strafe when you are shot', 'Defenders who strafed hardest (RMS %.1f+ u/s) won %s of their duels; the least strafing (under %.1f) %s.' % (s_[3], pc(s_[1]), s_[2], pc(s_[0])), s_[4]))
    s_ = split_rate(dec, 'a_strafe', 'a', 'a')
    if s_: items.append(item('Strafe while you attack', 'Attackers strafing hardest won %s; the least strafing %s.' % (pc(s_[1]), pc(s_[0])), s_[4]))
    jb = [e for e in dec if e['f']['b_jumps'] > 0]; nj = [e for e in dec if e['f']['b_jumps'] == 0]
    if jb and nj: items.append(item('Jumping under fire', 'Defenders who jumped during the fight won %s; those who stayed on the ground %s.' % (pc(sum(e['out']['win'] == 'b' for e in jb) / len(jb)), pc(sum(e['out']['win'] == 'b' for e in nj) / len(nj))), min(len(jb), len(nj))))
    ja = [e for e in dec if e['f']['a_jumps'] > 0]; nja = [e for e in dec if e['f']['a_jumps'] == 0]
    if ja and nja: items.append(item('Jumping while attacking', 'Attackers who jumped won %s; grounded attackers %s.' % (pc(sum(e['out']['win'] == 'a' for e in ja) / len(ja)), pc(sum(e['out']['win'] == 'a' for e in nja) / len(nja))), min(len(ja), len(nja))))
    s_ = split_rate([e for e in dec if e['f']['a_close'] is not None], 'a_close', 'a', 'a')
    if s_: items.append(item('Push or hold', 'Attackers who pushed in fastest (%.1f+ u/s toward the enemy) won %s; those backing off (%.1f or less) %s.' % (s_[3], pc(s_[1]), s_[2], pc(s_[0])), s_[4]))
    sec('Movement in a fight', items)

    # ---- grenades
    G = [e for e in dec if e['out'].get('gren_known')]
    items = []
    if G:
        tg = [e for e in G if e['out'].get('a_throws')]; ng = [e for e in G if not e['out'].get('a_throws')]
        if tg and ng: items.append(item('Open with a grenade?', 'Attackers who threw a grenade in the duel won %s; without one %s.' % (pc(sum(e['out']['win'] == 'a' for e in tg) / len(tg)), pc(sum(e['out']['win'] == 'a' for e in ng) / len(ng))), min(len(tg), len(ng))))
        th = sorted(x[1] for e in G for x in (e['out'].get('a_throws') or []) + (e['out'].get('b_throws') or []) if e['out']['win'] in ('a', 'b'))
        if th: items.append(item('Grenade distance', 'Grenades in fights are thrown from %.0f-%.0f units (middle half), median %.0f.' % (th[len(th) // 4], th[3 * len(th) // 4], th[len(th) // 2]), len(th)))
        bg = [e for e in G if e['out'].get('b_throws')]; nbg = [e for e in G if not e['out'].get('b_throws')]
        if bg and nbg: items.append(item('Grenade back when attacked', 'Defenders who threw one back won %s; those who did not %s.' % (pc(sum(e['out']['win'] == 'b' for e in bg) / len(bg)), pc(sum(e['out']['win'] == 'b' for e in nbg) / len(nbg))), min(len(bg), len(nbg))))
    else: items.append(item('Grenades', 'not enough data yet: grenade counts are recorded since 12:00 today', 0, weak=True))
    sec('Grenades', items)

    # ---- surviving a specific weapon (the defender's survival: the duel did not end with the defender dead)
    items = []
    DS = [e for e in D if e['out']['win'] in ('a', 'b', 'none', 'a?', 'b?')]
    for e in DS:
        if 'f' not in e: e['f'] = duel_features(e)
    for c, label in (('shotgun', 'a Shotgun or Mauler'), ('sword', 'a Sword or Hammer'), ('sniper', 'a Sniper'), ('precision', 'a Battle Rifle / Carbine'), ('auto', 'an Assault Rifle'), ('rocket', 'rockets')):
        sub = [e for e in DS if wcls(e['ctx']['a_wpn']) == c]
        if len(sub) < MIN: continue
        surv = lambda e: e['out']['win'] not in ('a', 'a?')
        bands = [(0, 6), (6, 12), (12, 20), (20, 45)]; parts = []; best = None
        for lo, hi in bands:
            b = [e for e in sub if lo <= e['ctx']['dist'] < hi]
            if len(b) >= 10:
                r_ = sum(surv(e) for e in b) / len(b); parts.append('%d-%d u %s' % (lo, hi, pc(r_)))
                if best is None or r_ > best[2]: best = (lo, hi, r_)
        st = [e for e in sub if (e['f']['b_strafe'] or 0) > 1.2]; ns = [e for e in sub if (e['f']['b_strafe'] or 0) <= 0.6]
        jm = [e for e in sub if e['f']['b_jumps'] > 0]; gp = [e for e in sub if e['f']['a_gap'] >= 1.2]; ngp = [e for e in sub if 0 < e['f']['a_gap'] < 1.2]
        bits = []
        if parts: bits.append('by distance: ' + ', '.join(parts) + (' -- best at %d-%d units' % (best[0], best[1]) if best else ''))
        if len(st) >= 10 and len(ns) >= 10: bits.append('strafing hard %s vs barely %s' % (pc(sum(surv(e) for e in st) / len(st)), pc(sum(surv(e) for e in ns) / len(ns))))
        if len(jm) >= 10: bits.append('jumping %s' % pc(sum(surv(e) for e in jm) / len(jm)))
        if len(gp) >= 10 and len(ngp) >= 10: bits.append('when it paused firing 1.2+ s (a reload, a lost aim) %s vs %s' % (pc(sum(surv(e) for e in gp) / len(gp)), pc(sum(surv(e) for e in ngp) / len(ngp))))
        items.append(item('Surviving %s' % label, 'Shot at by %s, the defender survives %s overall; %s.' % (label, pc(sum(surv(e) for e in sub) / len(sub)), '; '.join(bits)), len(sub)))
    sec('Surviving a specific weapon', items)

    # ---- maps: the safest and deadliest areas (deaths per 100 passes, 4-unit squares), the fight distance, the best weapon
    items = []
    def read_deaths(mp):
        try: b = open(os.path.join(HM, mp + '.bin'), 'rb').read()
        except OSError: return []
        if b[:4] != b'RHM2': return []
        cell, nc, nd, mm = struct.unpack_from('<fIII', b, 4); o = 20 + 12 * nc; out = []
        for i in range(nd):
            x, y = struct.unpack_from('<ff', b, o); o += 76; out.append((x, y))
        return out
    def read_spawns(mp):
        try: b = open(os.path.join(HM, mp + '.spawns'), 'rb').read()
        except OSError: return []
        if b[:4] != b'RSP1': return []
        n = struct.unpack_from('<I', b, 4)[0]; out = []
        for i in range(n):
            x, y, z, cnt, match, kind = struct.unpack_from('<fffIIB', b, 8 + 64 * i); nm = b[8 + 64 * i + 21:8 + 64 * i + 64].split(b'\0')[0].decode('utf-8', 'replace')
            out.append((x, y, nm))
        return out
    for mp in sorted({r['map'] for r in meta}):
        g, _, _ = li.read_lanes(os.path.join(HM, mp + '.lanes')); dth = read_deaths(mp); sp = read_spawns(mp)
        if not g or len(dth) < 50: continue
        P4, D4 = Counter(), Counter()
        for (x, y), c in g.items(): P4[(int((x + 0.5) * 0.5 // 4), int((y + 0.5) * 0.5 // 4))] += c[0]
        for x, y in dth: D4[(int(x // 4), int(y // 4))] += 1
        cand = [(k, D4[k] / P4[k] * 100, P4[k], D4[k]) for k in P4 if P4[k] >= 150]
        if len(cand) < 6: continue
        cand.sort(key=lambda x: x[1])
        def where(k):
            cx, cy = k[0] * 4 + 2, k[1] * 4 + 2; near = min(sp, key=lambda s: (s[0] - cx) ** 2 + (s[1] - cy) ** 2) if sp else None
            tag = (' near the %s' % wname(near[2])) if near and (near[0] - cx) ** 2 + (near[1] - cy) ** 2 < 100 else ''
            return '(%.0f, %.0f)%s' % (cx, cy, tag)
        safe = ', '.join('%s %.1f' % (where(k), r) for k, r, p, d in cand[:3]); deadly = ', '.join('%s %.1f' % (where(k), r) for k, r, p, d in cand[-3:][::-1])
        items.append(item('%s: where to walk' % mname(mp), 'Busy 4-unit squares with the fewest deaths per 100 crossings: %s. Deadliest: %s. (%d deaths in the heatmap -- every game seen there, Infection included -- over %.0f km of lanes.)' % (
            safe, deadly, len(dth), sum(c[0] for c in g.values()) * 0.5 * 3.048 / 1000), len(dth), mp))
    for mp in sorted({e['map'] for e in D}):
        sub = [e for e in D if e['map'] == mp]
        if len(sub) < 100: continue
        dd = sorted(e['ctx']['dist'] for e in sub)
        items.append(item('%s: fight distance' % mname(mp), 'Fights start at %.0f units (median; middle half %.0f-%.0f).' % (dd[len(dd) // 2], dd[len(dd) // 4], dd[3 * len(dd) // 4]), len(sub), mp))
    sec('Maps', items)

    # ---- weapon zones (shim/tools/weapon-kills.py -> tracks/maps/<map>.wzones.txt: the walker's weapon choice by where the target
    # stands): spots where one weapon class takes far more of the kills than its share of the weapons carried on that map
    items = []
    CLN = {'precision': 'precision rifles (BR / Carbine / Magnum)', 'auto': 'automatics', 'sniper': 'snipers', 'shotgun': 'shotguns', 'rocket': 'rockets / lasers', 'melee': 'swords / hammers'}
    for fz in sorted(glob.glob(os.path.join(TR, 'maps', '*.wzones.txt'))):
        mp = os.path.basename(fz)[:-len('.wzones.txt')]; held = {}; cells = {}
        for ln in open(fz):
            t = ln.split()
            if not t or t[0] == 'wzones': continue
            if t[0] == 'held': held = {k: float(v) for k, v in (x.split('=') for x in t[1:])}; continue
            cells[(int(t[0]), int(t[1]))] = {k: int(v) for k, v in (x.split('=') for x in t[2:])}
        if not cells: continue
        sp = read_spawns(mp); found = []
        for (cx, cy) in cells:
            by = Counter()
            for dx in (-1, 0, 1):
                for dy in (-1, 0, 1): by.update(cells.get((cx + dx, cy + dy), {}))
            kt = sum(by.values())
            if kt < 20: continue
            for c, kc in by.items():
                hs = held.get(c, 0)
                if kc < 8 or hs < 0.003: continue
                e = (kc + 0.5) / (kt + 3) / hs
                if e >= 2.5 and kc / kt >= 0.25: found.append((e, c, cx, cy, kc, kt, hs))
        found.sort(reverse=True); pick = []
        for f in found:
            if all(abs(f[2] - q[2]) > 3 or abs(f[3] - q[3]) > 3 for q in pick) and len(pick) < 3: pick.append(f)
        for e, c, cx, cy, kc, kt, hs in pick:
            x, y = (cx + 0.5) * 6, (cy + 0.5) * 6; near = min(sp, key=lambda s_: (s_[0] - x) ** 2 + (s_[1] - y) ** 2) if sp else None
            tag = (' near the %s' % wname(near[2])) if near and (near[0] - x) ** 2 + (near[1] - y) ** 2 < 225 else ''
            items.append(item('%s: %s own (%.0f, %.0f)%s' % (mname(mp), CLN.get(c, c), x, y, tag), 'they make %s of the %d kills on players standing within ~9 units of it while being %s of the weapons carried on this map (%.1fx): a walker with one switches to it for a target there.' % (pc(kc / kt), kt, pc(hs), e), kt, mp))
    sec('Weapon zones', items)

    # ---- the players' own inventory (recordings with the other-weapon / camo columns, since 2026-11-08 15:32)
    items = []
    SW = [r for r in R if r.get('kind') == 'wswitch' and r['class'] != 'infection']; CM = [r for r in R if r.get('kind') == 'camo' and r['class'] != 'infection']
    if SW:
        pairs = Counter((r['frm_c'], r['to_c']) for r in SW)
        for (a_, b_), n in pairs.most_common(4):
            ds = sorted(r['dist'] for r in SW if (r['frm_c'], r['to_c']) == (a_, b_) and r['dist'] is not None)
            items.append(item('Switching %s -> %s' % (a_, b_), '%d switches; the nearest enemy was %.0f units away (median), %s of them in a fight.' % (n, ds[len(ds) // 2] if ds else 0,
                pc(sum(r['fight'] for r in SW if (r['frm_c'], r['to_c']) == (a_, b_)) / n)), n))
    if CM:
        fd = sorted(r['first_shot_dist'] for r in CM if r['first_shot_dist'] is not None)
        items.append(item('Under active camo', '%d camo stretches (median %.0f s): players fire %s of the time, open fire at %s units (median), get %.2f kills and die in %s of them.' % (len(CM),
            sorted(r['dur'] for r in CM)[len(CM) // 2], pc(sorted(r['fired_share'] for r in CM)[len(CM) // 2]), '%.0f' % fd[len(fd) // 2] if fd else '-',
            sum(r['kills'] for r in CM) / len(CM), pc(sum(r['died'] for r in CM) / len(CM))), len(CM)))
    sec('Inventory: switches and camo', items)

    # ---- radar, revenge, power items (the behaviour records)
    items = []
    C = [r for r in R if r.get('kind') == 'contact' and r['class'] != 'infection']
    if C:
        mv = [c for c in C if c['dist'] < 8.2 and c['speed'] > 1.2]; stl = [c for c in C if c['dist'] < 8.2 and c['speed'] < 0.5]
        if mv and stl: items.append(item('The radar works', 'An unseen enemy within the tracker\'s 25 m that MOVES gets looked at %s of the time within 2.5 s, a still one %s -- crouch or stand still near enemies to stay off their radar.' % (
            pc(sum(c['turned'] is not None for c in mv) / len(mv)), pc(sum(c['turned'] is not None for c in stl) / len(stl))), min(len(mv), len(stl))))
    RS = [r for r in R if r.get('kind') == 'respawn' and r['class'] != 'infection']
    if RS:
        nt = [r for r in RS if r['next_target']]
        if nt: items.append(item('Revenge?', 'After a respawn the next target is the killer %s of the time (chance %s): players do not hunt their killer in this data.' % (pc(sum(r['next_target'] == 'killer' for r in nt) / len(nt)), pc(sum(1 / max(1, r['enemies']) for r in nt) / len(nt))), len(nt)))
    IT = [r for r in R if r.get('kind') == 'item' and r['class'] != 'infection']
    if IT:
        f2 = [i for i in IT if i['first_s'] is not None]
        items.append(item('Power items go fast', 'A power weapon or power-up is reached in %s of cases, median %.1f s after it appears; the nearest player gets there first only %s of the time -- the ones who take it were already on their way.' % (
            pc(len(f2) / len(IT)), sorted(i['first_s'] for i in f2)[len(f2) // 2] if f2 else 0, pc(sum(bool(i['first_was_nearest']) for i in f2) / max(1, len(f2)))), len(IT)))
    sec('Radar, revenge, power items', items)

    out = {'updated': now, 'source': src, 'min_cases': MIN, 'sections': S}
    tmp = os.path.join(TR, 'learnings.json.tmp'); json.dump(out, open(tmp, 'w'), indent=1); os.replace(tmp, os.path.join(TR, 'learnings.json'))
    if not QUIET:
        print('What the game is learning -- %d recordings, %d min, %d duels (%d decided), %d maps' % (src['recordings'], src['minutes'], src['duels'], src['decided'], src['maps']))
        for s in S:
            print('\n== %s' % s['topic'])
            for i in s['items']: print('  %s%s: %s  [n=%d]' % ('(weak) ' if i['weak'] else '', i['title'], i['text'], i['n']))

if __name__ == '__main__':
    main()
