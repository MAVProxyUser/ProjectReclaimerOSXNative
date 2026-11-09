#!/usr/bin/env python3
"""Find the weapon object's ammo fields (2026-11-08: the walker should know its own inventory, ammo included). Reads the
[wpndump] lines nhook writes with NHOOK_WPNDUMP=1 (nstats.c: the local player's held weapon object, 0x300 bytes, with its shots
counter, every snapshot) and scores every 16-bit field: ROUNDS LOADED drops while the shots counter rises (the drop tracks the
shots) and jumps up on a reload while nothing is fired; ROUNDS IN RESERVE drops on a reload by about what the magazine gained.
Prints the best candidates per weapon and writes ReclaimerMac/native/ammo.json {"loaded": off, "reserve": off, "evidence": ...}
when one field wins clearly on 2+ weapons or with 3+ reloads (the walker reads it: NHOOK_AMMO_OFF / nbot.c).
    python3 shim/tools/ammo-scan.py <app log> [<app log> ...] [--write]"""
import json, os, sys
from collections import defaultdict

def parse(paths):
    S = defaultdict(list)   # weapon handle -> [(t, shots, name, bytes)]
    for p in paths:
        for l in open(p, errors='replace'):
            i = l.find('[wpndump] ')
            if i < 0: continue
            tk = l[i:].split()
            try: t = float(tk[1]); shots = int(tk[3]); h = tk[7]; name = ' '.join(tk[8:-1]); b = bytes.fromhex(tk[-1])
            except Exception: continue
            S[h].append((t, shots, name, b))
    return S

def score(seq):
    """per 16-bit offset: (score, reloads, fired steps matched, value range)"""
    out = {}
    if len(seq) < 10: return out
    n = len(seq[0][3])
    for off in range(0, n - 1, 2):
        v = [int.from_bytes(s[3][off:off + 2], 'little', signed=True) for s in seq]
        if max(v) - min(v) < 2 or max(v) > 2000 or min(v) < 0: continue
        good = bad = rel = 0
        for k in range(1, len(seq)):
            ds = seq[k][1] - seq[k - 1][1]; dv = v[k] - v[k - 1]
            if ds > 0 and dv < 0: good += 1
            elif ds > 0 and dv == 0: pass
            elif ds == 0 and dv < 0: bad += 1
            elif dv > 0 and ds == 0: rel += 1
            elif dv > 0 and ds > 0: bad += 1
        if good >= 3: out[off] = (good - 2 * bad + 2 * rel, rel, good, bad, min(v), max(v))
    return out

def reserve(seq, loaded):
    """the field that drops when the loaded count jumps up (a reload) by about the same amount"""
    out = {}; n = len(seq[0][3])
    lv = [int.from_bytes(s[3][loaded:loaded + 2], 'little', signed=True) for s in seq]
    for off in range(0, n - 1, 2):
        if off == loaded: continue
        v = [int.from_bytes(s[3][off:off + 2], 'little', signed=True) for s in seq]
        if max(v) - min(v) < 2 or min(v) < 0 or max(v) > 2000: continue
        m = sum(1 for k in range(1, len(seq)) if lv[k] > lv[k - 1] and 0 < v[k - 1] - v[k] <= lv[k] - lv[k - 1] + 1)
        x = sum(1 for k in range(1, len(seq)) if v[k] < v[k - 1] and not lv[k] > lv[k - 1])
        if m >= 1: out[off] = (m - x, m, x, min(v), max(v))
    return out

def main():
    paths = [a for a in sys.argv[1:] if not a.startswith('--')]
    S = parse(paths); win = defaultdict(float); ev = {}
    if not S: print('no [wpndump] lines (run the app with NHOOK_WPNDUMP=1)'); return 1
    for h, seq in S.items():
        sc = score(seq); name = seq[0][2]
        if not sc: continue
        top = sorted(sc.items(), key=lambda kv: -kv[1][0])[:4]
        print('%s (%s): %d samples, %d shots' % (name, h, len(seq), seq[-1][1] - seq[0][1]))
        for off, (s, rel, good, bad, lo, hi) in top: print('   +0x%03x  score %3d  reloads %d  drops while firing %d  wrong %d  values %d..%d' % (off, s, rel, good, bad, lo, hi))
        best = top[0]
        if best[1][0] >= 5: win[best[0]] += best[1][0]; ev.setdefault(best[0], []).append({'weapon': name, 'score': best[1][0], 'reloads': best[1][1], 'range': [best[1][4], best[1][5]]})
        rs = reserve(seq, best[0])
        if rs:
            rt = sorted(rs.items(), key=lambda kv: -kv[1][0])[:2]
            for off, (s, m, x, lo, hi) in rt: print('   reserve? +0x%03x  score %d  (reload drops %d, other drops %d, values %d..%d)' % (off, s, m, x, lo, hi))
            ev.setdefault('reserve', []).append({'weapon': name, 'off': rt[0][0], 'score': rt[0][1][0]})
    if not win: print('no clear rounds-loaded field yet'); return 1
    off = max(win, key=win.get); clear = len(ev[off]) >= 2 or sum(e['reloads'] for e in ev[off]) >= 3
    rv = [e for e in ev.get('reserve', []) if e['score'] > 0]; rbest = max(set(e['off'] for e in rv), key=lambda o: sum(e['score'] for e in rv if e['off'] == o)) if rv else None
    print('rounds loaded: weapon object +0x%03x (%s)%s' % (off, ', '.join('%s %d' % (e['weapon'], e['score']) for e in ev[off]), '' if clear else ' -- not clear yet'))
    if rbest is not None: print('rounds in reserve: +0x%03x' % rbest)
    if '--write' in sys.argv and clear:
        p = os.path.expanduser('~/Library/Application Support/ReclaimerMac/native/ammo.json')
        json.dump({'loaded': off, 'reserve': rbest, 'evidence': ev[off]}, open(p, 'w'), indent=1); print('->', p)
    return 0

if __name__ == '__main__':
    sys.exit(main())
