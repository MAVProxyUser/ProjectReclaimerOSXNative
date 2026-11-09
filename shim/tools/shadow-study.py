#!/usr/bin/env python3
"""The walker riding along in a player's game, compared with the player (2026-11-09; NHOOK_SHADOW=1, nbot.c shadow_log: the walker
decides every step in a real public match and publishes nothing). Each 0.1 s sample has the walker's decision (its target and the
target's position, fire, melee, its throttle forward / left and aim yaw / pitch) and the player's own (throttle, aim, the control
record's button word). Per file:
  target      when the walker has a target, how often the player looks within 10 deg of it (the same enemy)
  fire        the walker wants to fire vs the player firing (button bit 19), by distance band
  melee       the walker's melee vs the player's melee (button bit 0; NHOOK_INPUT_LOG's labelling: bit 11 is part of the grenade throw)
  move        the angle between the walker's chosen direction and the player's (both moving), median; how often within 45 deg
  aim         the angle between the walker's intended aim and the player's, both on the walker's target
    python3 shim/tools/shadow-study.py [file.jsonl ...]   (default: the newest in tracks/shadow)"""
import glob, json, math, os, sys

SH = os.path.expanduser('~/Library/Application Support/ReclaimerMac/tracks/shadow')
FIRE, MELEE = 1 << 19, 1 << 0   # (the input map from the user's own play: +8 bit 0 = melee; 0x21100800 = bit 11 + 20/24/29 is the grenade throw)
def wrap(a): return (a + math.pi) % (2 * math.pi) - math.pi
def med(v):
    v = sorted(v); return round(v[len(v) // 2], 1) if v else None

def study(p):
    S = []
    for l in open(p, errors='replace'):
        try: S.append(json.loads(l))
        except Exception: pass
    if not S: print('%s: empty' % p); return
    tg = same = 0; fire = {}; mel = [0, 0, 0]; mv = []; aim = []
    for s in S:
        you = s['you']; btn = int(you[4], 16); w = s['want']; pos = s['pos']
        if 'tg' in s:
            tp = s['tp']; dx, dy, dz = tp[0] - pos[0], tp[1] - pos[1], tp[2] - pos[2]; h = math.hypot(dx, dy)
            ty, tpi = math.atan2(dy, dx), math.atan2(dz, h)
            off = math.degrees(math.hypot(wrap(you[2] - ty), you[3] - tpi)); tg += 1; same += off < 10
            band = '<8' if s['d'] < 8 else '8-15' if s['d'] < 15 else '15-25' if s['d'] < 25 else '25+'
            f = fire.setdefault(band, [0, 0, 0, 0])   # both, walker only, player only, neither
            wf, pf = bool(s['fire']), bool(btn & FIRE); f[0 if wf and pf else 1 if wf else 2 if pf else 3] += 1
            if off < 10: aim.append(math.degrees(math.hypot(wrap(w[2] - you[2]), w[3] - you[3])))
        wm, pm = bool(s['mel']), bool(btn & MELEE); mel[0] += wm and pm; mel[1] += wm and not pm; mel[2] += pm and not wm
        if math.hypot(w[0], w[1]) > 0.3 and math.hypot(you[0], you[1]) > 0.3:
            a = w[2] + math.atan2(w[1], w[0]); b = you[2] + math.atan2(you[1], you[0]); mv.append(abs(math.degrees(wrap(a - b))))
    print('== %s: %d samples (%.0f s)' % (os.path.basename(p), len(S), S[-1]['t'] - S[0]['t']))
    print('   target: the walker had one %d samples; the player looked at the same enemy %.0f%% of them' % (tg, 100 * same / max(1, tg)))
    for band in ('<8', '8-15', '15-25', '25+'):
        if band in fire: b, wo, po, ne = fire[band]; n = b + wo + po + ne; print('   fire %-6s units: both %3.0f%%, walker only %3.0f%%, player only %3.0f%%, neither %3.0f%% (%d samples)' % (
            band, 100 * b / n, 100 * wo / n, 100 * po / n, 100 * ne / n, n))
    print('   melee: both %d, walker only %d, player only %d (samples)' % tuple(mel))
    if mv: print('   move: the walker\'s direction vs the player\'s: median %.0f deg off, within 45 deg %.0f%% (%d samples)' % (med(mv), 100 * sum(x < 45 for x in mv) / len(mv), len(mv)))
    if aim: print('   aim on the same enemy: the walker\'s intended aim vs the player\'s: median %.1f deg (%d samples)' % (med(aim), len(aim)))

def main():
    F = [a for a in sys.argv[1:] if not a.startswith('--')] or sorted(glob.glob(os.path.join(SH, '*.jsonl')))[-1:]
    for p in F: study(p)
    return 0

if __name__ == '__main__':
    sys.exit(main())
