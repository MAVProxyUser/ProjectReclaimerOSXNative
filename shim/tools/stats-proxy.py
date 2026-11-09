#!/usr/bin/env python3
"""Serve another stats page on a port (2026-11-09; the user, their game closed with the walker playing on: "I need to see the game stats for
the walker so ... I can still see http://127.0.0.1:8765/"): a relay on 127.0.0.1:<listen> to 127.0.0.1:<target> (GET and POST, bodies and
headers passed through, Cache-Control too) -- the walker's own page (8771) under the user's usual address. This Mac only. When a game of
the user's starts (a Project Reclaimer without --headless) it lets the port go for 30 s -- a game started with nhook binds 8765 then --
and serves again once the port is free (the game had no page of its own, or it ended).
--stay: never step aside (the user, 11:40: "the stats server should stay tied to the walker for now").
    python3 shim/tools/stats-proxy.py [--listen 8765] [--target 8771] [--stay]"""
import http.server, os, socketserver, subprocess, sys, threading, time, urllib.request
import json, select, socket

# THE SERVER'S NAME (2026-11-09; the user: "can you add the name of the connected server to the top of http://127.0.0.1:8765"): the
# game's stats.json says only which address it joined (server_addr); the relay asks that server once a minute what it is -- the one
# status datagram every client sends to each listed server (a6 01 00 <nonce8>, zero-padded to 1229 bytes -> a7 <nonce8> {"info": {name,
# map, mode, players, max_players, ...}}; shim/tools/servers10.py, server-status.md) -- and adds server_name / server_info to the
# stats.json it passes on. Nothing is sent anywhere else.
_srv = {'addr': None, 't': 0.0, 'info': None, 'busy': False}
def server_info(addr):
    if not addr or ':' not in addr: return None
    now = time.time()
    if _srv['addr'] == addr and (now - _srv['t'] < 60 or _srv['busy']): return _srv['info']
    if _srv['busy']: return _srv['info'] if _srv['addr'] == addr else None
    if _srv['addr'] != addr: _srv.update(addr=addr, info=None)
    _srv['busy'] = True; _srv['t'] = now
    def ask():
        try:
            h, port = addr.rsplit(':', 1); k = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); n = os.urandom(8)
            q = bytes([0xa6, 1, 0]) + n; q += bytes(1229 - len(q)); k.sendto(q, (h, int(port))); end = time.time() + 2.5
            while time.time() < end:
                r, _, _ = select.select([k], [], [], 0.2)
                if not r: continue
                d, _ = k.recvfrom(65536)
                if len(d) >= 10 and d[0] == 0xa7 and d[1:9] == n:
                    j = json.loads(d[9:]); i = j.get('info') if isinstance(j, dict) else None
                    if isinstance(i, dict) and _srv['addr'] == addr:
                        _srv['info'] = {x: i.get(x) for x in ('name', 'map', 'mode', 'players', 'max_players', 'version')}
                    break
            k.close()
        except Exception: pass
        finally: _srv['busy'] = False
    threading.Thread(target=ask, daemon=True).start()
    return _srv['info']

def arg(k, d):
    a = sys.argv; return a[a.index(k) + 1] if k in a else d
LISTEN, TARGET = int(arg('--listen', '8765')), int(arg('--target', '8771')); STAY = '--stay' in sys.argv

# ONE PLAYER'S MATCH (2026-11-09; the user: the player details "full window, and it should side by side a version of the heatmap with JUST my
# player visualized"): GET /trail.json?slot=<n>&map=<map> -> that slot's path, death spots and kill spots this match, from the walker's own
# recording of it (nhook ntracks.c: every player's position 10x a second; the newest tracks/*.jsonl.gz written in the last 90 s, of that
# map). Positions every 0.5 s alive; a death where it was last alive; a kill where it stood when its kill count rose. A slot whose
# counters go back down changed hands: its path starts again. No names in the recording: the page asks by slot. Cached 10 s.
TRACKS = os.path.expanduser('~/Library/Application Support/ReclaimerMac/tracks')
_trail = {}
def trail(slot, mp):
    import glob, gzip
    fs = sorted(glob.glob(os.path.join(TRACKS, '*.jsonl.gz')), key=os.path.getmtime)
    if not fs or time.time() - os.path.getmtime(fs[-1]) > 90: return None
    f = fs[-1]; c = _trail.get((f, slot))
    if c and time.time() - c[0] < 10: return c[1]
    head = None; pts, deaths, kills = [], [], []; lt = -9.0; was = None; lastpos = None; k0 = d0 = None; t0 = t1 = None
    try:
        with gzip.open(f, 'rt', errors='replace') as g:
            for line in g:
                if head is None:
                    head = json.loads(line)
                    if mp and head.get('map') != mp: return None
                    continue
                if not line.startswith('{"t"'): continue
                try: smp = json.loads(line)
                except ValueError: continue
                for r in smp.get('p') or []:
                    if r[0] != slot: continue
                    t = smp['t']; k, d = (r[12], r[13]) if len(r) > 13 else (0, 0)
                    if k0 is not None and (k < k0 or d < d0): pts, deaths, kills, lastpos, was, t0 = [], [], [], None, None, None
                    alive = bool(r[2]) and bool(r[3] or r[4])
                    if alive:
                        lastpos = [round(r[3], 1), round(r[4], 1)]
                        if t - lt >= 0.5: pts.append(lastpos); lt = t
                        if t0 is None: t0 = t
                        t1 = t
                    elif was and lastpos: deaths.append(lastpos)
                    if k0 is not None and k > k0 and lastpos: kills.append(lastpos)
                    k0, d0, was = k, d, alive
                    break
    except (EOFError, OSError, ValueError): pass
    res = {'file': os.path.basename(f), 'map': head.get('map') if head else None, 'slot': slot, 'points': pts, 'deaths': deaths, 'kills': kills,
           'seconds': round(t1 - t0, 1) if t0 is not None and t1 is not None else 0}
    _trail[(f, slot)] = (time.time(), res); return res

class H(http.server.BaseHTTPRequestHandler):
    def relay(self, body=None):
        req = urllib.request.Request('http://127.0.0.1:%d%s' % (TARGET, self.path), data=body, method=self.command)
        for k in ('Content-Type', 'X-Reclaimer-Control'):
            if self.headers.get(k): req.add_header(k, self.headers[k])
        try:
            with urllib.request.urlopen(req, timeout=10) as r: data = r.read(); code = r.status; ct = r.headers.get('Content-Type', 'application/octet-stream'); cc = r.headers.get('Cache-Control')
        except urllib.error.HTTPError as e: data = e.read(); code = e.code; ct = e.headers.get('Content-Type', 'text/plain'); cc = None
        except Exception as e: data = ('relay: %s' % e).encode(); code = 502; ct = 'text/plain'; cc = None
        if code == 200 and self.command == 'GET' and self.path.startswith('/stats.json'):
            try:
                j = json.loads(data); inf = server_info(j.get('server_addr'))
                if inf: j['server_name'] = inf.get('name'); j['server_info'] = inf
                data = json.dumps(j).encode()
            except Exception: pass
        # (2026-11-09: the game's Cache-Control passed on -- without it the browser kept an old copy of the page after an edit)
        self.send_response(code); self.send_header('Content-Type', ct); self.send_header('Content-Length', str(len(data)))
        self.send_header('Cache-Control', cc or 'no-cache'); self.end_headers(); self.wfile.write(data)
    def stream(self):   # (the page's live feed, /events: Server-Sent Events -- passed on line by line as they come, never buffered)
        try:
            r = urllib.request.urlopen('http://127.0.0.1:%d%s' % (TARGET, self.path), timeout=60)
            self.send_response(r.status)
            for k in ('Content-Type', 'Cache-Control'):
                if r.headers.get(k): self.send_header(k, r.headers[k])
            self.send_header('Connection', 'close'); self.end_headers()
            while True:
                line = r.readline()
                if not line: break
                if line.startswith(b'data: {') and b'"server_addr"' in line:   # (the snapshots the page draws: the server's name added, as in stats.json)
                    try:
                        j = json.loads(line[6:]); inf = server_info(j.get('server_addr'))
                        if inf: j['server_name'] = inf.get('name'); j['server_info'] = inf; line = b'data: ' + json.dumps(j).encode() + b'\n'
                    except Exception: pass
                self.wfile.write(line); self.wfile.flush()
        except Exception: pass
    def do_GET(self):
        if self.path.startswith('/events'): self.stream()
        elif self.path.startswith('/trail.json'):
            from urllib.parse import urlparse, parse_qs
            q = parse_qs(urlparse(self.path).query)
            try: r = trail(int(q.get('slot', ['-1'])[0]), q.get('map', [''])[0])
            except ValueError: r = None
            b = json.dumps(r or {'points': [], 'deaths': [], 'kills': [], 'none': True}).encode()
            self.send_response(200); self.send_header('Content-Type', 'application/json'); self.send_header('Content-Length', str(len(b))); self.send_header('Cache-Control', 'no-cache'); self.end_headers(); self.wfile.write(b)
        else: self.relay()
    def do_POST(self): self.relay(self.rfile.read(int(self.headers.get('Content-Length') or 0)))
    def log_message(self, *a): pass

def users_game():   # -> the pids of the user's own games (not walkers / servers)
    r = subprocess.run(['pgrep', '-fl', '^/Applications/Project Reclaimer.app/Contents/MacOS/project-reclaimer'], capture_output=True, text=True)
    return {l.split()[0] for l in r.stdout.splitlines() if l.strip() and '--headless' not in l and '--server' not in l and '--server-worker' not in l}

class S(socketserver.ThreadingMixIn, http.server.HTTPServer): daemon_threads = True; allow_reuse_address = True

def port_free():
    import socket
    k = socket.socket(); k.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try: k.bind(('127.0.0.1', LISTEN)); return True
    except OSError: return False
    finally: k.close()

def main():
    # (2026-11-09: it used to QUIT when the user's game started -- and the user's game, opened straight from /Applications, has no stats
    #  page of its own: "game stats disappeared". Now: a new game of the user -> the port is let go for 30 s, so a game with nhook can take
    #  it at its start; nobody took it (a game without nhook) or the game ended -> the relay serves again. It never ends by itself)
    srv = None; yielded = set(); since = 0
    while True:
        ug = set() if STAY else users_game(); new = ug - yielded
        if srv and new:
            print('%s the user\'s game started: letting %d go for 30 s' % (time.strftime('%H:%M:%S'), LISTEN), flush=True); srv.shutdown(); srv.server_close(); srv = None
            yielded |= new; since = time.time()
        elif not srv and time.time() - since > 30 and port_free():
            srv = S(('127.0.0.1', LISTEN), H); threading.Thread(target=srv.serve_forever, daemon=True).start()
            print('%s 127.0.0.1:%d -> 127.0.0.1:%d%s' % (time.strftime('%H:%M:%S'), LISTEN, TARGET, ' (the user\'s game has no page of its own)' if ug else ''), flush=True)
            yielded |= ug
        yielded &= ug   # (ended games drop out)
        time.sleep(5)

if __name__ == '__main__':
    sys.exit(main())
