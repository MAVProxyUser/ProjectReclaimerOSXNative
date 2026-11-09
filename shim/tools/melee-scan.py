#!/usr/bin/env python3
"""Which input makes a walker MELEE? (2026-11-08; backlog N26. The user: "train the walker to melee more when it is in close to an
opponent! especially if the opponent's back is to it." -- but no walker melee had ever landed: B, RB and the pulsed trigger all
failed in melee-battle.sh.) On our local dedicated server (native-dedicated.sh, assault rifles, no grenades, normal shields): two
test clients join -- A (test-v2: the virtual pad, the sparring aim on the nearest enemy, never firing) and B (test-second: standing
still). For each candidate input, A is teleported next to B (the host tools' tpnext: right / left / behind of B), B's shield must be
full, the input is held 0.3 s on A, and B's shield and health are read from the SERVER's stats a second later: a drop without a
shot (A never fires) is a melee hit. Candidates: every virtual-pad button (do=botpad: A B X Y LB RB, the stick clicks, the
d-pad, the left trigger) and every bit of the control record's +0xC8 button word but jump (1) / fire (19, 21) / grenade (30)
(do=botbtn). Prints each candidate's damage; writes tracks/melee-scan.json.
    python3 shim/tools/melee-scan.py [--pads-only | --packet] [--bits 0,2,3] [--secs 0.3] [--repeat 2]
Never beside a running Project Reclaimer."""
import json, os, subprocess, sys, time, urllib.request

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..')); os.chdir(ROOT)
SUP = os.path.expanduser('~/Library/Application Support/ReclaimerMac/native')
TR = os.path.expanduser('~/Library/Application Support/ReclaimerMac/tracks')
SRV, PA, PB = 8767, 8765, 8766
APPRE = '^/Applications/Project Reclaimer.app/Contents/MacOS/project-reclaimer'
arg = lambda k, d: sys.argv[sys.argv.index(k) + 1] if k in sys.argv else d
SECS = float(arg('--secs', '0.3')); REP = int(arg('--repeat', '1'))
PADS = [('A', 0x1), ('B', 0x2), ('X', 0x4), ('Y', 0x8), ('LB', 0x10), ('RB', 0x20), ('left stick click', 0x40), ('right stick click', 0x80),
        ('d-pad up', 0x400), ('d-pad down', 0x800), ('d-pad left', 0x1000), ('d-pad right', 0x2000), ('left trigger', 0x40000000)]
BITS = [int(b) for b in arg('--bits', ','.join(str(b) for b in range(64) if b not in (1, 19, 21, 30))).split(',') if b != '']
if '--pads-only' in sys.argv: BITS = []
if '--no-pads' in sys.argv: PADS = []   # (e.g. --no-pads --bits 19: the control-record fire bit)
if '--rt-control' in sys.argv: PADS = [('right trigger', 0x80000000)]; BITS = []   # (the positive control: the pad's trigger must hurt B)
# --packet: the bits the control packet actually carries to the server (net-authority.md: action +8 bits 0-2, 11, 14 and +0x2c's
# 7 bits), set straight in the packed record (do=botpkt) -- no pad button meleed (first scan, 2026-11-08)
PKT = [('packet +8 bit %d' % k, 'b8', 1 << k) for k in (0, 1, 2, 11, 14)] + [('packet +0x2c bit %d' % k, 'b2c', 1 << k) for k in range(7)]

def get(port, path='/stats.json'):
    try: return json.load(urllib.request.urlopen('http://127.0.0.1:%d%s' % (port, path), timeout=3))
    except Exception: return None
def ctl(port, q):
    try:
        r = urllib.request.Request('http://127.0.0.1:%d/control?%s' % (port, q), method='POST', headers={'X-Reclaimer-Control': '1'})
        return json.load(urllib.request.urlopen(r, timeout=3)).get('ok')
    except Exception: return None
def players():
    s = get(SRV); P = (s or {}).get('players') or []
    b = [p for p in P if p.get('name') == 'Second']; a = [p for p in P if p.get('name') != 'Second']
    return (a[0] if a else None), (b[0] if b else None)

def main():
    if subprocess.run(['pgrep', '-f', APPRE], capture_output=True).stdout.strip(): print('Project Reclaimer is running: not starting'); return 1
    env = dict(os.environ, DED_OPTIONS='{"player.primary_weapon": 1, "player.secondary_weapon": 1, "player.grenades": 2}')
    subprocess.run(['bash', 'shim/tools/native-dedicated.sh', 'start'], env=env, stdout=subprocess.DEVNULL)
    logs = '/tmp' if not os.environ.get('TMPDIR') else os.environ['TMPDIR']; T = os.path.join(logs, 'melee-scan-%d' % os.getpid()); os.makedirs(T, exist_ok=True)
    procs = []
    for who, prof, port, extra in (('a', 'test-v2', PA, {'NHOOK_VPAD': '1', 'NHOOK_SPAR': '1', 'NHOOK_BOT_FIRE': '0', 'NHOOK_SPAR_LOG': '1'}), ('b', 'test-second', PB, {})):
        d = os.path.join(SUP, prof)
        e = dict(os.environ, NATIVE_LOG=os.path.join(T, who + '.log'), NATIVE_PROFILE=d, RECLAIMER_CACHE_DIR=d + '-cache', PELOAD_FORENSIC_DIR=d + '-forensics',
                 NHOOK_HEATMAP_DIR=d + '-heatmaps', NHOOK_STATS_HTTP=str(port), NHOOK_STATS_OPEN='0', NATIVE_ALLOW_SECOND='1', NHOOK_MUTE_OUTPUT='1', **extra)
        procs.append(subprocess.Popen(['bash', 'shim/native.sh', '--join-server', '127.0.0.1:49190', '--background'], env=e, stdout=open(os.path.join(T, who + '.out'), 'w'), stderr=subprocess.STDOUT))
        time.sleep(6)
    print('logs: %s' % T, flush=True)
    res = []
    try:
        t0 = time.time()
        while time.time() - t0 < 180:
            a, b = players()
            if a and b and a.get('alive') and b.get('alive'): break
            time.sleep(2)
        else: print('the two players never both came alive'); return 1
        time.sleep(4)
        if '--hold' in sys.argv:   # (the setup only, for poking by hand: POST controls to 8765 (A), read 8767 (the server))
            a, b = players(); print('holding %s s: A slot %s, B slot %s' % (arg('--hold', '300'), a.get('slot'), b.get('slot')), flush=True)
            time.sleep(float(arg('--hold', '300'))); return 0
        cands = [('pkt', n, (f, v)) for n, f, v in PKT] if '--packet' in sys.argv else [('pad', n, v) for n, v in PADS] + [('bit', 'bit %d' % k, 1 << k) for k in BITS]
        for kind, name, v in cands * REP:
            # B alive with a full shield (it recharges ~5 s after damage; a dead B respawns)
            for i in range(30):
                a, b = players()
                if a and b and a.get('alive') and b.get('alive') and b.get('shield', 0) >= 0.99 and b.get('health', 0) >= 0.99: break
                time.sleep(0.5)
            else: res.append({'input': name, 'skipped': 'B never full'}); print('%-20s skipped (B never full)' % name, flush=True); continue
            ctl(SRV, 'do=tpnext&slot=%d&who=%d' % (b['slot'], a['slot'])); time.sleep(1.0)
            a, b = players(); s0, h0 = b.get('shield', 0), b.get('health', 0)
            if kind == 'pkt': ok = ctl(PA, 'do=botpkt&%s=%x&secs=%.2f' % (v[0], v[1], SECS))
            else: ok = ctl(PA, ('do=botpad&b=%x&secs=%.2f' if kind == 'pad' else 'do=botbtn&bits=%x&secs=%.2f') % (v, SECS))
            time.sleep(1.2)
            a2, b2 = players(); s1 = b2.get('shield', 0) if b2 else 0; h1 = b2.get('health', 0) if b2 else 0; alive = bool(b2 and b2.get('alive'))
            if '--debug' in sys.argv:   # (the setup itself: where both are on the server, what A's client sees, A's shots)
                sa = get(PA) or {}; pa = [p for p in (sa.get('players') or []) if p.get('name') == 'Second']
                pos = lambda p: [round(x, 2) for x in (p.get('pos') or p.get('position') or [])] if p else None
                print('   server: A %s B %s (keys %s); A sees B shield %s; A perf %s; A shots %s' % (pos(a2), pos(b2), sorted((b2 or {}).keys())[:12],
                      pa[0].get('shield') if pa else None, (sa.get('perf') or {}).get('fps'), [p.get('shots') for p in (sa.get('players') or []) if p.get('name') != 'Second']), flush=True)
            dmg = (s0 - s1) + (h0 - h1) if alive else s0 + h0
            r = {'input': name, 'kind': kind, 'value': v if kind != 'pkt' else '%s=%x' % v, 'accepted': bool(ok), 'shield_before': s0, 'shield_after': s1, 'health_after': h1, 'killed': not alive, 'damage': round(dmg, 3)}
            res.append(r); print('%-20s %s damage %.2f (shield %.2f -> %.2f, health %.2f)%s' % (name, ' ' if ok else '!', dmg, s0, s1, h1, '  <== HIT' if dmg > 0.15 else ''), flush=True)
    finally:
        for p in procs: p.terminate()
        subprocess.run(['pkill', '-f', APPRE + ' --join-server 127.0.0.1:49190'])
        time.sleep(3); subprocess.run(['bash', 'shim/tools/native-dedicated.sh', 'stop'], stdout=subprocess.DEVNULL)
    hits = [r for r in res if r.get('damage', 0) > 0.15]
    json.dump({'when': time.strftime('%Y-%m-%d %H:%M'), 'secs': SECS, 'results': res, 'hits': [r['input'] for r in hits]}, open(os.path.join(TR, 'melee-scan.json'), 'w'), indent=1)
    print('melee scan: %s' % (', '.join(r['input'] for r in hits) if hits else 'no input hurt B'))
    return 0

if __name__ == '__main__':
    sys.exit(main())
