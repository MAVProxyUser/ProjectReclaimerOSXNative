#!/usr/bin/env python3
"""Record how real players move and fight (2026-11-08; the user: "join several games ... watch and sample how real players move
around the map, and skirmish when they encounter each other ... start building a library / model"). Polls a running nhook stats
page (the native app, any game) 10 times a second and writes one gzipped JSON line per sample:
    {"t": seconds, "map": "<map>", "you": <observer slot>, "p": [[slot, team, alive, x, y, z, yaw, pitch, shield, health,
     weapon, vehicle, kills, deaths, shots, hits], ...]}
and one line per feed event (kills, deaths, spawns...): {"t": .., "ev": kind, "a": <slot>, "b": <slot>, "how": "...", "est": bool,
"pos": [x, y, z]}. Player NAMES are not kept (slots only: a name is mapped to its slot from the samples and dropped). A new file
per match: <ReclaimerMac/tracks>/<date>_<map>_<server tag>.jsonl.gz (the server tag = a hash of its address).
    python3 shim/tools/match-recorder.py [--port 8765] [--seconds N] [--server host:port] [--out DIR]
Used by shim/tools/observe-public.sh; the model: shim/tools/walker-model.py."""
import gzip, hashlib, json, os, sys, time, urllib.request

def arg(k, d):
    a = sys.argv; return a[a.index(k) + 1] if k in a else d
PORT = int(arg('--port', '8765')); SECS = float(arg('--seconds', '0')); SERVER = arg('--server', ''); MODE = arg('--mode', '')
OUT = arg('--out', os.path.expanduser('~/Library/Application Support/ReclaimerMac/tracks')); os.makedirs(OUT, exist_ok=True)
TAG = hashlib.sha1(SERVER.encode()).hexdigest()[:8] if SERVER else 'local'

def get(path, t=2.0):
    with urllib.request.urlopen('http://127.0.0.1:%d%s' % (PORT, path), timeout=t) as r: return json.loads(r.read())

def main():
    t0 = time.time(); f = None; cur = None; feed = 0; names = {}; n = 0; nev = 0; last_ok = time.time(); last_feed = 0
    while not SECS or time.time() - t0 < SECS:
        tick = time.time()
        try: s = get('/stats.json'); last_ok = tick
        except Exception:
            if tick - last_ok > 120: print('[rec] the page stopped answering: done', flush=True); break
            time.sleep(1); continue
        mp = s.get('map') or ''; inm = bool(s.get('in_match'))
        if not inm or not mp:
            if f: f.close(); print('[rec] %s: %d samples, %d events' % (cur, n, nev), flush=True); f = None; cur = None
            time.sleep(0.5); continue
        if cur != mp or f is None:
            if f: f.close(); print('[rec] %s: %d samples, %d events' % (cur, n, nev), flush=True)
            cur = mp; n = nev = 0
            path = os.path.join(OUT, '%s_%s_%s.jsonl.gz' % (time.strftime('%Y%m%d-%H%M%S'), mp, TAG)); f = gzip.open(path, 'wt')
            f.write(json.dumps({'start': time.time(), 'map': mp, 'server': TAG, 'mode': MODE, 'variant': (s.get('variant') or {}).get('name') if isinstance(s.get('variant'), dict) else None,
                                'lanes_live': bool(s.get('lanes_live'))}) + '\n')   # (lanes_live: the game counts this match's lanes itself; lanes-import.py skips it)
            print('[rec] recording %s -> %s' % (mp, os.path.basename(path)), flush=True)
            try: feed = get('/stats.json').get('feed_seq', 0)
            except Exception: pass
        you = -1; rows = []
        for p in s.get('players', []):
            sl = p.get('slot', -1); names[p.get('name', '')] = sl
            if p.get('you'): you = sl
            q = p.get('pos'); a = p.get('look')
            if not (isinstance(q, list) and len(q) == 3): q = [0, 0, 0]
            if not (isinstance(a, list) and len(a) == 2): a = [0, 0]   # (an old dylib's "aim" was sometimes the aim meter's object)
            rows.append([sl, p.get('team', -1), 1 if p.get('alive') else 0, round(q[0], 3), round(q[1], 3), round(q[2], 3), round(a[0], 2), round(a[1], 2),
                         round(p.get('shield', 0) or 0, 3), round(p.get('health', 0) or 0, 3), p.get('weapon') or '', p.get('vehicle') or '',
                         p.get('kills', 0), p.get('deaths', 0), p.get('shots', 0), p.get('hits', 0)])
        f.write(json.dumps({'t': round(tick - t0, 2), 'you': you, 'p': rows, 'proj': len(s.get('projectiles') or [])}, separators=(',', ':')) + '\n'); n += 1
        if tick - last_feed > 1.0:   # the feed: names -> slots, then dropped
            last_feed = tick
            try:
                j = get('/feed.json?since=%d' % feed); evs = j.get('events', j if isinstance(j, list) else [])
                for e in evs:
                    feed = max(feed, e.get('seq', feed))
                    k = e.get('kind')
                    if k in ('chat', 'join', 'leave', 'vote', 'log', 'server', 'notice'): continue
                    o = {'t': round(tick - t0, 2), 'ev': k, 'a': names.get(e.get('a'), -1), 'b': names.get(e.get('b'), -1), 'how': e.get('how') or '', 'est': bool(e.get('estimated')), 'verb': e.get('verb') or ''}
                    if e.get('pos'): o['pos'] = [round(x, 2) for x in e['pos']]
                    f.write(json.dumps(o, separators=(',', ':')) + '\n'); nev += 1
                if isinstance(j, dict) and 'seq' in j: feed = max(feed, j['seq'])
            except Exception: pass
        dt = 0.1 - (time.time() - tick)
        if dt > 0: time.sleep(dt)
    if f: f.close(); print('[rec] %s: %d samples, %d events' % (cur, n, nev), flush=True)

if __name__ == '__main__':
    main()
