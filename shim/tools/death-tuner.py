#!/usr/bin/env python3
"""Tune a walker from WHY it dies (2026-11-09; the user: "walker needs to focus hard on its K/D spread and tweak settings until it gets
better, I feel like a per death why did the other player beat me calculation could be interesting, weighting future growth").

The walker writes one line per death to battles/follow/why-<nickname>.jsonl (nbot.c why_note: the likely killer and ONE cause --
empty, outnumbered, unseen, point blank, traded, entered hurt, slow start, outranged, outshot). This loop:
  - weighs the causes, the newest most (each death's weight halves every HALF deaths: "weighting future growth"),
  - every STEP deaths takes ONE step on a setting for the heaviest cause (each cause has its own list of settings and directions,
    tried in turn, inside bounds) in the walker's live knob file (battles/live-knobs/<nickname>.json: it reads it within a second),
  - judges the last step on the walker's own K/D (its stats page, /stats.json): the STEP deaths after it against the STEP deaths
    before; 25%+ worse -> the setting goes back and that move rests for 30 deaths; else it stays.
"traded" deaths (the killer nearly dead too) take no step.
LEAD (11:24, the user: "classic inability to lead a shot, and predict the path of the head"): a second track on the SERVER's
accuracy (the scoreboard's hits / shots, the walker's /stats.json): every LEAD_SHOTS (250) shots the lead multiplier
NHOOK_BOT_LEAD_X moves one step (0.25, 0..3) in its current direction; the next window keeps it unless accuracy fell 3+ points
(then it goes back and the direction turns). The walker tab's "Self-tune from deaths" switch is NHOOK_TUNE in the same
file (0 = paused). Steps and verdicts -> battles/follow/tuner-<nickname>.jsonl and stdout.
    python3 shim/tools/death-tuner.py --nick AIWalkerTest --stats http://127.0.0.1:8771 [--step 6] [--half 20] [--dry]
"""
import json, os, sys, time, urllib.request

def arg(k, d):
    a = sys.argv; return a[a.index(k) + 1] if k in a else d
NICK = arg('--nick', 'AIWalkerTest'); STATS = arg('--stats', 'http://127.0.0.1:8771'); STEP = int(arg('--step', '6')); HALF = float(arg('--half', '20'))
DRY = '--dry' in sys.argv
SUP = os.path.expanduser('~/Library/Application Support/ReclaimerMac')
WHY = os.path.join(SUP, 'battles', 'follow', 'why-%s.jsonl' % NICK)
KNOBS = os.path.join(SUP, 'battles', 'live-knobs', '%s.json' % NICK)
LOG = os.path.join(SUP, 'battles', 'follow', 'tuner-%s.jsonl' % NICK)

# cause -> [(knob, step, lo, hi, the code's default)] -- tried in turn; a step moves the CURRENT value (file, else the default)
MOVES = {
    'ignored attacker': [('NHOOK_BOT_SECOND_P', +0.15, 0.3, 1.0, 0.5), ('NHOOK_BOT_TURNX', -0.1, 0.1, 1.0, 0.5)],
    'slow start':   [('NHOOK_BOT_REACT', -0.1, 0.2, 2.5, 1.6), ('NHOOK_BOT_TURN0', +0.5, 0.5, 6.0, 0.73), ('NHOOK_BOT_SEE_NEAR', +1.0, 2.0, 8.0, 2.5)],
    'unseen':       [('NHOOK_U_COVER', +5, 0, 95, 80), ('NHOOK_BOT_COVER_DROP', -0.05, 0.15, 0.6, 0.35), ('NHOOK_BOT_HUNT_KEEP', -1.0, 1.0, 10.0, 5.0)],
    'outnumbered':  [('NHOOK_BOT_COVER_SH', +0.1, 0.3, 0.9, 0.5), ('NHOOK_BOT_RETREAT', +0.1, 0.0, 0.6, 0.35), ('NHOOK_BOT_CLOSE', -0.1, 0.0, 1.0, 0.4)],
    'outshot':      [('NHOOK_BOT_NOISE', -0.03, 0.03, 2.0, 1.0), ('NHOOK_BOT_AIMLEAD', +0.01, 0.0, 0.12, 0.0), ('NHOOK_BOT_FIRE_EXTRA', -0.25, 0.0, 3.0, 2.0)],
    'outranged':    [('NHOOK_BOT_HUNT_WEAPON_X', -0.1, 1.0, 2.0, 1.3), ('NHOOK_BOT_FIGHT_SWITCH', +1, 0, 1, 1)],
    'empty':        [('NHOOK_U_FETCH', +5, 20, 70, 45), ('NHOOK_U_FETCH_GAIN', +1, 2, 12, 6)],
    'point blank':  [('NHOOK_BOT_MELEE_RACE', +0.2, 1.0, 3.0, 1.8), ('NHOOK_BOT_DIST', +0.1, 0.5, 2.5, 1.0)],
    'entered hurt': [('NHOOK_U_RECHARGE', +10, 0, 80, 65), ('NHOOK_BOT_RECHARGE_SH', +0.1, 0.3, 0.9, 0.6)],
}

def log(rec):
    rec['t'] = time.time(); print(time.strftime('%H:%M:%S'), json.dumps(rec), flush=True)
    if DRY: return
    os.makedirs(os.path.dirname(LOG), exist_ok=True)
    with open(LOG, 'a') as f: f.write(json.dumps(rec) + '\n')
def knobs_read():
    try: return json.load(open(KNOBS))
    except Exception: return {}
def knobs_write(d):
    if DRY: return
    t = KNOBS + '.tuner.tmp'; open(t, 'w').write(json.dumps(d) + '\n'); os.replace(t, KNOBS)
def you():
    try:
        d = json.load(urllib.request.urlopen(STATS + '/stats.json', timeout=4))
        for p in d.get('players', []):
            if p.get('you'): return int(p.get('kills') or 0), int(p.get('deaths') or 0), int(p.get('shots') or 0), int(p.get('hits') or 0), d.get('map') or ''
    except Exception: pass
    return None
LEAD_SHOTS = int(arg('--lead-shots', '250'))

def main():
    off = os.path.getsize(WHY) if os.path.exists(WHY) else 0   # (only deaths from now on)
    weights = {}; rest = {}; turn = {}; deaths = 0; since_step = 0
    K = D = SH = HI = 0; last = None   # the walker's kills / deaths / shots / hits since the start, across matches
    lead = {'dir': 1, 's0': 0, 'h0': 0, 'pending': None, 'acc_before': None, 'map': None}
    MAP = None   # (12:05: a window that spans a map change is no evidence -- Valhalla plays far worse than The Pit: no verdict, start again)
    hist = []                # (deaths, kills) after each death this loop saw
    pending = None           # the last step: {knob, old, new, cause, d0, k0, before}
    log({'event': 'start', 'nick': NICK, 'step': STEP, 'half': HALF, 'dry': DRY})
    while True:
        time.sleep(5)
        y = you()
        if y:
            if last and (y[0] < last[0] or y[1] < last[1] or y[2] < last[2]): last = (0, 0, 0, 0, last[4])   # (a new match: the counters started again)
            MAP = y[4]
            if last: K += max(0, y[0] - last[0]); D += max(0, y[1] - last[1]); SH += max(0, y[2] - last[2]); HI += max(0, y[3] - last[3])
            last = y
        # the lead track: one window of LEAD_SHOTS server-counted shots
        if lead['map'] != MAP:
            if lead['pending'] or lead['acc_before'] is not None: log({'event': 'lead window dropped', 'why': 'map %s -> %s' % (lead['map'], MAP)})
            lead.update({'s0': SH, 'h0': HI, 'acc_before': None, 'map': MAP})   # (a pending step stays; it is judged on this map's windows)
        if SH - lead['s0'] >= LEAD_SHOTS and knobs_read().get('NHOOK_TUNE', 1) > 0:
            acc = (HI - lead['h0']) / max(1, SH - lead['s0']); kn = knobs_read(); pd = lead['pending']
            if pd:
                worse = lead['acc_before'] is not None and acc < lead['acc_before'] - 0.03
                if worse:
                    if pd['old'] is None: kn.pop('NHOOK_BOT_LEAD_X', None)
                    else: kn['NHOOK_BOT_LEAD_X'] = pd['old']
                    knobs_write(kn); lead['dir'] = -lead['dir']
                log({'event': 'lead verdict', 'knob': 'NHOOK_BOT_LEAD_X', 'from': pd['old'], 'to': pd['new'], 'acc_before': lead['acc_before'], 'acc_after': round(acc, 3), 'kept': not worse})
                lead['acc_before'] = lead['acc_before'] if worse else round(acc, 3); lead['pending'] = None
            else:
                lead['acc_before'] = round(acc, 3)
                kn = knobs_read(); cur = kn.get('NHOOK_BOT_LEAD_X', 1.0); new = round(min(3.0, max(0.0, cur + 0.25 * lead['dir'])), 3)
                if new == cur: lead['dir'] = -lead['dir']; new = round(min(3.0, max(0.0, cur + 0.25 * lead['dir'])), 3)
                old = kn.get('NHOOK_BOT_LEAD_X'); kn['NHOOK_BOT_LEAD_X'] = new; knobs_write(kn); lead['pending'] = {'old': old, 'new': new}
                log({'event': 'lead step', 'knob': 'NHOOK_BOT_LEAD_X', 'from': cur, 'to': new, 'acc_before': lead['acc_before'], 'shots': SH - lead['s0']})
            lead['s0'] = SH; lead['h0'] = HI
        try: sz = os.path.getsize(WHY)
        except OSError: continue
        if sz < off: off = 0
        if sz == off: continue
        with open(WHY) as f: f.seek(off); new = f.read(); off = f.tell()
        for line in new.splitlines():
            try: r = json.loads(line)
            except Exception: continue
            c = r.get('cause', '?'); deaths += 1; since_step += 1
            for k in weights: weights[k] *= 0.5 ** (1.0 / HALF)
            weights[c] = weights.get(c, 0) + 1.0
            for k in list(rest):
                rest[k] -= 1
                if rest[k] <= 0: del rest[k]
            hist.append((deaths, K))   # (deaths counted from the why lines, kills from the stats page)
        kn = knobs_read()
        if kn.get('NHOOK_TUNE', 1) <= 0: continue
        # the verdict on the last step: the STEP deaths since it against the STEP deaths before it
        if pending and pending.get('map') != MAP:
            log({'event': 'verdict dropped', 'knob': pending['knob'], 'why': 'map %s -> %s (the step stays)' % (pending.get('map'), MAP)}); pending = None
        if pending and deaths - pending['d0'] >= STEP:
            dk = K - pending['k0']; dd = max(1, deaths - pending['d0']); after = dk / dd
            before = pending['before']; worse = before is not None and before > 0 and after < 0.75 * before
            if worse:
                kn = knobs_read()
                if pending['old'] is None: kn.pop(pending['knob'], None)
                else: kn[pending['knob']] = pending['old']
                knobs_write(kn); rest[(pending['cause'], pending['knob'])] = 30
            log({'event': 'verdict', 'knob': pending['knob'], 'cause': pending['cause'], 'from': pending['old'], 'to': pending['new'],
                 'kd_before': before, 'kd_after': round(after, 2), 'kept': not worse})
            pending = None
        if pending or since_step < STEP: continue
        # the next step: the heaviest cause that has a move
        order = sorted(((w, c) for c, w in weights.items() if c in MOVES), reverse=True)
        for w, c in order:
            moves = MOVES[c]; i0 = turn.get(c, 0); done = False
            for j in range(len(moves)):
                knob, st, lo, hi, dflt = moves[(i0 + j) % len(moves)]
                if (c, knob) in rest: continue
                cur = kn.get(knob, dflt); new = round(min(hi, max(lo, cur + st)), 3)
                if new == cur: continue
                # K/D of the STEP deaths before this step (the walker's own counters)
                before = None
                if len(hist) > STEP:
                    d_a, k_a = hist[-STEP - 1]; d_b, k_b = hist[-1]
                    before = round((k_b - k_a) / max(1, d_b - d_a), 2) if d_b > d_a else None
                old = kn.get(knob); kn[knob] = new; knobs_write(kn)
                pending = {'knob': knob, 'old': old, 'new': new, 'cause': c, 'd0': deaths, 'k0': K, 'D0': D, 'before': before, 'map': MAP}
                turn[c] = (i0 + j + 1) % len(moves); since_step = 0; done = True
                log({'event': 'step', 'cause': c, 'weight': round(w, 2), 'knob': knob, 'from': cur, 'to': new, 'kd_before': before,
                     'causes': {k: round(v, 2) for k, v in sorted(weights.items(), key=lambda x: -x[1])}})
                break
            if done: break

if __name__ == '__main__':
    try: main()
    except KeyboardInterrupt: pass
