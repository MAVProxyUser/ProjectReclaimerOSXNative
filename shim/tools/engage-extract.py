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
  team         TEAMWORK (the user, 2026-11-08: "noting both enemy and friendly radar blips, and how they may be cooperative working
               together ... cooperate with other team mates toward a common offense, or if one comes to your aid ... collective
               defense as a unit"), team games, one per recording: the nearest teammate's distance (all, and while fighting),
               the share of time with a teammate within 8 / 15 / 25 units, and focus fire (the share of attacking players whose
               target another teammate attacks too)
  aid          team games: an enemy starts attacking a player with teammates within 25 units: did one of them attack that
               enemy within 3 s (aid by fire), or come toward the player (1+ unit/s over a second), how fast, and did the player
               die within 5 s -- helped vs not
  contact      RADAR (the user, 2026-11-08: "the walkers don't understand how the radar works, and that is critical"): an enemy
               within 15 units comes up UNSEEN (more than 75 deg off the player's view), not attacking it, while the player has
               no target: its distance, its speed (halo3's motion tracker shows movement, not a crouch-walk or standing), and
               whether / how fast the player turned to face it (within 30 deg in 2.5 s) before being attacked -- moving vs still
               enemies at the same distance = what the radar adds; the distance where turning falls off = its range
  respawn      REVENGE (the user: "a propensity to return to where you were just killed to seek out the person that just
               killed you"): after each death with a known killer, the first 40 s of the next life: time to come within 8 units
               of where it died (and, as a baseline, of another death spot on the map as far from the spawn), whether its next
               target was the killer (chance = 1 / enemies alive), and its approach speed toward the killer vs other enemies
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

_WALKER = None
def walker_files():   # the recordings a walker made (shim/tools/walkerrec.py), read once a run
    global _WALKER
    if _WALKER is None:
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__))); import walkerrec; _WALKER = walkerrec.walker_files()
    return _WALKER

def mode_class(mode, variant, engine=None):
    # the game itself first: the engine the recording saw ("Slayer", "Capture the Flag", "Infection"), then the variant's name,
    # then the server listing's mode last (2026-11-08: a "CTF 30VS30" server played "BIGASSSLAYER": filed as CTF)
    e = (engine or '').lower()
    if e: return 'infection' if 'infection' in e else 'ctf' if 'capture' in e or 'flag' in e else 'slayer' if 'slayer' in e else 'other'
    for t in ((variant or '').lower(), (mode or '').lower()):
        if any(k in t for k in ('infection', 'zombie', 'flood', 'braaa')): return 'infection'
        if any(k in t for k in ('ctf', 'capture the flag', 'flag')): return 'ctf'
        if any(k in t for k in ('slayer', 'swat', 'snipe', 'br starts', 'fiesta', 'ffa', 'free for all', 'rumble', 'doubles', 'beams', 'kills', 'mlg', 'team br')): return 'slayer'
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
def wcls(w):   # a weapon name -> its class (the same classes as shim/tools/coach.py)
    w = (w or '').upper()
    for c, keys in (('sniper', ('SNIPER', 'BEAM RIFLE')), ('laser', ('SPARTAN LASER',)), ('rocket', ('ROCKET', 'FUEL ROD', 'MISSILE')), ('shotgun', ('SHOTGUN', 'EXCAVATOR', 'MAULER')),
                    ('sword', ('ENERGY BLADE', 'ENERGY SWORD', 'HAMMER')), ('precision', ('BATTLE RIFLE', 'CARBINE', 'MAGNUM')), ('auto', ('ASSAULT RIFLE', 'SMG', 'PLASMA RIFLE', 'SPIKE', 'NEEDLER', 'BRUTE SHOT'))):
        if any(k in w for k in keys): return c
    return 'other'

def cut(path):
    head, S, E = load(path)
    if not head or len(S) < 100: return []
    slots = sorted({r[0] for s in S for r in s['p'] if isinstance(r[0], int) and 0 <= r[0] < 256})
    if not slots: return []
    ix = {sl: k for k, sl in enumerate(slots)}; T, N = len(S), len(slots)
    t = np.array([s['t'] for s in S], dtype=np.float64)
    alive = np.zeros((T, N), bool); X = np.zeros((T, N)); Y = np.zeros((T, N)); Z = np.zeros((T, N)); YAW = np.zeros((T, N))
    SH = np.zeros((T, N)); HP = np.zeros((T, N)); SHOTS = np.zeros((T, N)); TEAM = np.full((T, N), -1, int); VEH = np.zeros((T, N), bool)
    KILLS = np.zeros((T, N)); DEATHS = np.zeros((T, N)); HITS = np.zeros((T, N)); PRESENT = np.zeros((T, N), bool)
    WPN = np.full((T, N), '', dtype=object); VNAME = np.full((T, N), '', dtype=object); you = np.full(T, -1, int); GREN = np.full((T, N), -1.0)
    CAMO = np.zeros((T, N)); OTHER = np.full((T, N), None, dtype=object)   # (columns 17 / 18 since 2026-11-08 16:35: None = not recorded)
    for k, s in enumerate(S):
        you[k] = s.get('you', -1)
        for r in s['p']:
            if not isinstance(r[0], int) or r[0] not in ix: continue
            c = ix[r[0]]; PRESENT[k, c] = True; alive[k, c] = bool(r[2]) and (r[3] or r[4]); X[k, c], Y[k, c], Z[k, c] = r[3], r[4], r[5]; YAW[k, c] = r[6]
            SH[k, c], HP[k, c], TEAM[k, c] = r[8], r[9], r[1]; WPN[k, c] = r[10] or ''; VNAME[k, c] = r[11] or ''; VEH[k, c] = bool(r[11])
            SHOTS[k, c] = r[14] if len(r) > 14 else 0
            KILLS[k, c] = r[12] if len(r) > 12 else 0; DEATHS[k, c] = r[13] if len(r) > 13 else 0; HITS[k, c] = r[15] if len(r) > 15 else 0
            if len(r) > 16: GREN[k, c] = r[16]
            if len(r) > 18: CAMO[k, c] = r[17] or 0; OTHER[k, c] = r[18] or ''
    local = int(np.bincount(you[you >= 0]).argmax()) if (you >= 0).any() else -1
    # the observer (left out: it only stands there) -- but in a recording of the local player's own game (nhook's recorder: you_plays)
    # "you" is a real player: kept in, and named in the meta record as local_slot (shim/tools/coach.py)
    # (2026-11-09) -- unless a WALKER drives "you" (the public walker, walker battles: shim/tools/walkerrec.py): a bot, left out like the
    # observer, and never "you" for coach.py
    walker = bool(head.get('walker')) or os.path.basename(path) in walker_files()
    me = -1 if head.get('you_plays') and not walker else local
    cls = mode_class(head.get('mode'), head.get('variant'), head.get('engine') or (head.get('mode') if head.get('source') == 'nhook' else None)); inf = cls == 'infection'
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
    out.extend(items(path, head, t, slots, me, cls, alive, X, Y, Z, VX, VY, E))
    out.append(weapons(path, head, t, slots, me, cls, alive, WPN, VEH, E))
    out.extend(inventory(path, head, t, slots, me, inf, cls, alive, X, Y, Z, TEAM, ZOMB, VEH, WPN, OTHER, CAMO, FIRE, HURT, SHOTS, KILLS))
    # META (the tome: shim/tools/engage-model.py tracks/tome.json): what this recording is -- map, game type, length, players
    out.append({'kind': 'meta', 'file': os.path.basename(path), 'map': head.get('map'), 'mode': head.get('mode'), 'variant': head.get('variant'), 'class': cls,
                'start': head.get('start'), 'samples': int(T), 'seconds': round(float(t[-1] - t[0]), 1) if T > 1 else 0, 'players_max': int(alive.sum(axis=1).max()) if T else 0,
                'source': head.get('source') or 'match-recorder', 'kills': sum(1 for e in E if e.get('ev') == 'kill'),
                'local_slot': local if head.get('you_plays') and me < 0 else None, 'walker': me >= 0 and bool(head.get('you_plays'))})
    # PLAYERS: one summary per player of the match (coach.py's winners: the top quarter by kills / deaths)
    dtt = np.clip(np.diff(t, prepend=t[0]), 0, 0.5)
    for c in range(N):
        if slots[c] == me or not PRESENT[:, c].any(): continue
        pres = float(dtt[PRESENT[:, c]].sum())
        if pres < 60: continue
        out.append({'kind': 'player', 'file': os.path.basename(path), 'map': head.get('map'), 'class': cls, 'slot': slots[c], 'you': bool(head.get('you_plays') and me < 0 and slots[c] == local),
                    'seconds': round(pres, 1), 'kills': int(KILLS[:, c].max()), 'deaths': int(DEATHS[:, c].max()), 'shots': int(SHOTS[:, c].max()), 'hits': int(HITS[:, c].max()),
                    'team': int(np.bincount(TEAM[PRESENT[:, c], c] + 1).argmax()) - 1 if PRESENT[:, c].any() else -1})
    return out

POWER = ('rocket', 'sniper', 'beam rifle', 'spartan laser', 'shotgun', 'energy sword', 'gravity hammer', 'hammer', 'fuel rod', 'missile pod', 'flamethrower', 'plasma cannon', 'overshield', 'camo', 'power-up', 'powerup')
def inventory(path, head, t, slots, me, inf, cls, alive, X, Y, Z, TEAM, ZOMB, VEH, WPN, OTHER, CAMO, FIRE, HURT, SHOTS, KILLS):
    """the players' own inventory (2026-11-08; the user: "the walker should be aware of its own inventory, and power up status"),
    from recordings with the other-weapon / camo columns: kind "wswitch" = a weapon switch (the one in hand and the one carried trade
    places) with the nearest enemy's distance, fighting or not; kind "camo" = a stretch under active camo: its length, how much of it
    the player fired, kills during it, the distance of its first shot, the closing speed toward the nearest enemy"""
    out = []; T, N = alive.shape; base = {'file': os.path.basename(path), 'map': head.get('map'), 'mode': head.get('mode'), 'class': cls}
    if T < 10 or not any(OTHER[k, c] is not None for k in (0, T // 2, T - 1) for c in range(N)): return out
    side = ZOMB if inf else TEAM; pl = np.array([s_ != me for s_ in slots])
    def nearest(k, i):
        en = alive[k] & (side[k] != side[k, i]) & (np.arange(N) != i) & pl
        if inf or (side[k] < 0).all(): en = alive[k] & (np.arange(N) != i) & pl   # (free-for-all: everyone)
        if not en.any(): return None
        d = np.sqrt((X[k] - X[k, i]) ** 2 + (Y[k] - Y[k, i]) ** 2 + (Z[k] - Z[k, i]) ** 2); d = np.where(en, d, np.inf); return float(d.min())
    for i in range(N):
        if not pl[i]: continue
        for k in range(1, T):
            if not (alive[k, i] and alive[k - 1, i]) or VEH[k, i] or VEH[k - 1, i]: continue
            a0, b0, a1, b1 = WPN[k - 1, i], OTHER[k - 1, i], WPN[k, i], OTHER[k, i]
            if a0 and b0 and a1 == b0 and b1 == a0 and a0 != b0:
                fight = bool(FIRE[max(0, k - 30):k + 1, i].any() or HURT[max(0, k - 30):k + 1, i].any()); d = nearest(k, i)
                out.append(dict(base, kind='wswitch', i=slots[i], t=round(float(t[k]), 2), frm=a0, to=a1, frm_c=wcls(a0), to_c=wcls(a1), dist=None if d is None else round(d, 1), fight=fight))
        k = 0
        while k < T:
            if not (alive[k, i] and CAMO[k, i] > 0.05): k += 1; continue
            k0 = k
            while k < T and alive[k, i] and CAMO[k, i] > 0.05: k += 1
            if k - k0 < 10: continue
            fired = FIRE[k0:k, i]; f1 = int(np.argmax(fired)) if fired.any() else None
            d0 = nearest(k0, i); df = nearest(k0 + f1, i) if f1 is not None else None
            dk = KILLS[k - 1, i] - KILLS[k0, i]
            out.append(dict(base, kind='camo', i=slots[i], t=round(float(t[k0]), 2), dur=round(float(t[k - 1] - t[k0]), 1), fired_share=round(float(fired.mean()), 3),
                            kills=int(max(0, dk)), first_shot_dist=None if df is None else round(df, 1), nearest_at_start=None if d0 is None else round(d0, 1),
                            died=bool(k < T and not alive[k, i])))
    return out

def weapons(path, head, t, slots, me, cls, alive, WPN, VEH, E):
    """WEAPONS (the user, 2026-11-08: "gather some stats about the most effective weapons"): per weapon (lower case) the seconds
    it was held in hand by live players on foot (the observer left out), the kills credited to it by the game (the kill event's
    weapon), the kills by a player holding it, and the deaths of players holding it."""
    T, N = alive.shape; held, kh, dh, kev = {}, {}, {}, {}
    dt = np.diff(t, prepend=t[0]); dt = np.clip(dt, 0, 0.5)
    for c in range(N):
        if slots[c] == me: continue
        m = alive[:, c] & ~VEH[:, c]
        for w in set(WPN[m, c]):
            if w: held[w.lower()] = held.get(w.lower(), 0.0) + float(dt[m & (WPN[:, c] == w)].sum())
    for e in E:
        if e.get('ev') != 'kill' or e.get('verb') != 'killed': continue
        how = (e.get('how') or '').lower(); kev[how] = kev.get(how, 0) + 1
        k = int(np.searchsorted(t, e['t'])) - 1
        for slot, acc in ((e.get('a'), kh), (e.get('b'), dh)):
            if slot not in slots or slot == me: continue
            c = slots.index(slot); q = k
            while q > 0 and q > k - 30 and not (alive[q, c] and WPN[q, c]): q -= 1   # (the weapon in hand just before)
            w = (WPN[q, c] or '').lower() if q >= 0 else ''
            if w and not VEH[q, c]: acc[w] = acc.get(w, 0) + 1
    return {'kind': 'weapons', 'file': os.path.basename(path), 'map': head.get('map'), 'mode': head.get('mode'), 'class': cls,
            'held_s': {k: round(v, 1) for k, v in held.items()}, 'kills_holding': kh, 'deaths_holding': dh, 'kills_by_event': kev}

def items(path, head, t, slots, me, cls, alive, X, Y, Z, VX, VY, E):
    """ITEMS (the user, 2026-11-08: "players seek out spawning power weapons, and items like overshield and camo"): each power
    weapon / power-up spawn event with a position: every player within 40 units at that moment -> its approach speed toward the
    item over the next 10 s, against the same player's approach speed toward a point as far away in another direction (the
    baseline: players move anyway); how soon the first player comes within 1.5 units (a pickup, roughly) and whether that was
    the nearest one when it spawned."""
    out = []; T, N = alive.shape; pl = np.array([s_ != me for s_ in slots]); rnd = __import__('random').Random(int(t[-1]) if len(t) else 1)
    base = {'file': os.path.basename(path), 'map': head.get('map'), 'mode': head.get('mode'), 'class': cls}
    for e in E:
        if e.get('ev') not in ('weapon', 'powerup') or e.get('verb') != 'spawned' or not e.get('pos'): continue
        how = (e.get('how') or '').lower()
        if e['ev'] == 'weapon' and not any(k in how for k in POWER): continue
        k0 = int(np.searchsorted(t, e['t']))
        if k0 >= T - 50 or k0 < 1 or e['t'] - t[0] < 20: continue   # (the match start's lineup is no spawn players react to)
        ix, iy = e['pos'][0], e['pos'][1]; near = []; first = None
        for c in range(N):
            if not pl[c] or not alive[k0, c]: continue
            d0 = math.hypot(X[k0, c] - ix, Y[k0, c] - iy)
            if d0 > 40 or d0 < 2: continue
            a = rnd.uniform(0, 2 * math.pi); bx, by = X[k0, c] + d0 * math.cos(a), Y[k0, c] + d0 * math.sin(a)   # (a point as far, another way)
            ri, rb = [], []
            for q in range(k0, min(T, k0 + 100)):
                if not alive[q, c] or np.isnan(VX[q, c]): continue
                for (px, py, acc) in ((ix, iy, ri), (bx, by, rb)):
                    ex, ey = px - X[q, c], py - Y[q, c]; hn = math.hypot(ex, ey)
                    if hn > 0.5: acc.append((VX[q, c] * ex + VY[q, c] * ey) / hn)
            near.append((d0, c, float(np.mean(ri)) if ri else None, float(np.mean(rb)) if rb else None))
        for q in range(k0, min(T, k0 + 600)):
            hit = [c for c in range(N) if pl[c] and alive[q, c] and math.hypot(X[q, c] - ix, Y[q, c] - iy) < 1.5]
            if hit: first = (round(float(t[q] - t[k0]), 1), hit[0]); break
        if not near: continue
        near.sort(); nearest = near[0][1]
        out.append(dict(base, kind='item', what=e.get('how'), ev=e['ev'], players=len(near),
                        approach=[[round(d, 1), None if a is None else round(a, 3), None if b is None else round(b, 3)] for d, c, a, b in near],
                        first_s=first[0] if first else None, first_was_nearest=(first[1] == nearest) if first else None))
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
                out.append(dict(base, kind='pressure', i=slots[i], team_game=team_game, attackers=int(max(NA[k:k1 + 1, i])), level=lvl, dur=round(float(t[k1] - t[k]), 2),
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
    # radar: unseen quiet contacts within 15 units while the player has no target (vectorized per player: [T, N] at once)
    SPD = np.hypot(np.nan_to_num(VX), np.nan_to_num(VY))
    R15 = np.zeros((T, N, N), bool); R15[15:] = (cs[15:] - cs[:-15]) > 0; R15[:15] = cs[:15] > 0
    for i in range(N):
        if not pl[i]: continue
        dx, dy = X - X[:, i:i + 1], Y - Y[:, i:i + 1]; d = np.hypot(dx, dy)
        off = np.abs(wrap(YAW[:, i:i + 1] - np.degrees(np.arctan2(dy, dx))))
        cond = (alive[:, i:i + 1] & (HT[:, i:i + 1] < 0) & alive & pl[None, :] & (side != side[:, i:i + 1]) & (d < 15) & (off > 75) & ~R15[:, :, i])
        cond[:, i] = False
        new = cond.copy(); new[1:] &= ~cond[:-1]; new[:15] = False; new[T - 25:] = False
        last_ev = {}
        for k, c in zip(*np.nonzero(new)):
            if t[k] - last_ev.get(c, -1e9) < 3.0: continue
            last_ev[c] = t[k]; turned = None; hit = False
            for q in range(k, min(T, k + 26)):
                if not alive[q, i] or not alive[q, c]: break
                if ATT[q, c, i]: hit = True; break
                ex, ey = X[q, c] - X[q, i], Y[q, c] - Y[q, i]
                if abs((YAW[q, i] - math.degrees(math.atan2(ey, ex)) + 180) % 360 - 180) < 30: turned = round(float(t[q] - t[k]), 2); break
            out.append(dict(base, kind='contact', i=slots[i], dist=round(float(d[k, c]), 2), off=round(float(off[k, c]), 1), speed=round(float(SPD[k, c]), 2), own_speed=round(float(SPD[k, i]), 2),
                            dz=round(float(Z[k, c] - Z[k, i]), 2), turned=turned, attacked_first=hit))
    # revenge: each death with a known killer -> the next life's first 40 s
    death_spots = [(kt, kb, ka) for (kt, ka, kb, kh) in kills]
    spots_xy = []
    for (kt, ka, kb, kh) in kills:
        if kb in slots:
            v = slots.index(kb); kk = int(np.searchsorted(t, kt)) - 1
            if 0 <= kk < T: spots_xy.append((X[kk, v], Y[kk, v]))
    rnd = __import__('random').Random(len(t))
    for (kt, ka, kb, kh) in kills:
        if ka not in slots or kb not in slots or ka == kb: continue
        v, kl = slots.index(kb), slots.index(ka)
        if not pl[v]: continue
        kd = int(np.searchsorted(t, kt)) - 1
        if kd < 0: continue
        sx, sy = X[kd, v], Y[kd, v]
        if not (sx or sy):   # (the death sample may already be dead: the last live position)
            q = kd
            while q > 0 and not alive[q, v]: q -= 1
            sx, sy = X[q, v], Y[q, v]
        r0 = kd + 1   # (the respawn: alive again, at least 1 s after the kill)
        while r0 < T and not (alive[r0, v] and t[r0] - kt > 1.0): r0 += 1
        if r0 >= T - 10: continue
        px, py = X[r0, v], Y[r0, v]; d0 = math.hypot(px - sx, py - sy)
        if d0 < 10: continue   # (respawned next to it: nothing to measure)
        cand = [p2 for p2 in spots_xy if abs(math.hypot(px - p2[0], py - p2[1]) - d0) < 0.2 * d0 and math.hypot(p2[0] - sx, p2[1] - sy) > 15]
        bx, by = rnd.choice(cand) if cand else (None, None)
        reach = base_reach = None; nxt = None; appk = []; appo = []
        for q in range(r0, min(T, r0 + 400)):
            if not alive[q, v]: break
            tt = float(t[q] - t[r0])
            if reach is None and math.hypot(X[q, v] - sx, Y[q, v] - sy) < 8: reach = round(tt, 2)
            if bx is not None and base_reach is None and math.hypot(X[q, v] - bx, Y[q, v] - by) < 8: base_reach = round(tt, 2)
            if nxt is None and HT[q, v] >= 0: nxt = 'killer' if HT[q, v] == kl else 'other'
            if tt < 15 and not np.isnan(VX[q, v]):
                ex, ey = X[q] - X[q, v], Y[q] - Y[q, v]; hn = np.hypot(ex, ey)
                m = pl & alive[q] & (side[q] != side[q, v]) & (hn >= 0.5); m[v] = False
                if m.any():
                    rv = (VX[q, v] * ex + VY[q, v] * ey) / np.where(hn > 0, hn, 1)
                    if m[kl]: appk.append(float(rv[kl]))
                    m[kl] = False; appo.extend(float(x) for x in rv[m])
        nen = int(sum(1 for c in range(N) if c != v and pl[c] and alive[r0, c] and side[r0, c] != side[r0, v]))
        out.append(dict(base, kind='respawn', v=slots[v], team_game=team_game, d0=round(d0, 1), reached=reach, base_reached=base_reach if bx is not None else 'none',
                        next_target=nxt, enemies=nen, killer_alive=bool(alive[r0, kl]), app_killer=round(float(np.mean(appk)), 3) if appk else None,
                        app_other=round(float(np.mean(appo)), 3) if appo else None, respawn_s=round(float(t[r0] - kt), 1)))
    # teamwork: cohesion, focus fire, aid (team games only)
    if team_game:
        near, near_eng, foc, att_n = [], [], 0, 0
        for k in range(0, T, 5):
            idx = np.nonzero(alive[k] & pl)[0]
            if len(idx) < 3: continue
            for i in idx:
                mates = idx[(side[k, idx] == side[k, i]) & (idx != i)]
                if not len(mates): continue
                dmin = float(np.hypot(X[k, mates] - X[k, i], Y[k, mates] - Y[k, i]).min()); near.append(dmin)
                if HT[k, i] >= 0:
                    near_eng.append(dmin); att_n += 1; tg = HT[k, i]
                    if any(HT[k, m] == tg for m in mates): foc += 1
        if near:
            q = lambda v, f: float(np.percentile(v, f)) if v else None
            out.append(dict(base, kind='team', samples=len(near), players=int(np.median(alive.sum(axis=1)[alive.sum(axis=1) > 0])) if alive.any() else 0, near_p25=round(q(near, 25), 2), near_median=round(q(near, 50), 2), near_p75=round(q(near, 75), 2),
                            within8=round(sum(d < 8 for d in near) / len(near), 3), within15=round(sum(d < 15 for d in near) / len(near), 3), within25=round(sum(d < 25 for d in near) / len(near), 3),
                            near_fighting_median=round(q(near_eng, 50), 2) if near_eng else None, within15_fighting=round(sum(d < 15 for d in near_eng) / len(near_eng), 3) if near_eng else None,
                            focus_share=round(foc / att_n, 3) if att_n else None))
        # aid: a NEW attack on a player (no attack from that enemy in the 1.5 s before) with teammates within 25 units
        for k in range(16, T - 50, 2):
            onset = ATT[k] & ~R15[k - 1]
            for e_, p_ in zip(*np.nonzero(onset)):
                if not pl[p_] or not alive[k, p_]: continue
                mates = [m for m in range(N) if m != p_ and pl[m] and alive[k, m] and side[k, m] == side[k, p_] and math.hypot(X[k, m] - X[k, p_], Y[k, m] - Y[k, p_]) < 25]
                if not mates: continue
                aid, aid_s, kind_, helper = False, None, None, None
                for q_ in range(k, min(T, k + 30)):
                    hit = [m for m in mates if ATT[q_, m, e_]]
                    if hit: aid, aid_s, kind_, helper = True, round(float(t[q_] - t[k]), 2), 'fire', slots[hit[0]]; break
                if not aid:
                    for m in mates:
                        v = []
                        for q_ in range(k, min(T, k + 30)):
                            if np.isnan(VX[q_, m]): continue
                            ex, ey = X[q_, p_] - X[q_, m], Y[q_, p_] - Y[q_, m]; hn = math.hypot(ex, ey)
                            if hn > 2: v.append((VX[q_, m] * ex + VY[q_, m] * ey) / hn)
                            if len(v) >= 10 and sum(v[-10:]) / 10 > 1.0: aid, aid_s, kind_, helper = True, round(float(t[q_] - t[k]), 2), 'move', slots[m]; break
                        if aid: break
                died = bool((~alive[k:min(T, k + 50), p_]).any())
                nm = min(math.hypot(X[k, m] - X[k, p_], Y[k, m] - Y[k, p_]) for m in mates)
                out.append(dict(base, kind='aid', p=slots[p_], helper=helper, mate_slots=[slots[m] for m in mates][:12], players=int(alive[k].sum()), mates=len(mates), nearest_mate=round(nm, 1), aided=aid, aid_s=aid_s, aid_kind=kind_, died5=died))
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
            for e in eps: k = e['out']['win'] if e.get('kind') == 'duel' else e.get('kind'); w[k] = w.get(k, 0) + 1
            print('%-48s %4d duels %s' % (os.path.basename(p), len(eps), ' '.join('%s %d' % kv for kv in sorted(w.items()))))
    print('%d recordings cut, %d records -> %s' % (done, tot, OUT))

if __name__ == '__main__':
    main()
