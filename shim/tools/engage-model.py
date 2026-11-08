#!/usr/bin/env python3
"""How players fight, from the engagement library (shim/tools/engage-extract.py; 2026-11-08). Reads the duels
(ReclaimerMac/tracks/engagements/*.eng.jsonl.gz) and writes:
  - a report: per game class (slayer / ctf / infection / all) the outcomes (who wins: the first to attack, the one with more
    shield, the one caught looking away, the one with teammates near), distances by weapon, movement in the fight (strafe,
    closing, jumping, by own and the other's shield), retreats and how they end, aim and fire while attacking, time to kill,
    target choice when several enemies are near
  - ReclaimerMac/tracks/engage-model.json for the walkers (nbot.c reads it: NHOOK_BOT_ENGAGE): per weapon class the distance
    band players attack from, the retreat / push rules as the players' numbers, and a MOVEMENT LIBRARY -- 2-second pieces of
    real fights (the attacker's speed across / toward the other, every 0.1 s) by weapon class and distance band, from duels the
    player won or survived, that the walker plays back instead of its own strafe.
    python3 shim/tools/engage-model.py [--eng DIR] [--out FILE] [--json report.json] [--classes slayer,ctf]
Infection duels are reported apart and left out of the walkers' model (zombies rush with swords; --model-classes to change).
Several enemies at once (the extractor's choice / switch / pressure / join / skirmish records): the TARGET CHOICE rule is fitted
as a conditional logit -- P(enemy k) ~ exp(w . features_k) over the enemies within 45 units, features = log distance, how far
off the aim (/180 deg), it attacked me in the last 1.5 s, its shield + health / 2, my previous target, a teammate attacks it,
it was just hurt, height difference / 5 -- and checked against "the nearest" and "the one nearest the aim" (top-1 hit rate on
a held-out fifth); then how often / how fast players turn to a second attacker, what they do and how they fare outnumbered vs
1v1, joining teammates' fights, and skirmish sizes. The model gets the weights ("target") and those numbers ("multi")."""
import glob, gzip, json, math, os, random, sys
from collections import defaultdict, Counter

SUP = os.path.expanduser('~/Library/Application Support/ReclaimerMac')
def arg(k, d):
    a = sys.argv; return a[a.index(k) + 1] if k in a else d
ENG = arg('--eng', os.path.join(SUP, 'tracks', 'engagements')); OUT = arg('--out', os.path.join(SUP, 'tracks', 'engage-model.json'))
MODEL_CLASSES = set(arg('--model-classes', 'slayer,ctf,other').split(','))
# weapon classes (the recorder's names are the HUD's, upper case)
WCLS = [('sniper', ('SNIPER', 'BEAM RIFLE')), ('rocket', ('ROCKET', 'SPARTAN LASER', 'FUEL ROD', 'MISSILE')), ('shotgun', ('SHOTGUN', 'MAULER')),
        ('melee', ('ENERGY BLADE', 'ENERGY SWORD', 'GRAVITY HAMMER', 'HAMMER')), ('precision', ('BATTLE RIFLE', 'CARBINE', 'MAGNUM', 'DMR')),
        ('auto', ('ASSAULT RIFLE', 'SMG', 'SUBMACHINE', 'PLASMA RIFLE', 'SPIKER', 'NEEDLER', 'BRUTE SHOT', 'MACHINEGUN', 'PLASMA PISTOL', 'PLASMA CANNON', 'SENTINEL'))]
def wcls(w):
    w = (w or '').upper()
    if not w: return 'none'
    for c, keys in WCLS:
        if any(k in w for k in keys): return c
    return 'other'
def pct(v, q):
    v = sorted(x for x in v if x is not None)
    if not v: return None
    k = (len(v) - 1) * q; f = int(k); c = min(f + 1, len(v) - 1); return v[f] + (v[c] - v[f]) * (k - f)
def r2(x, n=2): return None if x is None else round(x, n)

FEATS = ['log_dist', 'off_aim', 'attacked_me', 'vitality', 'previous_target', 'teammate_on_it', 'just_hurt', 'height']
def fit_choice(C, iters=400, l2=0.02):
    """Conditional logit by gradient ascent on standardized features. -> (raw weights, standardized weights, top-1 rates)."""
    import numpy as np
    if len(C) < 30: return None
    allf = np.array([f for c in C for f in c['feat']], float); mu = allf.mean(0); sd = allf.std(0); sd[sd < 1e-6] = 1
    rnd = random.Random(5); idx = list(range(len(C))); rnd.shuffle(idx); cut = len(idx) // 5; test = [C[i] for i in idx[:cut]]; train = [C[i] for i in idx[cut:]]
    sets = [((np.array(c['feat'], float) - mu) / sd, c['chosen']) for c in train]; w = np.zeros(len(FEATS))
    for it in range(iters):
        g = -l2 * w
        for F, ch in sets:
            sc = F @ w; sc -= sc.max(); p = np.exp(sc); p /= p.sum(); g += F[ch] - p @ F
        w += 1.0 * g / len(sets)
    def top1(rule):
        return sum(1 for c in test if rule(np.array(c['feat'], float)) == c['chosen']) / max(1, len(test))
    res = {'logit': top1(lambda F: int(np.argmax(((F - mu) / sd) @ w))), 'nearest': top1(lambda F: int(np.argmin(F[:, 0]))),
           'nearest_aim': top1(lambda F: int(np.argmin(F[:, 1]))), 'random': sum(1 / len(c['feat']) for c in test) / max(1, len(test)),
           'sets': len(C), 'mean_candidates': round(sum(len(c['feat']) for c in C) / len(C), 2)}
    raw = w / sd; return {'weights': {k: round(float(v), 3) for k, v in zip(FEATS, raw)}, 'weights_standardized': {k: round(float(v), 3) for k, v in zip(FEATS, w)},
                          'bias_note': 'raw weights apply to raw features (log units, /180 deg, 0/1 flags, vitality 0..1.5, height/5)', 'top1': {k: (round(v, 3) if isinstance(v, float) else v) for k, v in res.items()}}

def multi_stats(R):
    S = {}
    C = [r for r in R if r['kind'] == 'choice']
    if C:
        S['target_choice'] = fit_choice(C)
        S['target_choice_switches'] = fit_choice([c for c in C if c['switch']]) if sum(c['switch'] for c in C) >= 30 else None
    W = [r for r in R if r['kind'] == 'switch']
    if W:
        tr = [w for w in W if w['turned'] is not None]
        def share(sub): return [r2(sum(w['turned'] is not None for w in sub) / len(sub), 3), len(sub)] if sub else None
        S['second_attacker'] = {'n': len(W), 'turned_to_it': r2(len(tr) / len(W), 3), 'turn_s_median': r2(pct([w['turned'] for w in tr], .5)),
                                'turned_if_first_target_weak': share([w for w in W if w['b_vit'] < 0.4]), 'turned_if_first_target_strong': share([w for w in W if w['b_vit'] >= 0.8]),
                                'turned_if_new_one_nearer': share([w for w in W if w['c_dist'] < w['b_dist']]), 'turned_if_new_one_farther': share([w for w in W if w['c_dist'] >= w['b_dist']]),
                                'turned_if_new_one_in_view': share([w for w in W if w['c_ang'] < 45]), 'turned_if_new_one_behind': share([w for w in W if w['c_ang'] >= 90]),
                                'died_in_5s_if_turned': r2(sum(w['died5'] for w in tr) / len(tr), 3) if tr else None,
                                'died_in_5s_if_kept_on': r2(sum(w['died5'] for w in W if w['turned'] is None) / max(1, len(W) - len(tr)), 3)}
    P = [r for r in R if r['kind'] == 'pressure']
    if P:
        def ps(sub):
            if not sub: return None
            return {'n': len(sub), 'died': r2(sum(p['died'] for p in sub) / len(sub), 3), 'kills_per_stretch': r2(sum(p['kills'] for p in sub) / len(sub), 3),
                    'retreat_share': r2(pct([p['retreat_share'] for p in sub], .5), 3), 'retreated_mostly': r2(sum(p['retreat_share'] > 0.3 for p in sub) / len(sub), 3),
                    'dur_median': r2(pct([p['dur'] for p in sub], .5))}
        out2 = [p for p in P if p['level'] == 2]
        S['outnumbered'] = {'one_attacker': ps([p for p in P if p['level'] == 1]), 'two_plus': ps(out2),
                            'two_plus_retreated': ps([p for p in out2 if p['retreat_share'] > 0.3]), 'two_plus_stood': ps([p for p in out2 if p['retreat_share'] <= 0.3]),
                            'two_plus_with_mates_near': ps([p for p in out2 if p['mates'] > 0]), 'two_plus_alone': ps([p for p in out2 if p['mates'] <= 0])}
    J = [r for r in R if r['kind'] == 'join']
    if J:
        jj = [j for j in J if j['joined'] is not None]
        S['join_teammate'] = {'n': len(J), 'joined': r2(len(jj) / len(J), 3), 'join_s_median': r2(pct([j['joined'] for j in jj], .5)),
                              'joined_if_within_10': r2(sum(j['joined'] is not None for j in J if j['dist'] < 10) / max(1, sum(j['dist'] < 10 for j in J)), 3)}
    K = [r for r in R if r['kind'] == 'skirmish']
    if K:
        sz = Counter(min(6, k['size']) for k in K); sd = Counter('v'.join(str(x) for x in k['sides'][:3]) for k in K)
        S['skirmishes'] = {'n': len(K), 'size_share': {('%d+' % s if s == 6 else str(s)): r2(n / len(K), 3) for s, n in sorted(sz.items())},
                           'sides_top': {k: r2(n / len(K), 3) for k, n in sd.most_common(8)}}
    return S

def load():
    out = []
    for p in sorted(glob.glob(os.path.join(ENG, '*.eng.jsonl.gz'))):
        try:
            with gzip.open(p, 'rt') as f:
                for l in f:
                    try: out.append(json.loads(l))
                    except Exception: break
        except (EOFError, OSError): pass
    return out

def attack_rows(e):   # the rows from the first attack to the end
    t0 = e['attack_t']; return [r for r in e['rows'] if r[0] >= t0 - 1e-6]

def stats(D):
    S = {'duels': len(D)}
    if not D: return S
    dec = [e for e in D if e['out']['win'] in ('a', 'b')]
    S['outcome'] = dict(Counter(e['out']['win'] for e in D))
    S['first_attacker_wins'] = r2(sum(e['out']['win'] == 'a' for e in dec) / len(dec), 3) if dec else None
    # the loser never fired back (an ambush): of the decided duels
    def fired(e, who): return any(r[12 if who == 'a' else 13] for r in e['rows'])
    S['loser_never_fired'] = r2(sum(not fired(e, 'b' if e['out']['win'] == 'a' else 'a') for e in dec) / len(dec), 3) if dec else None
    # caught looking away (b's aim error when the attack began > 60 deg): how often, and how often b still wins
    away = [e for e in dec if e['ctx']['b_err'] > 60]; facing = [e for e in dec if e['ctx']['b_err'] <= 20]
    S['b_caught_looking_away'] = r2(len(away) / len(dec), 3) if dec else None
    S['b_wins_if_looking_away'] = r2(sum(e['out']['win'] == 'b' for e in away) / len(away), 3) if away else None
    S['b_wins_if_facing'] = r2(sum(e['out']['win'] == 'b' for e in facing) / len(facing), 3) if facing else None
    # vitality advantage at the start (shield + health, a minus b) vs a winning
    bins = defaultdict(lambda: [0, 0])
    for e in dec:
        c = e['ctx']; adv = (c['a_sh'] + c['a_hp']) - (c['b_sh'] + c['b_hp']); k = '<-0.5' if adv < -0.5 else '-0.5..-0.1' if adv < -0.1 else '-0.1..0.1' if adv <= 0.1 else '0.1..0.5' if adv <= 0.5 else '>0.5'
        bins[k][0] += e['out']['win'] == 'a'; bins[k][1] += 1
    S['a_wins_by_vitality_lead'] = {k: [r2(w / n, 3), n] for k, (w, n) in sorted(bins.items())}
    # numbers: a's teammates near minus b's
    nb = defaultdict(lambda: [0, 0])
    for e in dec:
        c = e['ctx']; k = max(-2, min(2, c['a_mates'] - c['b_mates'])); nb[k][0] += e['out']['win'] == 'a'; nb[k][1] += 1
    S['a_wins_by_mates_lead'] = {str(k): [r2(w / n, 3), n] for k, (w, n) in sorted(nb.items())}
    S['third_party'] = r2(sum(e['out']['win'] == 'third' for e in D) / len(D), 3)
    # distance when the attack began, by the attacker's weapon class
    byw = defaultdict(list)
    for e in D:
        if e['ctx']['a_veh']: continue
        byw[wcls(e['ctx']['a_wpn'])].append(e['ctx']['dist'])
    S['attack_distance_all'] = r2(pct([e['ctx']['dist'] for e in D if not e['ctx']['a_veh']], .5))
    S['attack_distance_by_weapon'] = {w: {'n': len(v), 'p25': r2(pct(v, .25)), 'median': r2(pct(v, .5)), 'p75': r2(pct(v, .75))} for w, v in sorted(byw.items(), key=lambda kv: -len(kv[1])) if len(v) >= 5}
    # time to kill (decided duels: from the first attack to the kill), by the winner's weapon class
    ttk = defaultdict(list)
    for e in dec:
        w = e['ctx']['a_wpn'] if e['out']['win'] == 'a' else e['ctx']['b_wpn']; ttk[wcls(w)].append(e['out']['dur'])
    S['time_to_kill_by_weapon'] = {w: {'n': len(v), 'median': r2(pct(v, .5)), 'p75': r2(pct(v, .75))} for w, v in sorted(ttk.items(), key=lambda kv: -len(kv[1])) if len(v) >= 5}
    # movement while attacking (on foot): strafe, closing, jumps, by shield state
    lat, rad_hi, rad_lo, rad_vs_broken, jump, fire, aim, rev = [], [], [], [], 0, [], [], []
    rows_n = 0
    for e in D:
        if e['ctx']['a_veh'] or e['ctx']['b_veh']: continue
        last = None
        for r in attack_rows(e):
            if r[2] is None: continue
            rows_n += 1; lat.append(abs(r[2]))
            (rad_lo if r[8] < 0.25 else rad_hi).append(r[3])
            if r[10] < 0.05 and r[11] < 1.0: rad_vs_broken.append(r[3])
            if r[14] is not None and r[14] > 2.5: jump += 1
            fire.append(r[12]); aim.append(r[6])
            sg = 1 if r[2] > 0.5 else -1 if r[2] < -0.5 else 0
            if sg and last and sg != last[0]: rev.append(r[0] - last[1])
            if sg and (not last or sg != last[0]): last = (sg, r[0])
    S['move'] = {'rows': rows_n, 'strafe_median': r2(pct(lat, .5)), 'strafe_rms': r2(math.sqrt(sum(x * x for x in lat) / len(lat))) if lat else None,
                 'strafe_switch_s': r2(pct(rev, .5)), 'closing_shield_up': r2(pct(rad_hi, .5)), 'closing_shield_low': r2(pct(rad_lo, .5)),
                 'closing_vs_broken_shield': r2(pct(rad_vs_broken, .5)), 'jumps_per_min': r2(jump / 3.0 / (rows_n / 600.0)) if rows_n else None,
                 'fire_share': r2(sum(fire) / len(fire), 3) if fire else None, 'aim_err_median': r2(pct(aim, .5)), 'aim_err_p75': r2(pct(aim, .75))}
    # retreats: a's shield < 0.2 while b's > 0.5, then a's closing speed < -1 for 0.5 s within the next 2 s
    ret, stay = Counter(), Counter()
    for e in dec:
        rr = attack_rows(e); k = next((i for i, r in enumerate(rr) if r[8] < 0.2 and r[10] > 0.5), None)
        if k is None: continue
        win = rr[k:k + 20]; back = sum(1 for r in win if r[3] is not None and r[3] < -1.0) >= 5
        (ret if back else stay)[e['out']['win']] += 1
    S['retreat_when_outshielded'] = {'retreat': dict(ret), 'stay': dict(stay), 'share': r2(sum(ret.values()) / max(1, sum(ret.values()) + sum(stay.values())), 3),
                                     'survive_if_retreat': r2(ret['b'] / max(1, sum(ret.values())), 3) if ret else None,
                                     'survive_if_stay': r2(stay['b'] / max(1, sum(stay.values())), 3) if stay else None}
    # reaction: b's time from the start of the awareness window to aiming within 8 deg (b was attacked first)
    rx = []
    for e in D:
        for r in e['rows']:
            if r[7] < 8: rx.append(r[0]); break
    S['b_turns_to_aim_s'] = {'median': r2(pct(rx, .5)), 'p75': r2(pct(rx, .75)), 'n': len(rx)}
    # target choice: when a attacked, other enemies within 30 units of a (a_foes includes b? no: others) -> nearest?
    S['others_near_when_attacking'] = r2(sum(e['ctx']['a_foes'] > 0 for e in D) / len(D), 3)
    return S

def library(D, rnd):
    """2-second pieces (20 rows) of real fights: the attacker's (lat, rad) every 0.1 s, from duels it won or left alive."""
    lib = defaultdict(list)
    for e in D:
        if e['ctx']['a_veh'] or e['ctx']['b_veh'] or e['out']['win'] not in ('a', 'none', 'a?'): continue
        rr = [r for r in attack_rows(e) if r[2] is not None]
        if len(rr) < 20: continue
        w = wcls(e['ctx']['a_wpn']);
        for s in range(0, len(rr) - 19, 10):
            seg = rr[s:s + 20]
            if seg[-1][0] - seg[0][0] > 2.6: continue   # (gaps)
            d = seg[0][1]; band = 'close' if d < 6 else 'mid' if d < 15 else 'far'
            lib[(w, band)].append({'d': round(d, 1), 'lat': [r[2] for r in seg], 'rad': [r[3] for r in seg], 'sh': round(seg[0][8], 2)})
    out = {}
    for k, v in lib.items():
        rnd.shuffle(v); out['%s/%s' % k] = v[:60]
    return out

def main():
    R = load(); D = [e for e in R if e.get('kind', 'duel') == 'duel']
    if not D: print('no duels in %s (shim/tools/engage-extract.py first)' % ENG); sys.exit(1)
    rep = {'source': {'duels': len(D), 'recordings': len({e['file'] for e in D}), 'maps': dict(Counter(e['map'] for e in D))}, 'by_class': {}}
    for c in ('slayer', 'ctf', 'infection', 'other'):
        sub = [e for e in D if e['class'] == c]
        if sub: rep['by_class'][c] = stats(sub)
        msub = [r for r in R if r.get('kind', 'duel') != 'duel' and r['class'] == c]
        if msub: rep['by_class'].setdefault(c, {})['multi'] = multi_stats(msub)
    rep['all_but_infection'] = stats([e for e in D if e['class'] != 'infection'])
    rep['all_but_infection']['multi'] = multi_stats([r for r in R if r.get('kind', 'duel') != 'duel' and r['class'] != 'infection'])
    M = [e for e in D if e['class'] in MODEL_CLASSES]
    st = stats(M) if M else {}
    model = {'version': 1, 'source': {'duels': len(M), 'classes': sorted(MODEL_CLASSES)},
             'distance_by_weapon': st.get('attack_distance_by_weapon', {}), 'move': st.get('move', {}),
             'retreat': st.get('retreat_when_outshielded', {}), 'ttk': st.get('time_to_kill_by_weapon', {}),
             'library': library(M, random.Random(11))}
    mm = multi_stats([r for r in R if r.get('kind', 'duel') != 'duel' and r['class'] in MODEL_CLASSES])
    model['target'] = (mm.get('target_choice') or {}).get('weights'); model['multi'] = {k: v for k, v in mm.items() if k != 'target_choice'}
    json.dump(model, open(OUT, 'w'), separators=(',', ':'))
    if '--json' in sys.argv: json.dump(rep, open(arg('--json', ''), 'w'), indent=1)
    print(json.dumps(rep, indent=1))
    print('model -> %s (%d library pieces in %d groups)' % (OUT, sum(len(v) for v in model['library'].values()), len(model['library'])))

if __name__ == '__main__':
    main()
