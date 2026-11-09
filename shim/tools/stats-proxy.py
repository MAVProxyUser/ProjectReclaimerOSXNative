#!/usr/bin/env python3
"""Serve another stats page on a port (2026-11-09; the user, his game closed with the walker playing on: "I need to see the game stats for the
walker so ... I can still see http://127.0.0.1:8765/"): a relay on 127.0.0.1:<listen> to 127.0.0.1:<target> (GET and POST, bodies and
headers passed through) -- the walker's own page (8771) under the user's usual address. This Mac only. It steps aside by itself when the
port is wanted back: every 5 s it checks whether a Project Reclaimer game without --headless is running, and quits if so (the user's
game binds 8765 at its start).
    python3 shim/tools/stats-proxy.py [--listen 8765] [--target 8771]"""
import http.server, os, socketserver, subprocess, sys, threading, time, urllib.request

def arg(k, d):
    a = sys.argv; return a[a.index(k) + 1] if k in a else d
LISTEN, TARGET = int(arg('--listen', '8765')), int(arg('--target', '8771'))

class H(http.server.BaseHTTPRequestHandler):
    def relay(self, body=None):
        req = urllib.request.Request('http://127.0.0.1:%d%s' % (TARGET, self.path), data=body, method=self.command)
        for k in ('Content-Type', 'X-Reclaimer-Control'):
            if self.headers.get(k): req.add_header(k, self.headers[k])
        try:
            with urllib.request.urlopen(req, timeout=10) as r: data = r.read(); code = r.status; ct = r.headers.get('Content-Type', 'application/octet-stream')
        except urllib.error.HTTPError as e: data = e.read(); code = e.code; ct = e.headers.get('Content-Type', 'text/plain')
        except Exception as e: data = ('relay: %s' % e).encode(); code = 502; ct = 'text/plain'
        self.send_response(code); self.send_header('Content-Type', ct); self.send_header('Content-Length', str(len(data))); self.end_headers(); self.wfile.write(data)
    def do_GET(self): self.relay()
    def do_POST(self): self.relay(self.rfile.read(int(self.headers.get('Content-Length') or 0)))
    def log_message(self, *a): pass

def users_game():
    r = subprocess.run(['pgrep', '-fl', '^/Applications/Project Reclaimer.app/Contents/MacOS/project-reclaimer'], capture_output=True, text=True)
    return any('--headless' not in l and '--server' not in l and '--server-worker' not in l for l in r.stdout.splitlines() if l.strip())

class S(socketserver.ThreadingMixIn, http.server.HTTPServer): daemon_threads = True; allow_reuse_address = True

def main():
    if users_game(): print('a game of the user runs: it serves %d itself' % LISTEN); return 0
    srv = S(('127.0.0.1', LISTEN), H); threading.Thread(target=srv.serve_forever, daemon=True).start()
    print('%s 127.0.0.1:%d -> 127.0.0.1:%d' % (time.strftime('%H:%M:%S'), LISTEN, TARGET), flush=True)
    while True:
        time.sleep(5)
        if users_game(): print('%s the user\'s game is back: stepping aside' % time.strftime('%H:%M:%S'), flush=True); srv.shutdown(); return 0

if __name__ == '__main__':
    sys.exit(main())
