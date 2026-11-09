#!/usr/bin/env python3
"""How random are the spawns? (2026-11-08; the user: "pay attention to the randomness of the spawn patterns too in your learning
process ... patterns in either would be interesting"). From every recorded public match (tracks/*.jsonl.gz, players' positions 10
times a second): each RESPAWN (a player alive again after a death) is matched to the map's spawn points (all respawn positions seen
on that map, merged within 2 units), and the point the game chose is compared with every other point of the map at that moment:
  nearest enemy      distance from the point to the nearest live enemy (Halo 3 is said to avoid enemies)
  enemies near       live enemies within 15 units
  nearest teammate   (team games) distance to the nearest live teammate
  own death          distance to where this player died
  just used          the point was a spawn in the last 10 s (anyone)
A conditional logit over the points gives each one's weight, and its PREDICTABILITY: how often the chosen point is the model's
first / among its first three, against chance (1 / points, 3 / points); plus how often the game chose the point FARTHEST from
enemies, the same point as the player's previous spawn, and the spread of the choice (entropy against uniform).
Per game class (free-for-all / team), Infection left out. Writes tracks/spawns.json, prints the report.
    python3 shim/tools/spawn-patterns.py [--quiet]"""
import glob, gzip, json, math, os, sys
from collections import defaultdict
import numpy as np

TR = os.path.expanduser('~/Library/Application Support/ReclaimerMac/tracks'); QUIET = '--quiet' in sys.argv
INF = ('infection', 'zombie', 'flood')

def read(path):
    head = {}; S = []
    try:
        with gzip.open(path, 'rt') as f:
            for l in f:
                try: o = json.loads(l)
                except Exception: break
                if 'start' in o and not head: head = o
                elif 'p' in o: S.append(o)
    except (EOFError, OSError): pass
    return head, S

def spawns_of(head, S):
    """respawns: (t, slot, team, pos, death pos, others [(team, pos)], team_game)"""
    teams = defaultdict(set)
    for s in S[::50]:
        for r in s['p']:
            if r[1] is not None and r[1] >= 0: teams[r[1]].add(r[0])
    team_game = sum(1 for v in teams.values() if len(v) >= 2) >= 2
    alive = {}; last = {}; death = {}; out = []
    for s in S:
        cur = {}
        for r in s['p']:
            a = bool(r[2]) and bool(r[3] or r[4]); cur[r[0]] = (a, r[1], (r[3], r[4], r[5]))
        for sl, (a, tm, pos) in cur.items():
            was = alive.get(sl)
            if was is True and not a and sl in last: death[sl] = last[sl]
            if was is False and a and sl in death:
                others = [(t2, p2) for s2, (a2, t2, p2) in cur.items() if s2 != sl and a2]
                out.append((s['t'], sl, tm, pos, death.pop(sl), others, team_game))
            alive[sl] = a
            if a: last[sl] = pos
    return out, team_game

def d3(a, b): return math.sqrt((a[0] - b[0]) ** 2 + (a[1] - b[1]) ** 2 + (a[2] - b[2]) ** 2)

def fit(X, y, mask, iters=400, lr=0.5):
    """conditional logit: X (N, K, F), chosen index y (N), mask (N, K) -> weights (F)"""
    N, K, F = X.shape; w = np.zeros(F); m = mask.astype(float)
    for _ in range(iters):
        s = (X @ w) + np.where(mask, 0, -1e9); s -= s.max(1, keepdims=True); p = np.exp(s) * m; p /= p.sum(1, keepdims=True)
        g = X[np.arange(N), y] - (p[:, :, None] * X).sum(1); w += lr * g.mean(0)
    s = (X @ w) + np.where(mask, 0, -1e9); return w, s

def main():
    files = sorted(p for p in glob.glob(os.path.join(TR, '*.jsonl.gz')) if not p.endswith('_local.jsonl.gz'))
    by_map = defaultdict(list)
    for p in files:
        head, S = read(p)
        mode = ' '.join(str(head.get(k) or '') for k in ('engine', 'variant', 'mode')).lower()
        if len(S) < 300 or any(k in mode for k in INF): continue
        sp, tg = spawns_of(head, S)
        for e in sp: by_map[(head.get('map'), tg)].append(e)
    FEAT = ['log nearest enemy', 'enemies within 15', 'log nearest teammate', 'log own death', 'just used (10 s)']
    rep = {'feature_names': FEAT, 'maps': {}, 'classes': {}}
    pooled = {False: [], True: []}
    for (mp, tg), E in sorted(by_map.items(), key=lambda kv: -len(kv[1])):
        if len(E) < 40: continue
        # the map's spawn points: respawn positions merged within 2 units (a point seen once is dropped as noise)
        pts = []
        for e in E:
            p = e[3]
            for q in pts:
                if d3(q[0], p) < 2.0: q[1] += 1; break
            else: pts.append([p, 1])
        pts = [q[0] for q in pts if q[1] >= 2]
        if len(pts) < 4: continue
        P = np.array(pts); used = []   # (t, point index)
        rows = []
        for (t, sl, tm, pos, dp, others, team_game) in E:
            ci = int(np.argmin(np.linalg.norm(P - np.array(pos), axis=1)))
            if np.linalg.norm(P[ci] - np.array(pos)) > 2.5: continue
            en = [np.array(p2) for t2, p2 in others if not team_game or t2 != tm]; mt = [np.array(p2) for t2, p2 in others if team_game and t2 == tm]
            de = np.min(np.linalg.norm(P[:, None, :] - np.array(en)[None], axis=2), axis=1) if en else np.full(len(P), 60.0)
            ne = (np.linalg.norm(P[:, None, :] - np.array(en)[None], axis=2) < 15).sum(1) if en else np.zeros(len(P))
            dm = np.min(np.linalg.norm(P[:, None, :] - np.array(mt)[None], axis=2), axis=1) if mt else np.full(len(P), 60.0)
            dd = np.linalg.norm(P - np.array(dp), axis=1)
            ju = np.zeros(len(P)); [ju.__setitem__(j, 1.0) for (t0, j) in used if t - t0 < 10]
            rows.append((np.stack([np.log1p(de), ne, np.log1p(dm) if team_game else np.zeros(len(P)), np.log1p(dd), ju], 1), ci, de, sl))
            used.append((t, ci)); used = [u for u in used if t - u[0] < 30]
        if len(rows) < 30: continue
        K = len(P); X = np.stack([r[0] for r in rows]); y = np.array([r[1] for r in rows]); mask = np.ones((len(rows), K), bool)
        w, s = fit(X, y, mask); rank = (s > s[np.arange(len(y)), y][:, None]).sum(1)
        far = np.mean([int(np.argmax(r[2]) == r[1]) for r in rows])
        # the same point as this player's previous respawn on this map
        prev = {}; same = []
        for r in rows:
            if r[3] in prev: same.append(int(prev[r[3]] == r[1]))
            prev[r[3]] = r[1]
        cnt = np.bincount(y, minlength=K) / len(y); ent = float(-(cnt[cnt > 0] * np.log(cnt[cnt > 0])).sum() / math.log(K))
        rep['maps']['%s %s' % (mp, 'team' if tg else 'ffa')] = {'spawns': len(rows), 'points': K, 'weights': dict(zip(FEAT, np.round(w, 3).tolist())),
            'top1': round(float((rank == 0).mean()), 3), 'top3': round(float((rank < 3).mean()), 3), 'chance_top1': round(1 / K, 3), 'chance_top3': round(min(1, 3 / K), 3),
            'farthest_from_enemies': round(float(far), 3), 'same_as_previous': round(float(np.mean(same)), 3) if same else None, 'chance_same': round(float((cnt ** 2).sum()), 3),
            'entropy_vs_uniform': round(ent, 3)}
        pooled[tg].append((X, y, K))
    for tg, L in pooled.items():
        if not L: continue
        Kmax = max(k for _, _, k in L); Xs = []; ys = []; ms = []
        for X, y, K in L:
            pad = np.zeros((X.shape[0], Kmax, X.shape[2])); pad[:, :K] = X; m = np.zeros((X.shape[0], Kmax), bool); m[:, :K] = True; Xs.append(pad); ys.append(y); ms.append(m)
        X = np.concatenate(Xs); y = np.concatenate(ys); m = np.concatenate(ms); w, s = fit(X, y, m)
        rank = (np.where(m, s, -1e18) > s[np.arange(len(y)), y][:, None]).sum(1); Ks = m.sum(1)
        rep['classes']['team' if tg else 'ffa'] = {'spawns': int(len(y)), 'weights': dict(zip(FEAT, np.round(w, 3).tolist())), 'top1': round(float((rank == 0).mean()), 3),
            'top3': round(float((rank < 3).mean()), 3), 'chance_top1': round(float((1 / Ks).mean()), 3), 'chance_top3': round(float(np.minimum(1, 3 / Ks).mean()), 3)}
    json.dump(rep, open(os.path.join(TR, 'spawns.json'), 'w'), indent=1)
    if not QUIET:
        for c, v in rep['classes'].items():
            print('== %s games: %d respawns; the model names the chosen point first %s of the time (chance %s), in its first three %s (chance %s)' % (c, v['spawns'], v['top1'], v['chance_top1'], v['top3'], v['chance_top3']))
            print('   weights: ' + ', '.join('%s %+.2f' % kv for kv in v['weights'].items()))
        for k, v in sorted(rep['maps'].items(), key=lambda kv: -kv[1]['spawns']):
            print('  %-24s %4d respawns over %2d points: first %.0f%% (chance %.0f%%), top 3 %.0f%% (%.0f%%); farthest from enemies %.0f%%; same as previous %s (chance %.0f%%); spread %.2f of uniform' % (
                k, v['spawns'], v['points'], 100 * v['top1'], 100 * v['chance_top1'], 100 * v['top3'], 100 * v['chance_top3'], 100 * v['farthest_from_enemies'],
                '-' if v['same_as_previous'] is None else '%.0f%%' % (100 * v['same_as_previous']), 100 * v['chance_same'], v['entropy_vs_uniform']))
            print('       ' + ', '.join('%s %+.2f' % kv for kv in v['weights'].items()))
    return 0

if __name__ == '__main__':
    sys.exit(main())
