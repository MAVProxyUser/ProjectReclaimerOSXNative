#!/usr/bin/env python3
"""Each walker's own score in the running round (2026-11-08, coach-rounds.sh: the best walker of a round is followed by the others):
asks every walker's stats server (identity 1 -> 8766, 2 -> 8768, 3 -> 8769, 4 -> 8770) for its own player ("you") and prints
"<id> <kills> <deaths>" per line; a walker that does not answer is left out.
    python3 shim/tools/walker-scores.py "1 2 3 4"
    python3 shim/tools/walker-scores.py --best <file>    the best of a saved listing: (kills + 1) / (deaths + 1), most kills on a tie
    python3 shim/tools/walker-scores.py --recording <rec.jsonl.gz> <samples.jsonl> "1 2 3 4"   the scores at the end of a recorded round"""
import json, sys, urllib.request

PORTS = {1: 8766, 2: 8768, 3: 8769, 4: 8770}

NICKS = {'Second': 1, 'Third': 2, 'Fourth': 3, 'Fifth': 4}

def from_recording(rec, samples, ids):
    """each walker's kills / deaths at the END of a recorded round: the recording's last sample by slot, slots -> walkers by the
    names in the samples file (the server's players list)"""
    import gzip
    slot_of = {}
    for l in open(samples):
        try: d = json.loads(l)
        except Exception: continue
        for p in d.get('players') or []:
            if p.get('name') in NICKS and p.get('slot') is not None: slot_of[NICKS[p['name']]] = p['slot']
    last = None
    try:
        with gzip.open(rec, 'rt') as f:
            for l in f:
                try: o = json.loads(l)
                except Exception: break
                if 'p' in o: last = o
    except (EOFError, OSError): pass
    if not last: return
    R = {r[0]: r for r in last['p']}
    for k in ids:
        sl = slot_of.get(k)
        if sl in R: print(k, R[sl][12], R[sl][13])

def main():
    if sys.argv[1] == '--recording':   # --recording <rec> <samples.jsonl> "1 2 3 4"
        from_recording(sys.argv[2], sys.argv[3], [int(x) for x in sys.argv[4].split()]); return 0
    if sys.argv[1] == '--best':
        rows = [l.split() for l in open(sys.argv[2]) if len(l.split()) == 3]
        if not rows: return 1
        best = max(rows, key=lambda r: ((int(r[1]) + 1) / (int(r[2]) + 1), int(r[1])))
        print(best[0]); return 0
    for k in [int(x) for x in sys.argv[1].split()]:
        try: d = json.load(urllib.request.urlopen('http://127.0.0.1:%d/stats.json' % PORTS[k], timeout=3))
        except Exception: continue
        me = [p for p in d.get('players', []) if p.get('you')]
        if me: print(k, me[0].get('kills', 0), me[0].get('deaths', 0))
    return 0

if __name__ == '__main__':
    sys.exit(main())
