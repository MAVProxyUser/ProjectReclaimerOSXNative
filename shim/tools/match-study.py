#!/usr/bin/env python3
"""Study one match of walkers against a human (2026-11-08; the user, after beating two walkers 17-4: "the bots should study that ass
whipping I just gave them! there are some things I notice they fail at. meles and just generally shooting first are two of em. They
do well though. some times they stand around. coach up"). From the match's recording (match-recorder.py: every player 10 times a
second -- position, look, shield, health, weapon, kills, deaths, SHOTS and HITS counters -- and the kill events):
  kills        each kill: killer, victim, how, distance at the victim's last live sample, the victim's shield 0.5 s before
  fights       an encounter = two enemies both alive within 40 units while at least one fires (shots counter going up), ended by
               5 s without a shot or a death. Per encounter: who fired FIRST, how long the other took to fire back (the reply),
               who hit first, who died. Summed per player: first shots, replies (median), encounters won (the other died first)
  accuracy     hits / shots per player (the server's counters)
  idle         a player alive and moving under 0.3 units/s for 2 s or more: the spells, their total and where (6-unit cells)
  melee        kills within 1.5 units (our server names melee ""; public servers "Melee")
Writes <recording>.study.json beside it and prints the report.
    python3 shim/tools/match-study.py <recording.jsonl.gz> [--human SLOT]   (default: the player with the most kills)"""
import bisect, collections, gzip, json, math, os, sys

def read(path):
    head = {}; S = []; E = []
    try:
        with gzip.open(path, 'rt') as f:
            for l in f:
                try: o = json.loads(l)
                except Exception: break
                if 'start' in o and not head: head = o
                elif 'p' in o: S.append(o)
                elif o.get('ev') == 'kill': E.append(o)
    except (EOFError, OSError): pass   # (a recording still being written: what is there so far)
    return head, S, E

def d3(a, b): return math.sqrt((a[3] - b[3]) ** 2 + (a[4] - b[4]) ** 2 + (a[5] - b[5]) ** 2)
def med(v):
    v = sorted(v); return v[len(v) // 2] if v else None

def main():
    path = [a for a in sys.argv[1:] if not a.startswith('--')][0]
    head, S, E = read(path); T = [s['t'] for s in S]
    rows = lambda s: {r[0]: r for r in s['p']}
    last = rows(S[-1]); slots = sorted(last)
    human = int(sys.argv[sys.argv.index('--human') + 1]) if '--human' in sys.argv else max(slots, key=lambda k: last[k][12])
    name = lambda k: 'you' if k == human else 'walker %d' % k
    out = {'recording': os.path.basename(path), 'human': human, 'seconds': round(T[-1] - T[0], 1)}
    # scoreboard
    sb = {k: {'kills': last[k][12], 'deaths': last[k][13], 'shots': last[k][14], 'hits': last[k][15]} for k in slots}
    out['scoreboard'] = {name(k): v for k, v in sb.items()}
    # kills
    K = []
    for e in E:
        a, b = e.get('a'), e.get('b')
        if a is None or b is None: continue
        i = bisect.bisect_right(T, e['t'] + 1e-6) - 1; ra = rb = None; tk = e['t']
        for j in range(i, max(-1, i - 41), -1):
            R = rows(S[j])
            if b in R and R[b][2]: rb = R[b]; ra = R.get(a); tk = T[j]; break
        sh = None
        j5 = bisect.bisect_right(T, tk - 0.5) - 1
        if j5 >= 0 and b in rows(S[j5]): sh = rows(S[j5])[b][8]
        K.append({'t': round(tk - T[0], 1), 'killer': name(a) if a != b else 'self', 'victim': name(b), 'how': e.get('how') or '(unnamed)',
                  'distance': round(d3(ra, rb), 2) if ra and rb and ra[2] else None, 'killer_weapon': ra[10] if ra else '', 'victim_shield_before': sh})
    out['kills'] = K
    # encounters
    enc = []; cur = {}
    prev = None
    for s in S:
        R = rows(s)
        if prev is not None:
            for x in slots:
                for y in slots:
                    if y <= x or x not in R or y not in R: continue
                    rx, ry = R[x], R[y]; px, py = prev.get(x), prev.get(y)
                    if not (rx[2] and ry[2]) or d3(rx, ry) > 40:
                        if (x, y) in cur:
                            c = cur.pop((x, y))
                            for k in (x, y):   # (a death ends it: who died)
                                if k in R and not R[k][2] and px is not None and k in prev and prev[k][2]: c['died'] = k
                            enc.append(c)
                        continue
                    fx = px is not None and rx[14] > px[14]; fy = py is not None and ry[14] > py[14]
                    hx = px is not None and rx[15] > px[15]; hy = py is not None and ry[15] > py[15]
                    c = cur.get((x, y))
                    if c and s['t'] - c['last'] > 5.0: enc.append(cur.pop((x, y))); c = None
                    if not c and (fx or fy): c = cur[(x, y)] = {'a': x, 'b': y, 't0': s['t'], 'last': s['t'], 'first': {}, 'hit': {}, 'dist0': d3(rx, ry)}
                    if c:
                        for k, f, h in ((x, fx, hx), (y, fy, hy)):
                            if f: c['first'].setdefault(k, s['t']); c['last'] = s['t']
                            if h: c['hit'].setdefault(k, s['t'])
            # deaths end encounters
            for (x, y), c in list(cur.items()):
                for k in (x, y):
                    if k in R and not R[k][2] and prev.get(k) and prev[k][2]: c['died'] = k; enc.append(cur.pop((x, y))); break
        prev = R
    enc += list(cur.values())
    P = {k: {'encounters': 0, 'fired_first': 0, 'hit_first': 0, 'replies': [], 'won': 0, 'lost': 0} for k in slots}
    for c in enc:
        if not c['first']: continue
        ks = (c['a'], c['b']); f = min(c['first'], key=c['first'].get)
        for k in ks:
            P[k]['encounters'] += 1
            o = ks[1] if k == ks[0] else ks[0]
            if f == k: P[k]['fired_first'] += 1
            elif k in c['first']: P[k]['replies'].append(c['first'][k] - c['first'][o])
            if c['hit'] and min(c['hit'], key=c['hit'].get) == k: P[k]['hit_first'] += 1
            if c.get('died') == o: P[k]['won'] += 1
            elif c.get('died') == k: P[k]['lost'] += 1
    out['fights'] = {name(k): {'encounters': v['encounters'], 'fired_first': v['fired_first'], 'hit_first': v['hit_first'], 'won': v['won'], 'lost': v['lost'],
                               'reply_median_s': round(med(v['replies']), 2) if v['replies'] else None, 'never_replied': v['encounters'] - v['fired_first'] - len(v['replies'])}
                     for k, v in P.items()}
    # idle
    I = {}
    for k in slots:
        spells = []; st = None; alive_t = 0.0
        for i in range(1, len(S)):
            R0, R1 = rows(S[i - 1]), rows(S[i])
            if k not in R0 or k not in R1 or not (R0[k][2] and R1[k][2]): st = None; continue
            dt = S[i]['t'] - S[i - 1]['t']; alive_t += dt
            v = math.hypot(R1[k][3] - R0[k][3], R1[k][4] - R0[k][4]) / max(dt, 1e-3)
            if v < 0.3:
                if st is None: st = (S[i - 1]['t'], R0[k][3], R0[k][4], R0[k][10])
            else:
                if st and S[i]['t'] - st[0] >= 2.0: spells.append((round(st[0] - T[0], 1), round(S[i]['t'] - st[0], 1), round(st[1], 1), round(st[2], 1), st[3]))
                st = None
        cells = collections.Counter((round(x / 6) * 6, round(y / 6) * 6) for _, _, x, y, w in spells)
        I[name(k)] = {'spells': len(spells), 'seconds': round(sum(s[1] for s in spells), 1), 'share_of_alive': round(sum(s[1] for s in spells) / max(alive_t, 1), 3),
                      'longest': sorted(spells, key=lambda s: -s[1])[:5], 'places': cells.most_common(5),
                      'weapon_seconds': dict(collections.Counter({w: 0 for _, _, _, _, w in spells}) + collections.Counter(dict((w, round(sum(sp[1] for sp in spells if sp[4] == w), 1)) for _, _, _, _, w in spells)))}
    out['idle'] = I
    out['melee_kills'] = [k for k in K if k['distance'] is not None and k['distance'] <= 1.5]
    # LEVELS APART (the user: "enemy radar on multi level maps ... above or below you. the engagement struggled"): a player and an
    # enemy within 12 units across the floor but more than 2 units apart in height, both alive, for 2 s or more. Per episode: the
    # player's speed, whether the height gap closed under 1.2 units (it reached the other's level) or the other came down / up,
    # and who killed whom within 8 s of its end.
    V = {k: [] for k in slots}; open_ = {}
    for i, s0 in enumerate(S):
        R = rows(s0)
        for x in slots:
            for y in slots:
                if x == y or x not in R or y not in R: continue
                rx, ry = R[x], R[y]; key = (x, y)
                cond = rx[2] and ry[2] and math.hypot(rx[3] - ry[3], rx[4] - ry[4]) < 12 and abs(rx[5] - ry[5]) > 2.0
                if cond and key not in open_: open_[key] = {'i0': i, 'z0': rx[5] - ry[5], 'path': 0.0, 'px': (rx[3], rx[4])}
                elif cond: e = open_[key]; e['path'] += math.hypot(rx[3] - e['px'][0], rx[4] - e['px'][1]); e['px'] = (rx[3], rx[4])
                elif key in open_:
                    e = open_.pop(key); dur = s0['t'] - S[e['i0']]['t']
                    if dur < 2.0: continue
                    met = rx[2] and ry[2] and abs(rx[5] - ry[5]) < 1.2
                    t1 = s0['t']; res = None
                    for k2 in K:
                        tk = k2['t'] + T[0]
                        if t1 - 1 <= tk <= t1 + 8 and {k2['killer'], k2['victim']} == {name(x), name(y)}: res = 'won' if k2['killer'] == name(x) else 'lost'; break
                    V[x].append({'dur': round(dur, 1), 'speed': round(e['path'] / dur, 2), 'above': e['z0'] > 0, 'met': bool(met), 'result': res})
    out['levels_apart'] = {name(k): {'episodes': len(v), 'median_speed': med([e['speed'] for e in v]), 'met_on_one_level': sum(e['met'] for e in v),
                                    'won': sum(e['result'] == 'won' for e in v), 'lost': sum(e['result'] == 'lost' for e in v),
                                    'won_from_above': sum(e['result'] == 'won' and e['above'] for e in v), 'won_from_below': sum(e['result'] == 'won' and not e['above'] for e in v)}
                           for k, v in V.items()}
    json.dump(out, open(path.replace('.jsonl.gz', '') + '.study.json', 'w'), indent=1)
    print('%s: %.0f s; you = slot %d' % (out['recording'], out['seconds'], human))
    for n, v in out['scoreboard'].items(): print('  %-9s %2d kills %2d deaths, accuracy %s' % (n, v['kills'], v['deaths'], '%.0f%%' % (100 * v['hits'] / v['shots']) if v['shots'] else '-'))
    print('fights (two enemies within 40 units, someone firing):')
    for n, v in out['fights'].items(): print('  %-9s %3d fights: fired first %3d, hit first %3d, won %2d, lost %2d; reply when shot at first: median %s s, never fired back %d' % (
        n, v['encounters'], v['fired_first'], v['hit_first'], v['won'], v['lost'], v['reply_median_s'], v['never_replied']))
    print('kills by type: %s' % dict(collections.Counter(k['how'] for k in K)))
    for k in K: print('  %6.1f s %-9s -> %-9s %-14s %5s units, victim shield %s, killer holding %s' % (k['t'], k['killer'], k['victim'], k['how'], k['distance'], k['victim_shield_before'], k['killer_weapon']))
    print('standing still while alive (under 0.3 units/s for 2 s+):')
    for n, v in out['idle'].items(): print('  %-9s %2d spells, %5.1f s (%.0f%% of alive time); longest %s; places %s' % (n, v['spells'], v['seconds'], 100 * v['share_of_alive'], v['longest'][:3], v['places'][:3]))
    print('weapon held while standing still (seconds):')
    for n, v in out['idle'].items(): print('  %-9s %s' % (n, v['weapon_seconds']))
    print('levels apart (an enemy within 12 units across, over 2 units above / below, 2 s+): episodes, speed, met on one level, then won / lost within 8 s:')
    for n, v in out['levels_apart'].items(): print('  %-9s %3d episodes, median speed %s u/s, met on one level %d, won %d (from above %d, below %d), lost %d' % (
        n, v['episodes'], v['median_speed'], v['met_on_one_level'], v['won'], v['won_from_above'], v['won_from_below'], v['lost']))
    print('melee-range kills (within 1.5 units): %d -- %s' % (len(out['melee_kills']), ', '.join('%s on %s' % (k['killer'], k['victim']) for k in out['melee_kills'])))
    return 0

if __name__ == '__main__':
    sys.exit(main())
