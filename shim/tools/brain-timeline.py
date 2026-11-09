#!/usr/bin/env python3
"""Each 3-minute window of the walker's public training with the brain that played it (2026-11-09; the user: "each training session
reload you should describe what the new brain is trying to show that it learned"). Joins battles/follow/learned.jsonl (follow-study.py:
one line per window -- kills, deaths, shots, hits, kill weapons) with battles/follow/brain-lessons.jsonl (build.sh: one line per brain
build -- its lesson) and prints, per brain, its windows and their sums: kills, deaths, K/D, accuracy.
    python3 shim/tools/brain-timeline.py [--since HH:MM] [--json]"""
import json, os, sys, time

SUP = os.path.expanduser('~/Library/Application Support/ReclaimerMac'); D = os.path.join(SUP, 'battles', 'follow')
def rd(p):
    out = []
    try:
        for l in open(p):
            try: out.append(json.loads(l))
            except Exception: pass
    except OSError: pass
    return out
def main():
    W = rd(os.path.join(D, 'learned.jsonl')); B = rd(os.path.join(D, 'brain-lessons.jsonl'))
    since = 0
    if '--since' in sys.argv:
        hh, mm = sys.argv[sys.argv.index('--since') + 1].split(':'); lt = time.localtime(); since = time.mktime((lt.tm_year, lt.tm_mon, lt.tm_mday, int(hh), int(mm), 0, 0, 0, -1))
    B.sort(key=lambda b: b['t']); rows = {}
    for w in W:
        if w.get('t', 0) < since: continue
        # (the brain in play over a window = the last one built before the window's middle, 90 s before its end)
        mid = w['t'] - 90; br = None
        for b in B:
            if b['t'] <= mid: br = b
        key = br['t'] if br else 0; r = rows.setdefault(key, {'lesson': br['lesson'] if br else '(before the lessons)', 'built': br['built'] if br else '-', 'win': []})
        r['win'].append(w)
    if '--json' in sys.argv: print(json.dumps(rows, indent=1)); return 0
    for key in sorted(rows):
        r = rows[key]; k = sum(w['walker']['k'] for w in r['win']); d = sum(w['walker']['d'] for w in r['win'])
        sh = sum(w['walker'].get('shots', 0) for w in r['win']); hi = sum(w['walker'].get('hits', 0) for w in r['win'])
        print('brain built %s -- %d window%s: %d kills / %d deaths (K/D %.2f), accuracy %s' % (r['built'], len(r['win']), '' if len(r['win']) == 1 else 's', k, d, (k / d) if d else float(k),
              '%.0f%%' % (100.0 * hi / sh) if sh else '-'))
        print('   lesson: %s' % (r['lesson'][:300] + ('...' if len(r['lesson']) > 300 else '')))
        print('   windows: %s' % ', '.join('%s %d/%d' % (time.strftime('%H:%M', time.localtime(w['t'])), w['walker']['k'], w['walker']['d']) for w in r['win']))
    return 0

if __name__ == '__main__':
    sys.exit(main())
