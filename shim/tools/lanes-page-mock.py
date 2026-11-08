#!/usr/bin/env python3
"""The stats page's map with a REAL map's data and no game (2026-11-08, for the heatmap's Lanes view): serves the page from the
checkout at /, a snapshot on /events + /stats.json (no players unless --walkers: two dots walking the busiest lane cells),
/heat.json from <heatmaps>/<map>.bin (RHM2, player names blanked), /lanes.json from <map>.lanes (shim/native/nlanes.c's RLN1:
scope=all = the file, scope=match = a random third of its cells, so both scopes differ), /maptop/<file> from ReclaimerMac/maptop.
    python3 shim/tools/lanes-page-mock.py [--map bunkerworld] [--port 8798] [--heatmaps DIR] [--walkers]
then open http://127.0.0.1:8798/. Never port 8765 (the live game's)."""
import argparse, importlib.util, json, math, os, random, struct, sys, time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlsplit, parse_qs

HERE = os.path.dirname(os.path.abspath(__file__)); SUP = os.path.expanduser('~/Library/Application Support/ReclaimerMac')
PAGE = os.path.join(HERE, '..', 'src', 'stats_page.html')
spec = importlib.util.spec_from_file_location('li', os.path.join(HERE, 'lanes-import.py')); li = importlib.util.module_from_spec(spec)
_argv = sys.argv; sys.argv = ['lanes-import.py']; spec.loader.exec_module(li); sys.argv = _argv

def heat_json(m, d):
    out = {'maptop': 'ready', 'map': m, 'cell': 0.5, 'match': 1, 'cells': [], 'deaths': [], 'spawns': []}
    try: b = open(os.path.join(d, m + '.bin'), 'rb').read()
    except OSError: return out
    if b[:4] != b'RHM2': return out
    cell, nc, nd, mm = struct.unpack_from('<fIII', b, 4); o = 20
    for i in range(nc): x, y, n = struct.unpack_from('<iiI', b, o); o += 12; out['cells'] += [x, y, n]
    for i in range(nd):
        x, y, z, team, slot = struct.unpack_from('<fffii', b, o); match, t = struct.unpack_from('<If', b, o + 20 + 48); o += 76
        out['deaths'].append([round(x, 2), round(y, 2), round(z, 2), team, '', match, round(t), slot])
    out['match'] = mm
    return out

def lanes_json(m, d, scope):
    g, src, since = li.read_lanes(os.path.join(d, m + '.lanes')); cells = []; rnd = random.Random(7); foot = veh = 0.0
    for (x, y), (f, v, fx, fy) in g.items():
        if scope == 'match' and rnd.random() > 0.33: continue
        foot += f; veh += v; cells += [x, y, round(f * 10), round(v * 10), round(fx * 10), round(fy * 10)]
    return {'map': m, 'cell': 0.5, 'scope': scope, 'on': True, 'since': since, 'imported': len(src), 'cells': cells,
            'foot_km': round(foot * 0.5 * 3.048 / 1000, 2), 'vehicle_km': round(veh * 0.5 * 3.048 / 1000, 2)}

WALK = []
def snapshot():
    players = []
    if ARGS.walkers and WALK:
        t = time.time()
        for k in range(2):
            i = int(t * 4 + k * 37) % len(WALK); x, y = WALK[i]
            players.append({'slot': k, 'name': 'Walker %d' % (k + 1), 'team': k, 'team_name': ['red', 'blue'][k], 'alive': True, 'you': k == 0, 'shield': 1, 'health': 1,
                            'weapon': 'Assault Rifle', 'weapons': ['Assault Rifle'], 'kills': 0, 'deaths': 0, 'shots': 0, 'hits': 0, 'accuracy': 0,
                            'grenades': {'frag': 2, 'plasma': 0, 'spike': 0, 'firebomb': 0}, 'vehicle': None, 'pos': [x, y, 0], 'speed': 2, 'top_speed': 2,
                            'traits': {}, 'effects': [], 'camo': 0, 'life': 1, 'longest_life': 1, 'distance': 1, 'facing': 0, 'handle': '%08x' % (0xe0000000 + k)})
    o = {'hitboxes': False, 'vehicles': False, 'tracers': False, 'info': False, 'items': False, 'zones': False, 'visible': False, 'teams': 'all', 'teleport': False,
         'host': False, 'tp_forward': False, 'controls': False, 'text_px': 12, 'spar': False, 'spar_aim': -1.0, 'spar_available': False, 'bounds_off': False,
         'invwalls_off': False, 'mic': 'live', 'mic_in_use': False, 'spots': []}
    return {'time': 120.0, 'in_match': True, 'inst': 4243, 'feed_seq': 0, 'map': ARGS.map, 'native': True, 'lanes_live': True, 'zone_show': 0, 'weak_show': 7,
            'overlay': o, 'players': players, 'items': [], 'vehicles': [], 'projectiles': [], 'session': [], 'variant': None, 'mute': False}

class H(BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'
    def log_message(self, fmt, *a): pass
    def send(self, code, body, ctype='application/json'):
        b = body if isinstance(body, bytes) else body.encode()
        self.send_response(code); self.send_header('Content-Type', ctype); self.send_header('Content-Length', str(len(b))); self.send_header('Connection', 'close'); self.end_headers(); self.wfile.write(b)
    def do_POST(self): self.send(200, '{"ok":true}')
    def do_GET(self):
        u = urlsplit(self.path); p = u.path; q = parse_qs(u.query)
        if p == '/': return self.send(200, open(PAGE, 'rb').read(), 'text/html; charset=utf-8')
        if p == '/stats.json': return self.send(200, json.dumps(snapshot()))
        if p == '/feed.json': return self.send(200, '{"seq":0,"events":[]}')
        if p == '/heat.json': return self.send(200, json.dumps(heat_json(ARGS.map, ARGS.heatmaps)))
        if p == '/lanes.json': return self.send(200, json.dumps(lanes_json(ARGS.map, ARGS.heatmaps, (q.get('scope') or ['all'])[0])))
        if p == '/option-map.json': return self.send(200, '[]')
        if p.startswith('/maptop/') and '..' not in p:
            fp = os.path.join(SUP, 'maptop', p[8:])
            if os.path.isfile(fp):
                ct = 'image/png' if fp.endswith('.png') else 'application/json' if fp.endswith('.json') else 'application/octet-stream'
                return self.send(200, open(fp, 'rb').read(), ct)
            return self.send(404, '')
        if p == '/events':
            self.send_response(200); self.send_header('Content-Type', 'text/event-stream'); self.send_header('Cache-Control', 'no-cache'); self.end_headers()
            try:
                while True: self.wfile.write(('data: ' + json.dumps(snapshot()) + '\n\n').encode()); self.wfile.flush(); time.sleep(0.25)
            except (BrokenPipeError, ConnectionResetError): self.close_connection = True; return
        return self.send(404, '')

class Srv(ThreadingHTTPServer):
    daemon_threads = True
    def handle_error(self, request, client_address):
        if isinstance(sys.exc_info()[1], (BrokenPipeError, ConnectionResetError)): return
        super().handle_error(request, client_address)

def main():
    global ARGS
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--map', default='bunkerworld'); ap.add_argument('--port', type=int, default=8798)
    ap.add_argument('--heatmaps', default=os.path.join(SUP, 'heatmaps')); ap.add_argument('--walkers', action='store_true')
    ARGS = ap.parse_args()
    if ARGS.port == 8765: sys.exit("port 8765 is the live game's stats server: pick another")
    if ARGS.walkers:
        g, _, _ = li.read_lanes(os.path.join(ARGS.heatmaps, ARGS.map + '.lanes'))
        WALK.extend(((x + 0.5) * 0.5, (y + 0.5) * 0.5) for (x, y), c in sorted(g.items(), key=lambda kv: -kv[1][0])[:400])
    sys.stderr.write('lanes page mock (%s): http://127.0.0.1:%d/\n' % (ARGS.map, ARGS.port))
    Srv(('127.0.0.1', ARGS.port), H).serve_forever()

if __name__ == '__main__':
    main()
