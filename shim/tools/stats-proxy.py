#!/usr/bin/env python3
"""Serve another stats page on a port (2026-11-09; the user, their game closed with the walker playing on: "I need to see the game stats for
the walker so ... I can still see http://127.0.0.1:8765/"): a relay on 127.0.0.1:<listen> to 127.0.0.1:<target> (GET and POST, bodies and
headers passed through, Cache-Control too) -- the walker's own page (8771) under the user's usual address. This Mac only. When a game of
the user's starts (a Project Reclaimer without --headless) it lets the port go for 30 s -- a game started with nhook binds 8765 then --
and serves again once the port is free (the game had no page of its own, or it ended).
--stay: never step aside (the user, 11:40: "the stats server should stay tied to the walker for now").
    python3 shim/tools/stats-proxy.py [--listen 8765] [--target 8771] [--stay]"""
import http.server, os, socketserver, subprocess, sys, threading, time, urllib.request

def arg(k, d):
    a = sys.argv; return a[a.index(k) + 1] if k in a else d
LISTEN, TARGET = int(arg('--listen', '8765')), int(arg('--target', '8771')); STAY = '--stay' in sys.argv

class H(http.server.BaseHTTPRequestHandler):
    def relay(self, body=None):
        req = urllib.request.Request('http://127.0.0.1:%d%s' % (TARGET, self.path), data=body, method=self.command)
        for k in ('Content-Type', 'X-Reclaimer-Control'):
            if self.headers.get(k): req.add_header(k, self.headers[k])
        try:
            with urllib.request.urlopen(req, timeout=10) as r: data = r.read(); code = r.status; ct = r.headers.get('Content-Type', 'application/octet-stream'); cc = r.headers.get('Cache-Control')
        except urllib.error.HTTPError as e: data = e.read(); code = e.code; ct = e.headers.get('Content-Type', 'text/plain'); cc = None
        except Exception as e: data = ('relay: %s' % e).encode(); code = 502; ct = 'text/plain'; cc = None
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
                self.wfile.write(line); self.wfile.flush()
        except Exception: pass
    def do_GET(self):
        if self.path.startswith('/events'): self.stream()
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
