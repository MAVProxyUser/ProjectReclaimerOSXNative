#!/usr/bin/env python3
"""The brain monitor (2026-11-09; the user: "there should be a regular brain reload monitoring session in the work board tabs so this
doesn't happen again. there should be per map, and periodic, and other conditional settings that auto trigger new brains. bad k/d
spread is one, bad accuracy deviations, bad shots landed, etc, the obvious things you can derive that after a certain threshold auto
trigger relearning and reset the levers until they get tripped accordingly again").

A walker's brain revises itself (nbot_goals.c brain_review): on every map change and every NHOOK_BOT_REVIEW_S (180 s). This loop
watches the walker's scoreboard (its stats page /stats.json, every 10 s) and TRIPS a trigger when a number goes bad:
  spread    kills - deaths over the last 5 minutes <= -SPREAD (6)                 re-arms at >= -2
  kd        K/D over its last 10 deaths < half the lobby's median K/D (and < 0.6)  re-arms at >= 0.8 x the lobby's
  accuracy  hits / shots over its last 150 shots < the lobby's median - 0.08, or < its own match accuracy - 0.10
                                                                                   re-arms 0.05 above that limit
  hits      hits landed a minute over the last 3 minutes < half its match rate (5+ a minute)   re-arms at >= 0.8 x
  deaths    deaths a minute over the last 3 minutes > 2.5                          re-arms at < 1.5
  stale     in a match and no brain revision for 2 review periods + 30 s           re-arms when one comes
A trip (at most one every COOL, 90 s; a tripped trigger waits until it re-arms -- the hysteresis keeps it from firing every 10 s):
  1. RELEARN: battles/live-knobs/<nick>.review = "<seq> <reason>" -> the brain revises at once (its death map read again, its path
     penalties rebuilt from its own recent deaths, its caution from the window) -- "[brain] review r<N> [<reason>]",
  2. RESET THE LEVERS (spread, kd, accuracy, hits): the trip record says reset; shim/tools/death-tuner.py puts the settings it moves
     back to its last good set (taken after a window of deaths that went well) and steps on from there.
The counters start again with each map / match (the triggers compare inside one match: maps differ too much).
-> battles/follow/monitor-<nick>.jsonl (trips, re-arms, maps) and monitor-<nick>.json (the state: numbers, thresholds, armed or not,
the brain's build and lesson, the last revision) -- the work board's Brain tab.
    python3 shim/tools/brain-monitor.py [--nick AIWalkerTest] [--stats http://127.0.0.1:8771] [--log <walker log>] [--dry] [--once]"""
import json, os, statistics, sys, time, urllib.request

def arg(k, d):
    a = sys.argv; return a[a.index(k) + 1] if k in a else d
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__))); import walkerrec
NICK = arg('--nick', '') or walkerrec.pubwalker_nick(); STATS = arg('--stats', 'http://127.0.0.1:8771'); DRY = '--dry' in sys.argv or '--once' in sys.argv
SUP = os.path.expanduser('~/Library/Application Support/ReclaimerMac')
WLOG = arg('--log', os.path.join(SUP, 'battles', 'aiwalker.log'))
FOL = os.path.join(SUP, 'battles', 'follow'); KN = os.path.join(SUP, 'battles', 'live-knobs')
OUT = os.path.join(FOL, 'monitor-%s.jsonl' % NICK); STATE = os.path.join(FOL, 'monitor-%s.json' % NICK)
REVIEWS = os.path.join(FOL, 'review-%s.jsonl' % NICK); SIGNAL = os.path.join(KN, '%s.review' % NICK); KNOBS = os.path.join(KN, '%s.json' % NICK)
SPREAD = float(arg('--spread', '6')); COOL = float(arg('--cool', '90')); EVERY = 10

def log(rec):
    rec['t'] = time.time(); print(time.strftime('%H:%M:%S'), json.dumps(rec), flush=True)
    if not DRY:
        with open(OUT, 'a') as f: f.write(json.dumps(rec) + '\n')
def write_json(path, obj):
    if DRY: return
    t = path + '.tmp'; json.dump(obj, open(t, 'w')); os.replace(t, path)
def stats():
    try: return json.load(urllib.request.urlopen(STATS + '/stats.json', timeout=4))
    except Exception: return None
def review_period():
    try: return float(json.load(open(KNOBS)).get('NHOOK_BOT_REVIEW_S', 180))
    except Exception: return 180.0
def last_review():
    try:
        with open(REVIEWS, 'rb') as f:
            f.seek(max(0, os.path.getsize(REVIEWS) - 4096)); lines = f.read().decode(errors='replace').splitlines()
        return json.loads(lines[-1]) if lines else None
    except Exception: return None
_brain = {'t': 0, 'v': {}}
def brain_info():   # the loaded brain's build and lesson: the walker log's last "[brain] code built" / "[brain] lesson:" lines
    if time.time() - _brain['t'] < 30: return _brain['v']
    v = {}
    try:
        with open(WLOG, 'rb') as f:
            f.seek(max(0, os.path.getsize(WLOG) - 3_000_000)); tail = f.read().decode(errors='replace').splitlines()
        for l in reversed(tail):
            if 'built' not in v and '[brain] code built' in l: v['built'] = l.split('code built', 1)[1].replace('is stepping', '').strip()
            if 'lesson' not in v and '[brain] lesson:' in l: v['lesson'] = l.split('lesson:', 1)[1].strip()
            if len(v) == 2: break
    except OSError: pass
    _brain.update(t=time.time(), v=v); return v
def signal(reason):
    try: seq = int(open(SIGNAL).read().split()[0]) + 1
    except Exception: seq = 1
    if not DRY:
        t = SIGNAL + '.tmp'; open(t, 'w').write('%d %s\n' % (seq, reason[:60])); os.replace(t, SIGNAL)
    return seq

def at_or_before(S, key, target):   # the newest sample whose key is <= target (S oldest first)
    best = None
    for s in S:
        if s[key] <= target: best = s
        else: break
    return best
def at_time(S, t):
    return at_or_before(S, 't', t)

def main():
    S = []; mp = None; armed = {}; last_trip = 0; tripped_at = {}
    log({'event': 'start', 'nick': NICK, 'spread': SPREAD, 'cool': COOL, 'dry': DRY})
    while True:
        d = stats(); now = time.time()
        me = next((p for p in (d or {}).get('players', []) if p.get('you')), None) if d else None
        if me:
            k, dd, sh, hi = int(me.get('kills') or 0), int(me.get('deaths') or 0), int(me.get('shots') or 0), int(me.get('hits') or 0)
            m = d.get('map') or ''
            if m != mp or (S and (k < S[-1]['k'] or dd < S[-1]['d'] or sh < S[-1]['s'])):   # (a new map / match: the counters start again)
                if m != mp: log({'event': 'map', 'map': m, 'from': mp})
                S = []; mp = m; armed = {}
            S.append({'t': now, 'k': k, 'd': dd, 's': sh, 'h': hi}); S = S[-2000:]
            others = [p for p in d.get('players', []) if not p.get('you')]
            lobby_acc = statistics.median([p['hits'] / p['shots'] for p in others if (p.get('shots') or 0) >= 50]) if sum(1 for p in others if (p.get('shots') or 0) >= 50) >= 3 else None
            lobby_kd = statistics.median([(p.get('kills', 0) + 1) / (p.get('deaths', 0) + 1) for p in others if (p.get('deaths') or 0) >= 3]) if sum(1 for p in others if (p.get('deaths') or 0) >= 3) >= 3 else None
            cur = S[-1]; M = {'map': m, 'in_match': bool(d.get('in_match')), 'kills': k, 'deaths': dd, 'shots': sh, 'hits': hi, 'lobby_acc': lobby_acc, 'lobby_kd': lobby_kd}
            a5 = at_time(S, now - 300); M['spread5'] = (cur['k'] - a5['k']) - (cur['d'] - a5['d']) if a5 and now - S[0]['t'] >= 240 else None
            a10 = at_or_before(S, 'd', dd - 10); M['kd10'] = round((cur['k'] - a10['k'] + 0.5) / (cur['d'] - a10['d'] + 0.5), 2) if a10 and dd >= 10 else None
            a150 = at_or_before(S, 's', sh - 150); M['acc150'] = round((cur['h'] - a150['h']) / max(1, cur['s'] - a150['s']), 3) if a150 and sh >= 150 else None
            M['acc_match'] = round(hi / sh, 3) if sh >= 150 else None
            a3 = at_time(S, now - 180); span = now - S[0]['t']
            M['hpm3'] = round((cur['h'] - a3['h']) / ((now - a3['t']) / 60), 1) if a3 and now - a3['t'] >= 150 else None
            M['hpm_match'] = round((a3['h'] - S[0]['h']) / max(1e-6, (a3['t'] - S[0]['t']) / 60), 1) if a3 and a3['t'] - S[0]['t'] >= 180 else None
            M['dpm3'] = round((cur['d'] - a3['d']) / ((now - a3['t']) / 60), 2) if a3 and now - a3['t'] >= 150 else None
            rv = last_review(); per = review_period(); M['review_age'] = round(now - rv['t']) if rv else None
            # the triggers: (name, value, tripped?, re-armed?, threshold words, reset the levers?)
            T = []
            T.append(('spread', M['spread5'], M['spread5'] is not None and M['spread5'] <= -SPREAD, M['spread5'] is not None and M['spread5'] >= -2, '<= -%g in 5 min' % SPREAD, True))
            if M['kd10'] is not None and lobby_kd:
                T.append(('kd', M['kd10'], M['kd10'] < min(0.6, 0.5 * lobby_kd), M['kd10'] >= 0.8 * lobby_kd, '< %.2f (half the lobby\'s %.2f)' % (min(0.6, 0.5 * lobby_kd), lobby_kd), True))
            if M['acc150'] is not None:
                cands = [x for x in ((lobby_acc - 0.08) if lobby_acc else None, (M['acc_match'] - 0.10) if M['acc_match'] else None) if x is not None]
                if cands:   # (either deviation trips: the higher of the two limits)
                    lim = max(cands)
                    T.append(('accuracy', M['acc150'], M['acc150'] < lim, M['acc150'] >= lim + 0.05, '< %.3f (lobby %s - 0.08, its match %s - 0.10)' % (lim, '%.3f' % lobby_acc if lobby_acc else '-', M['acc_match']), True))
            if M['hpm3'] is not None and M['hpm_match'] and M['hpm_match'] >= 5:
                T.append(('hits', M['hpm3'], M['hpm3'] < 0.5 * M['hpm_match'], M['hpm3'] >= 0.8 * M['hpm_match'], '< %.1f a minute (half its match rate %.1f)' % (0.5 * M['hpm_match'], M['hpm_match']), True))
            if M['dpm3'] is not None:
                T.append(('deaths', M['dpm3'], M['dpm3'] > 2.5, M['dpm3'] < 1.5, '> 2.5 a minute', False))
            if M['in_match'] and span > 2 * per + 30:
                stale = M['review_age'] is None or M['review_age'] > 2 * per + 30
                T.append(('stale', M['review_age'], stale, not stale, 'no revision for %d s' % (2 * per + 30), False))
            trig = []
            for name, val, bad, ok, words, reset in T:
                was = armed.get(name, True)
                if not was and ok: armed[name] = True; log({'event': 're-armed', 'trigger': name, 'value': val, 'map': m})
                fire = armed.get(name, True) and bad and now - last_trip >= COOL
                if fire:
                    armed[name] = False; last_trip = now; tripped_at[name] = now
                    reason = '%s %s %s' % (name, val, words.split(' (')[0])
                    seq = signal(reason)
                    log({'event': 'trip', 'trigger': name, 'value': val, 'threshold': words, 'reset': reset, 'seq': seq, 'map': m, 'kills': k, 'deaths': dd})
                trig.append({'name': name, 'value': val, 'threshold': words, 'armed': armed.get(name, True), 'bad': bool(bad), 'resets': reset, 'last_trip': tripped_at.get(name)})
            write_json(STATE, {'t': now, 'nick': NICK, 'metrics': M, 'triggers': trig, 'brain': brain_info(), 'review': rv, 'review_period': per, 'cool': COOL})
        else:
            write_json(STATE, {'t': now, 'nick': NICK, 'metrics': None, 'triggers': [], 'brain': brain_info(), 'review': last_review(), 'note': 'the walker is not in a game (no scoreboard)'})
        if '--once' in sys.argv:   # (a test: one look, the state printed, nothing written)
            print(json.dumps({'metrics': (M if me else None), 'triggers': (trig if me else []), 'brain': brain_info(), 'review': last_review()}, indent=1)); return
        time.sleep(EVERY)

if __name__ == '__main__':
    try: main()
    except KeyboardInterrupt: pass
