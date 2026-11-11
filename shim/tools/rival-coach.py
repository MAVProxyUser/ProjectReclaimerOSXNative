#!/usr/bin/env python3
"""The walkers train on the player they fight (2026-11-09; the user, playing 3 walkers on our Guardian server: "I want them to continue the
every 3 minute train + brain reload vs me on this level. and aggressively try to get better than me by training on me vs the entire
collection corpus + corrections in playing me").

Every --every (180 s) while the battle runs:
  1. this battle's recording (the server's view, match-recorder.py -> battles/tracks/<time>_<map>_local.jsonl.gz) cut into fights
     (engage-extract.py into a scratch folder: only this file),
  2. the RIVAL's measures (coach.py: closing in, aim error, time to aim back, strafing, jumps, grenades, radar turns, fight distance)
     from it, blended 2 : 1 with the corpus WINNERS' (the top quarter of real players of this game class on this map -- walker-coach.py
     winners(), computed ONCE at background priority and cached: battles/coach/winners-<map>-<class>.json),
  3. each walker's own measures in the battle -> walker-coach.py steps() (half the gap a round, at most x2 / x0.5; a "better"
     measure -- aim error, time to aim back, radar turns -- moves only while the walker is BEHIND) -> written into the walker's LIVE
     settings file (battles/live-knobs/<nick>.json, NHOOK_BOT_<knob>): it plays with them within a second -- no restart,
  4. one line per walker: its K/D, its kills on / deaths to the rival this battle, the changes -> battles/coach/rival.jsonl + stdout.
The rival's deaths-to-it weighting is the death tuner's (--rival). No player names are written to files: the rival is "rival".
    python3 shim/tools/rival-coach.py --samples <battle samples .jsonl> --rec <battle recording .jsonl.gz> --rival <name>
                                      [--map guardian] [--class slayer] [--every 180] [--once]"""
import gzip, importlib.util, json, os, shutil, subprocess, sys, tempfile, time

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location('walker_coach', os.path.join(HERE, 'walker-coach.py')); wc = importlib.util.module_from_spec(spec)
_a = sys.argv; sys.argv = sys.argv[:1]; spec.loader.exec_module(wc); sys.argv = _a
coach = wc.coach; SUP = coach.SUP
def arg(k, d=None):
    a = sys.argv; return a[a.index(k) + 1] if k in a and a.index(k) + 1 < len(a) else d
SAMPLES, REC, RIVAL = arg('--samples'), arg('--rec'), arg('--rival', '')
MAP, CLS, EVERY = arg('--map', 'guardian'), arg('--class', 'slayer'), float(arg('--every', '180'))
KN = os.path.join(SUP, 'battles', 'live-knobs'); CO = os.path.join(SUP, 'battles', 'coach'); OUT = os.path.join(CO, 'rival.jsonl')

def log(rec):
    rec['t'] = time.time(); print(time.strftime('%H:%M:%S'), json.dumps(rec), flush=True)
    os.makedirs(CO, exist_ok=True)
    with open(OUT, 'a') as f: f.write(json.dumps(rec) + '\n')

def corpus_winners():
    """the corpus winners' measures for MAP / CLS, cached (the engagement library is big: loaded once, at background priority)"""
    cp = os.path.join(CO, 'winners-%s-%s.json' % (MAP, CLS))
    try:
        j = json.load(open(cp))
        if time.time() - j.get('t', 0) < 24 * 3600: return {k: tuple(v) for k, v in j['W'].items()}, j['desc']
    except Exception: pass
    try: os.setpriority(os.PRIO_PROCESS, 0, 20)
    except Exception: pass
    from collections import defaultdict
    # (2026-11-09: only this map's recordings -- the whole library is GBs in memory, and the user's game lagged beside walkers; the
    #  winners are this map's anyway when it has 40+ player-matches)
    import glob
    R = []
    for p in sorted(glob.glob(os.path.join(SUP, 'tracks', 'engagements', '*_%s_*.eng.jsonl.gz' % MAP))):
        try:
            with gzip.open(p, 'rt') as f:
                for l in f:
                    try: R.append(json.loads(l))
                    except Exception: break
        except (EOFError, OSError): pass
    by_file = defaultdict(list)
    for e in R:
        if e.get('class') != 'infection': by_file[e['file']].append(e)
    W, desc, _ = wc.winners(R, by_file, CLS, MAP)
    if W:
        os.makedirs(CO, exist_ok=True); t = cp + '.tmp'; json.dump({'t': time.time(), 'W': W, 'desc': desc}, open(t, 'w')); os.replace(t, cp)
    return W, desc

def slots():
    """slot -> name from the battle samples' newest line (the server's view)"""
    last = None
    for l in open(SAMPLES):
        if l.strip(): last = l
    d = json.loads(last) if last else {}
    return {p['slot']: p.get('name') for p in d.get('players') or [] if p.get('slot') is not None}, {p.get('name'): p for p in d.get('players') or []}

def battle_measures():
    tmp = tempfile.mkdtemp(prefix='rival-'); os.symlink(os.path.abspath(REC), os.path.join(tmp, os.path.basename(REC)))
    try:
        subprocess.run(['taskpolicy', '-b', sys.executable, os.path.join(HERE, 'engage-extract.py'), '--tracks', tmp, '--out', os.path.join(tmp, 'eng'), '--force', '--quiet'],
                       check=False, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        from collections import defaultdict
        B = []; import glob
        for p in glob.glob(os.path.join(tmp, 'eng', '*.eng.jsonl.gz')):
            with gzip.open(p, 'rt') as f:
                for l in f:
                    try: B.append(json.loads(l))
                    except Exception: break
        bfile = defaultdict(list)
        for e in B: bfile[e['file']].append(e)
        return bfile, sorted(bfile)
    finally: shutil.rmtree(tmp, ignore_errors=True)

def live_read(nick):
    try: return json.load(open(os.path.join(KN, nick + '.json')))
    except Exception: return {}
def live_write(nick, d):
    p = os.path.join(KN, nick + '.json'); t = p + '.rival.tmp'; open(t, 'w').write(json.dumps(d) + '\n'); os.replace(t, p)

def round_(Wc, wdesc):
    bfile, files = battle_measures()
    if not files: log({'event': 'no fights yet'}); return
    who, byname = slots()
    rs = [s for s, n in who.items() if n == RIVAL]
    if not rs: log({'event': 'the rival is not in the battle'}); return
    U = coach.measures([(f, rs[0]) for f in files], bfile)
    W = wc.blend([(U, 2.0)] + ([(Wc, 1.0)] if Wc else []))
    rk = byname.get(RIVAL) or {}
    for sl, nick in sorted(who.items()):
        if nick not in wc.NICKS: continue
        Y = coach.measures([(f, sl) for f in files], bfile)
        cur = live_read(nick); kn = {n: float(cur.get('NHOOK_BOT_' + n, v[0])) for n, v in wc.KNOBS.items()}
        ch = wc.steps(Y, W, kn, same_map=True)
        if ch:
            d = live_read(nick)
            for n, a, b, why in ch: d['NHOOK_BOT_' + n] = round(b, 3)
            live_write(nick, d)
        p = byname.get(nick) or {}; kd = Y.get('kd', (None, 0))[0]; dw = Y.get('win', (None, 0))
        log({'event': 'round', 'walker': nick, 'kills': p.get('kills'), 'deaths': p.get('deaths'), 'rival_kills': rk.get('kills'), 'rival_deaths': rk.get('deaths'),
             'duels_won': dw[0] and round(dw[0], 3), 'duels': dw[1], 'changes': [[n, round(a, 3), round(b, 3), w] for n, a, b, w in ch],
             'teacher': 'the rival x2 + ' + (wdesc if Wc else 'no corpus winners')})

def main():
    if not (SAMPLES and REC and RIVAL): sys.exit(__doc__)
    Wc, wdesc = corpus_winners()
    log({'event': 'start', 'map': MAP, 'class': CLS, 'corpus': wdesc, 'every': EVERY})
    while True:
        if os.path.exists(REC) and time.time() - os.path.getmtime(REC) > 300: log({'event': 'the battle recording stopped: done'}); return
        round_(Wc, wdesc)
        if '--once' in sys.argv: return
        time.sleep(EVERY)

if __name__ == '__main__':
    try: main()
    except KeyboardInterrupt: pass
