#!/usr/bin/env python3
"""The engagement library (2026-11-08; the user: "sample a bunch of games so we have a good dataset on how people *actually* play
when engaging with someone else"). Cuts every recorded match (shim/tools/match-recorder.py, ReclaimerMac/tracks/*.jsonl.gz) into
DUELS: two enemies, both alive, within 45 units, at least one ATTACKING the other -- aim within 8 deg (horizontal: a remote
player's pitch is often not sent) while its shots counter goes up, or within 12 deg while the other's shield / health drops --
merged across pauses under 2 s, at least 0.3 s of attack. Each duel runs from the AWARENESS before it (back up to 3 s while
either one looks within 30 deg of the other) to its end (+0.5 s), at most 30 s, and keeps:
  ctx      map, mode, class (slayer / ctf / infection / other), team game or not, both weapons and vehicles, distance, height
           difference, shields / health, who attacked first ("a" is always the one who did), each one's aim error at the start,
           teammates (alive, within 15 units) and other enemies (within 30) around each
  rows     every 0.1 s: [t, dist, a_lat, a_rad, b_lat, b_rad, a_err, b_err, a_sh, a_hp, b_sh, b_hp, a_fire, b_fire, a_vz, b_vz, dz]
           -- lat / rad = each one's speed across / toward the other (rad > 0 = closing in; lat > 0 = to its left), err = its
           horizontal aim error to the other (deg), fire = its shots counter went up since the last row, dz = b's height - a's
  out      who won ("a" / "b" from a kill event between them, "a?" / "b?" when the other only died with no event, "third"
           when one was killed by someone else, "none" = they separated), the weapon, duration, damage each took, retreats
MULTI-PLAYER (the user: "players will inevitably be engaged from multiple other players ... they may get shot by someone else
while fighting 1:1 and change focus ... there is a whole art to the tactics"): every player's TARGET at every sample (the enemy
it attacks; the nearest to its aim when it attacks several) held across pauses under 1 s, and four more kinds of record:
  choice       each time a player takes a new target with at least two enemies within 45 units: every candidate's features half
               a second before (log distance, how far off the aim it was, whether it had attacked this player in the last 1.5 s,
               its shield + health, whether it was this player's previous target, whether a teammate attacked it, whether it
               was just hurt, the height difference) and which one was chosen -- engage-model.py fits the players' choice rule
  switch       a player fighting B is attacked by another enemy C: did it turn to C (and how fast), with B's / C's / its own
               state and distances, and whether it died in the next 5 s
  pressure     stretches when 2+ enemies attacked a player within 1.5 s (outnumbered) or exactly one (1v1, for comparison):
               length, retreat (moving away from the attackers), its kills, whether it died
  join         team games: a teammate starts attacking an enemy near this player (25 units) while this player has no target:
               did it join on the same enemy within 3 s
  skirmish     every 3 s: the groups of players linked by attacks in those 3 s (sizes per side)
Infection: every player has a team of his own there, so the sides are guessed from weapons (a zombie carries the Energy Blade).
Output: ReclaimerMac/tracks/engagements/<recording>.eng.jsonl.gz (one JSON per duel), made again only when the recording is
newer. The walkers' battles (ReclaimerMac/battles/tracks) can be cut the same way with --tracks / --out.
    python3 shim/tools/engage-extract.py [--tracks DIR] [--out DIR] [--force] [--quiet]"""
import glob, gzip, json, math, os, sys, time
from collections import Counter
import numpy as np

SUP = os.path.expanduser('~/Library/Application Support/ReclaimerMac')
def arg(k, d):
    a = sys.argv; return a[a.index(k) + 1] if k in a else d
TRACKS = arg('--tracks', os.path.join(SUP, 'tracks')); OUT = arg('--out', os.path.join(TRACKS, 'engagements'))
FORCE = '--force' in sys.argv; QUIET = '--quiet' in sys.argv
R_MAX, AIM_FIRE, AIM_DMG, AWARE, GAP, MIN_ATT, PRE, POST, MAXLEN = 45.0, 8.0, 12.0, 30.0, 2.0, 0.3, 3.0, 0.5, 30.0
ZOMBIE = ('ENERGY BLADE',)

def mode_class(mode, variant):
    t = ((mode or '') + ' ' + (variant or '')).lower()
    if any(k in t for k in ('infection', 'zombie', 'flood', 'braaa')): return 'infection'
    if any(k in t for k in ('ctf', 'capture the flag', 'flag')): return 'ctf'
    if any(k in t for k in ('slayer', 'swat', 'snipe', 'br starts', 'fiesta', 'ffa', 'free for all', 'rumble', 'doubles', 'beams', 'kills', 'mlg', 'team br')): return 'slayer'
    return 'other'

def load(path):
    head, S, E = None, [], []
    try:
        with gzip.open(path, 'rt') as f:
            while True:
                try: l = f.readline()
                except (EOFError, OSError): break
                if not l: break
                try: o = json.loads(l)
                except Exception: break
                if 'start' in o: head = o
                elif 'p' in o: S.append(o)
                elif 'ev' in o: E.append(o)
    except (EOFError, OSError): pass
    return head, S, E

def wrap(a): return (a + 180.0) % 360.0 - 180.0

def cut(path):
    head, S, E = load(path)
    if not head or len(S) < 100: return []
    slots = sorted({r[0] for s in S for r in s['p'] if isinstance(r[0], int) and 0 <= r[0] < 256})
    if not slots: return []
    ix = {sl: k for k, sl in enumerate(slots)}; T, N = len(S), len(slots)
    t = np.array([s['t'] for s in S], dtype=np.float64)
    alive = np.zeros((T, N), bool); X = np.zeros((T, N)); Y = np.zeros((T, N)); Z = np.zeros((T, N)); YAW = np.zeros((T, N))
    SH = np.zeros((T, N)); HP = np.zeros((T, N)); SHOTS = np.zeros((T, N)); TEAM = np.full((T, N), -1, int); VEH = np.zeros((T, N), bool)
    WPN = np.full((T, N), '', dtype=object); VNAME = np.full((T, N), '', dtype=object); you = np.full(T, -1, int); GREN = np.full((T, N), -1.0)
    for k, s in enumerate(S):
        you[k] = s.get('you', -1)
        for r in s['p']:
            if not isinstance(r[0], int) or r[0] not in ix: continue
            c = ix[r[0]]; alive[k, c] = bool(r[2]) and (r[3] or r[4]); X[k, c], Y[k, c], Z[k, c] = r[3], r[4], r[5]; YAW[k, c] = r[6]
            SH[k, c], HP[k, c], TEAM[k, c] = r[8], r[9], r[1]; WPN[k, c] = r[10] or ''; VNAME[k, c] = r[11] or ''; VEH[k, c] = bool(r[11])
            SHOTS[k, c] = r[14] if len(r) > 14 else 0
            if len(r) > 16: GREN[k, c] = r[16]
    me = np.bincount(you[you >= 0]).argmax() if (you >= 0).any() else -1   # the observer (left out: it only stands there)
    cls = mode_class(head.get('mode'), head.get('variant')); inf = cls == 'infection'
    # velocities: +-2 samples, inside alive stretches, < 12 u/s on foot (a respawn / teleport otherwise)
    VX = np.full((T, N), np.nan); VY = np.full((T, N), np.nan); VZ = np.full((T, N), np.nan)
    if T > 5:
        dt = (t[4:] - t[:-4])[:, None]; ok = alive[4:] & alive[:-4] & alive[2:-2] & (dt > 0.1) & (dt < 0.8)
        vx = (X[4:] - X[:-4]) / np.where(dt > 0, dt, 1); vy = (Y[4:] - Y[:-4]) / np.where(dt > 0, dt, 1); vz = (Z[4:] - Z[:-4]) / np.where(dt > 0, dt, 1)
        ok &= np.hypot(vx, vy) < 12
        VX[2:-2] = np.where(ok, vx, np.nan); VY[2:-2] = np.where(ok, vy, np.nan); VZ[2:-2] = np.where(ok, vz, np.nan)
    # fired: shots counter up over the last 0.5 s; hurt: shield + health down over the last 0.3 s
    FIRE = np.zeros((T, N), bool); FIRE[5:] = (SHOTS[5:] > SHOTS[:-5]) & alive[5:] & alive[:-5]
    FIRE1 = np.zeros((T, N), bool); FIRE1[1:] = (SHOTS[1:] > SHOTS[:-1]) & alive[1:]
    THROW = np.zeros((T, N), bool); THROW[1:] = (GREN[1:] >= 0) & (GREN[:-1] > GREN[1:]) & alive[1:] & alive[:-1]   # (grenades carried went down: a throw)
    HURT = np.zeros((T, N), bool); HURT[3:] = ((SH[3:] + HP[3:]) < (SH[:-3] + HP[:-3]) - 0.02) & alive[3:] & alive[:-3]
    ZOMB = np.isin(WPN, ZOMBIE)
    kills = [(e['t'], e.get('a', -1), e.get('b', -1), e.get('how') or '') for e in E if e.get('ev') == 'kill' and e.get('verb') == 'killed']
    deaths_any = [(e['t'], e.get('b', -1)) for e in E if e.get('ev') in ('kill', 'death')]
    out = []
    for a_ in range(N):
        if slots[a_] == me: continue
        for b_ in range(a_ + 1, N):
            if slots[b_] == me: continue
            both = alive[:, a_] & alive[:, b_]
            if not both.any(): continue
            if inf: enemy = ZOMB[:, a_] != ZOMB[:, b_]
            else: enemy = TEAM[:, a_] != TEAM[:, b_]
            dx, dy = X[:, b_] - X[:, a_], Y[:, b_] - Y[:, a_]; dist = np.sqrt(dx * dx + dy * dy + (Z[:, b_] - Z[:, a_]) ** 2)
            bear = np.degrees(np.arctan2(dy, dx)); eab = np.abs(wrap(YAW[:, a_] - bear)); eba = np.abs(wrap(YAW[:, b_] - (bear + 180.0)))
            base = both & enemy & (dist < R_MAX) & (dist > 0.4)
            att_a = base & (((eab < AIM_FIRE) & FIRE[:, a_]) | ((eab < AIM_DMG) & HURT[:, b_]))
            att_b = base & (((eba < AIM_FIRE) & FIRE[:, b_]) | ((eba < AIM_DMG) & HURT[:, a_]))
            att = att_a | att_b
            if not att.any(): continue
            idx = np.nonzero(att)[0]
            # episodes: attack samples merged across gaps < GAP seconds
            segs = []; s0 = idx[0]; prev = idx[0]
            for k in idx[1:]:
                if t[k] - t[prev] > GAP: segs.append((s0, prev)); s0 = k
                prev = k
            segs.append((s0, prev))
            for s0, s1 in segs:
                if (att[s0:s1 + 1].sum()) * 0.1 < MIN_ATT: continue
                # who attacked first = "a"
                fa = np.nonzero(att_a[s0:s1 + 1])[0]; fb = np.nonzero(att_b[s0:s1 + 1])[0]
                first_a = len(fa) and (not len(fb) or fa[0] <= fb[0]); A, B = (a_, b_) if first_a else (b_, a_)
                eA, eB = (eab, eba) if first_a else (eba, eab)
                # awareness: back up to PRE s while either looks within AWARE deg and both alive and within range
                k0 = s0
                while k0 > 0 and t[s0] - t[k0 - 1] <= PRE and both[k0 - 1] and dist[k0 - 1] < R_MAX and min(eA[k0 - 1], eB[k0 - 1]) < AWARE: k0 -= 1
                k1 = s1
                while k1 + 1 < T and t[k1 + 1] - t[s1] <= POST: k1 += 1
                if t[k1] - t[k0] > MAXLEN: k1 = int(np.searchsorted(t, t[k0] + MAXLEN))
                k1 = min(k1, T - 1)
                # the outcome: a kill event between them in [start, end + 2.5 s]; else a death of either; else separated
                ta, tb = t[k0], t[s1] + 2.5; win = 'none'; how = ''
                for (kt, ka, kb, kh) in kills:
                    if ta <= kt <= tb and {ka, kb} == {slots[A], slots[B]}: win = 'a' if ka == slots[A] else 'b'; how = kh; break
                if win == 'none':
                    for (kt, ka, kb, kh) in kills:
                        if ta <= kt <= tb and kb in (slots[A], slots[B]): win = 'third'; how = kh; break
                if win == 'none':
                    da = (~alive[s1:min(T, s1 + 25), A]).any(); db = (~alive[s1:min(T, s1 + 25), B]).any()
                    if da != db: win = 'b?' if da else 'a?'
                if win == 'none' and any(ta <= dt_ <= tb and db_ in (slots[A], slots[B]) for dt_, db_ in deaths_any): win = 'third'
                ux, uy = dx / np.maximum(np.hypot(dx, dy), 1e-3), dy / np.maximum(np.hypot(dx, dy), 1e-3)
                if not first_a: ux, uy = -ux, -uy   # (u points from A to B)
                rows = []
                for k in range(k0, k1 + 1):
                    if not (alive[k, A] or alive[k, B]): break
                    def lr(c, sgn):
                        if np.isnan(VX[k, c]): return None, None
                        return round(float(sgn * (-VX[k, c] * uy[k] + VY[k, c] * ux[k])), 2), round(float(sgn * (VX[k, c] * ux[k] + VY[k, c] * uy[k])), 2)
                    al, ar = lr(A, 1); bl, br = lr(B, -1)
                    rows.append([round(float(t[k] - t[k0]), 2), round(float(dist[k]), 2), al, ar, bl, br, round(float(eA[k]), 1), round(float(eB[k]), 1),
                                 round(float(SH[k, A]), 3), round(float(HP[k, A]), 3), round(float(SH[k, B]), 3), round(float(HP[k, B]), 3),
                                 int(FIRE1[k, A]), int(FIRE1[k, B]), None if np.isnan(VZ[k, A]) else round(float(VZ[k, A]), 2), None if np.isnan(VZ[k, B]) else round(float(VZ[k, B]), 2),
                                 round(float(Z[k, B] - Z[k, A]), 2)])
                if len(rows) < 3: continue
                def near(c, k, team_side):
                    m = alive[k] & (np.arange(N) != c) & (np.arange(N) != (B if c == A else A)) & (np.array(slots) != me)
                    if inf: same = ZOMB[k] == ZOMB[k, c]
                    else: same = TEAM[k] == TEAM[k, c]
                    d = np.hypot(X[k] - X[k, c], Y[k] - Y[k, c])
                    return int((m & same & (d < 15)).sum()) if team_side else int((m & ~same & (d < 30)).sum())
                ks = s0
                taken = lambda c: round(float(max(0.0, (SH[k0, c] + HP[k0, c]) - min(SH[k0:k1 + 1, c] + HP[k0:k1 + 1, c]))), 3)
                out.append({'kind': 'duel', 'file': os.path.basename(path), 'map': head.get('map'), 'mode': head.get('mode'), 'variant': head.get('variant'), 'class': cls,
                            'team_game': (not inf) and len({int(x) for x in TEAM[ks][alive[ks]]}) < max(2, int(alive[ks].sum())),
                            'a': slots[A], 'b': slots[B], 't0': round(float(t[k0]), 2), 'attack_t': round(float(t[s0] - t[k0]), 2),
                            'ctx': {'a_wpn': WPN[ks, A], 'b_wpn': WPN[ks, B], 'a_veh': VNAME[ks, A], 'b_veh': VNAME[ks, B], 'dist': round(float(dist[ks]), 2),
                                    'dz': round(float(Z[ks, B] - Z[ks, A]), 2), 'a_sh': round(float(SH[ks, A]), 3), 'a_hp': round(float(HP[ks, A]), 3),
                                    'b_sh': round(float(SH[ks, B]), 3), 'b_hp': round(float(HP[ks, B]), 3), 'a_err': round(float(eA[ks]), 1), 'b_err': round(float(eB[ks]), 1),
                                    'b_err_aware': round(float(eB[k0]), 1), 'a_mates': near(A, ks, 1), 'b_mates': near(B, ks, 1), 'a_foes': near(A, ks, 0), 'b_foes': near(B, ks, 0),
                                    'a_zombie': bool(ZOMB[ks, A]) if inf else None, 'b_zombie': bool(ZOMB[ks, B]) if inf else None},
                            'rows': rows,
                            'out': {'win': win, 'how': how, 'dur': round(float(t[s1] - t[s0]), 2), 'a_taken': taken(A), 'b_taken': taken(B),
                                    'gren_known': bool((GREN[k0:k1 + 1, A] >= 0).any()),
                                    'a_throws': [[round(float(t[q] - t[k0]), 2), round(float(dist[q]), 2)] for q in range(k0, k1 + 1) if THROW[q, A]],
                                    'b_throws': [[round(float(t[q] - t[k0]), 2), round(float(dist[q]), 2)] for q in range(k0, k1 + 1) if THROW[q, B]]}})
    out.extend(multi(path, head, t, slots, me, inf, cls, alive, X, Y, Z, YAW, SH, HP, TEAM, ZOMB, FIRE, HURT, VX, VY, kills))
    return out

def multi(path, head, t, slots, me, inf, cls, alive, X, Y, Z, YAW, SH, HP, TEAM, ZOMB, FIRE, HURT, VX, VY, kills):
    T, N = alive.shape; out = []; base = {'file': os.path.basename(path), 'map': head.get('map'), 'mode': head.get('mode'), 'class': cls}
    pl = np.array([s_ != me for s_ in slots]); side = ZOMB if inf else TEAM
    team_game = (not inf) and any(len({int(x) for x in TEAM[k][alive[k]]}) < int(alive[k].sum()) for k in range(0, T, max(1, T // 20)) if alive[k].sum() > 2)
    ATT = np.zeros((T, N, N), bool); TGT = np.full((T, N), -1, np.int32)
    for c0 in range(0, T, 1500):   # (chunks: [T, N, N] float arrays of a long match are big)
        sl = slice(c0, min(T, c0 + 1500))
        dx = X[sl, None, :] - X[sl, :, None]; dy = Y[sl, None, :] - Y[sl, :, None]; dz = Z[sl, None, :] - Z[sl, :, None]
        dist = np.sqrt(dx * dx + dy * dy + dz * dz); err = np.abs(wrap(YAW[sl, :, None] - np.degrees(np.arctan2(dy, dx))))
        valid = alive[sl, :, None] & alive[sl, None, :] & (side[sl, :, None] != side[sl, None, :]) & (dist < R_MAX) & (dist > 0.4) & pl[None, :, None] & pl[None, None, :]
        a = valid & (((err < AIM_FIRE) & FIRE[sl, :, None]) | ((err < AIM_DMG) & HURT[sl, None, :]))
        ATT[sl] = a; e2 = np.where(a, err, 999.0); TGT[sl] = np.where(a.any(axis=2), e2.argmin(axis=2), -1)
    # held target: kept through pauses under 1 s
    HT = np.full((T, N), -1, np.int32)
    for i in range(N):
        cur, last = -1, -1e9
        for k in range(T):
            if TGT[k, i] >= 0: cur, last = TGT[k, i], t[k]
            elif t[k] - last > 1.0 or not alive[k, i]: cur = -1
            HT[k, i] = cur
    cs = np.cumsum(ATT, axis=0, dtype=np.int32)
    def recent(k, w):   # [N, N]: attacked in the w samples up to k
        return (cs[k] - (cs[k - w] if k >= w else 0)) > 0
    def geom(k, i, c):
        dx, dy = X[k, c] - X[k, i], Y[k, c] - Y[k, i]; d = math.sqrt(dx * dx + dy * dy + (Z[k, c] - Z[k, i]) ** 2)
        return d, abs((YAW[k, i] - math.degrees(math.atan2(dy, dx)) + 180) % 360 - 180)
    def vit(k, c): return float(min(1.5, (SH[k, c] + HP[k, c]) / 2.0))
    def mates_on(k, i, c):   # a teammate of i attacking c
        return bool(((side[k] == side[k, i]) & (np.arange(N) != i) & ATT[k, :, c] & pl).any())
    # choices and switches
    for i in range(N):
        if not pl[i]: continue
        prevtgt, prevt = -1, -1e9
        for k in range(1, T):
            h = HT[k, i]
            if h >= 0 and h != HT[k - 1, i]:
                kd = max(0, k - 5); rc = recent(kd, 15)
                cands = [c for c in range(N) if c != i and pl[c] and alive[kd, c] and alive[k, c] and side[kd, c] != side[kd, i] and geom(kd, i, c)[0] < R_MAX]
                if h in cands and len(cands) >= 2:
                    F = []
                    for c in cands:
                        d, ang = geom(kd, i, c)
                        F.append([round(math.log(max(d, 0.5)), 3), round(ang / 180.0, 3), int(rc[c, i]), round(vit(kd, c), 3), int(c == prevtgt and t[k] - prevt < 3.0),
                                  int(mates_on(kd, i, c)), int(HURT[kd, c] or (kd >= 10 and (SH[kd, c] + HP[kd, c]) < (SH[kd - 10, c] + HP[kd - 10, c]) - 0.05)), round(float(Z[kd, c] - Z[kd, i]) / 5.0, 3)])
                    out.append(dict(base, kind='choice', team_game=team_game, chosen=cands.index(h), feat=F, a_vit=round(vit(kd, i), 3),
                                    switch=bool(prevtgt >= 0 and prevtgt != h and t[k] - prevt < 3.0 and alive[k, prevtgt])))
            if HT[k - 1, i] >= 0: prevtgt, prevt = HT[k - 1, i], t[k - 1]
        # switch: fighting B for 1 s+, a new attacker C (no attack on i from C in the 1.5 s before)
        k = 15; since = None
        while k < T:
            b = HT[k, i]
            if b < 0 or not alive[k, i]: since = None; k += 1; continue
            if since is None or HT[k - 1, i] != b: since = t[k]
            if t[k] - since >= 1.0:
                newc = [c for c in range(N) if c != b and ATT[k, c, i] and not recent(k - 1, 15)[c, i]]
                if newc:
                    c = newc[0]; turned = None; died = False; k2 = k
                    while k2 < T and t[k2] - t[k] <= 3.0:
                        if not alive[k2, i]: died = True; break
                        if HT[k2, i] == c: turned = round(float(t[k2] - t[k]), 2); break
                        k2 += 1
                    d5 = bool((~alive[k:min(T, k + 50), i]).any())
                    db, _ = geom(k, i, b); dc, ac = geom(k, i, c)
                    out.append(dict(base, kind='switch', team_game=team_game, turned=turned, died5=d5, b_vit=round(vit(k, b), 3), c_vit=round(vit(k, c), 3),
                                    own_vit=round(vit(k, i), 3), b_dist=round(db, 2), c_dist=round(dc, 2), c_ang=round(ac, 1), fought_b_s=round(float(t[k] - since), 2)))
                    k += 30; continue
            k += 1
    # pressure: attackers within 1.5 s; runs of >= 2 (outnumbered) and of exactly 1 (for comparison), >= 1 s
    NA = np.zeros((T, N), np.int32)
    for k in range(T): NA[k] = recent(k, 15).sum(axis=0)
    for i in range(N):
        if not pl[i]: continue
        k = 0
        while k < T:
            lvl = 2 if NA[k, i] >= 2 else 1 if NA[k, i] == 1 else 0
            if not lvl or not alive[k, i]: k += 1; continue
            k1 = k
            while k1 + 1 < T and alive[k1 + 1, i] and (2 if NA[k1 + 1, i] >= 2 else 1 if NA[k1 + 1, i] == 1 else 0) == lvl: k1 += 1
            if t[k1] - t[k] >= 1.0:
                atk = np.nonzero(recent(k1, 15)[:, i] | recent(k, 15)[:, i])[0]
                back = 0
                for q in range(k, k1 + 1):
                    if np.isnan(VX[q, i]) or not len(atk): continue
                    cx, cy = X[q, atk].mean() - X[q, i], Y[q, atk].mean() - Y[q, i]; hn = math.hypot(cx, cy)
                    if hn > 0.1 and (VX[q, i] * cx + VY[q, i] * cy) / hn < -1.0: back += 1
                died = bool((~alive[k1:min(T, k1 + 30), i]).any())
                kl = sum(1 for (kt, ka, kb, kh) in kills if ka == slots[i] and t[k] <= kt <= t[k1] + 3.0)
                out.append(dict(base, kind='pressure', team_game=team_game, attackers=int(max(NA[k:k1 + 1, i])), level=lvl, dur=round(float(t[k1] - t[k]), 2),
                                retreat_share=round(back / max(1, k1 - k + 1), 3), died=died, kills=kl, own_vit=round(vit(k, i), 3), mates=int(((side[k] == side[k, i]) & alive[k] & pl & (np.hypot(X[k] - X[k, i], Y[k] - Y[k, i]) < 15)).sum()) - 1))
            k = k1 + 1
    # join (team games): a teammate starts on enemy c within 25 units of i, i has no target -> i on c within 3 s?
    if team_game:
        for k in range(16, T):
            new = ATT[k] & ~recent(k - 1, 15)
            for kk, c in zip(*np.nonzero(new)):
                for i in range(N):
                    if i == kk or not pl[i] or not alive[k, i] or side[k, i] != side[k, kk] or HT[k, i] >= 0: continue
                    d, ang = geom(k, i, c)
                    if d > 25: continue
                    k2 = k; j = None
                    while k2 < T and t[k2] - t[k] <= 3.0:
                        if HT[k2, i] == c: j = round(float(t[k2] - t[k]), 2); break
                        if HT[k2, i] >= 0: break
                        k2 += 1
                    out.append(dict(base, kind='join', joined=j, dist=round(d, 2), ang=round(ang, 1), c_vit=round(vit(k, c), 3), other=bool(HT[k2, i] >= 0 and HT[k2, i] != c) if k2 < T else False))
    # skirmishes every 3 s: components of the attack graph over the last 3 s
    for k in range(30, T, 30):
        g = recent(k, 30); g = g | g.T; seen = set()
        for s0 in range(N):
            if s0 in seen or not g[s0].any(): continue
            comp, stack = set(), [s0]
            while stack:
                u = stack.pop()
                if u in comp: continue
                comp.add(u); stack.extend(int(v) for v in np.nonzero(g[u])[0] if v not in comp)
            seen |= comp
            sides = Counter(int(side[k, u]) for u in comp)
            out.append(dict(base, kind='skirmish', size=len(comp), sides=sorted(sides.values(), reverse=True)))
    return out

def main():
    os.makedirs(OUT, exist_ok=True)
    recs = sorted(glob.glob(os.path.join(TRACKS, '*.jsonl.gz'))); done = tot = 0
    for p in recs:
        if not FORCE and time.time() - os.path.getmtime(p) < 120: continue   # (still being recorded; --force takes it: a battle's folder right after it ends)
        o = os.path.join(OUT, os.path.basename(p).replace('.jsonl.gz', '.eng.jsonl.gz'))
        if not FORCE and os.path.exists(o) and os.path.getmtime(o) >= os.path.getmtime(p): continue
        eps = cut(p); tmp = o + '.tmp'
        with gzip.open(tmp, 'wt') as f:
            for e in eps: f.write(json.dumps(e, separators=(',', ':')) + '\n')
        os.rename(tmp, o); done += 1; tot += len(eps)
        if not QUIET:
            w = {}
            for e in eps: w[e['out']['win']] = w.get(e['out']['win'], 0) + 1
            print('%-48s %4d duels %s' % (os.path.basename(p), len(eps), ' '.join('%s %d' % kv for kv in sorted(w.items()))))
    print('%d recordings cut, %d duels -> %s' % (done, tot, OUT))

if __name__ == '__main__':
    main()
