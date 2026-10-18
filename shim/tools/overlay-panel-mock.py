#!/usr/bin/env python3
"""Offline mock of the stats server for the overlay panel (shim/src/stats_page.html) -- no game needed.

Serves the page at /, a fake snapshot on /events (server-sent events, like the loader) and /stats.json, empty
/feed.json, /heat.json and /option-map.json, and answers POST /control (header X-Reclaimer-Control) the way
shim/src/stats.c + hitbox.c do for the overlay:
  do=feat&f=<feature>[,<feature>...]|all [&on=0|1] [&team=all|enemies|friends] [&vis=0|1] [&px=4..60]
      each setting lands only where the feature has it (null in the snapshot = not applicable); 400 on bad input
  do=teams / do=visible / do=textup / do=textdown   the old global switches = "apply to all"
  do=hitboxes|tracers|info|items|zones|vehicles     toggle one feature
  do=zonetype&t=<bit> / do=weaktype&t=<bit>         the chips
  do=bounds&off=0|1 / do=invwalls&off=0|1            the host switches (Boundaries / Invisible walls buttons)
  anything else (capture, mic, teleport, ...)        ok, nothing happens

  python3 shim/tools/overlay-panel-mock.py [--port 8799] [--lag 0.8] [--readonly] [--old] [--refuse]
  then open http://127.0.0.1:8799/

--lag delays applying each control request (tests the page's optimistic update), --readonly answers like
PELOAD_STATS_READONLY=1, --old sends a snapshot without overlay.features (an older loader), --refuse answers every
do=feat with 400 (the page must fall back to the real state). Never use port 8765: that is the live game's.
"""
import argparse, json, os, sys, threading, time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlsplit

HERE = os.path.dirname(os.path.abspath(__file__))
PAGE = os.path.join(HERE, '..', 'src', 'stats_page.html')
ORDER = ['hitboxes', 'vehicles', 'tracers', 'info', 'items', 'zones']
# which settings each feature has (hitbox.c hbs_init: team, vis, px); zones' px only with PELOAD_HB_ZONES_XRAY
HAS = {'hitboxes': (1, 1, 0), 'vehicles': (1, 1, 0), 'tracers': (1, 1, 0), 'info': (1, 1, 1), 'items': (0, 1, 1), 'zones': (0, 0, 0)}
TEAMS = ['all', 'enemies', 'friends']

lock = threading.Lock()
state = {'tc': 0, 'visible': False, 'text_px': 7, 'zone_show': 31, 'weak_show': 3, 'feat': {}, 'bounds_off': False, 'invwalls_off': False}
for f in ORDER:
    t, v, p = HAS[f]
    state['feat'][f] = {'on': f in ('hitboxes', 'info'), 'team': 'all' if t else None, 'vis': False if v else None, 'px': 7 if p else None}
ARGS = None


def apply_feat(qs, dry=False):
    """st_feat: returns True (ok) / False (400); dry = only validate."""
    mask, on, team, vis, px = [], None, None, None, None
    for tok in qs.split('&'):
        if '=' not in tok:
            continue
        k, v = tok.split('=', 1)
        if k == 'f':
            for w in v.replace('%2C', ',').replace('%2c', ',').split(','):
                if w == 'all':
                    mask += ORDER
                elif w in ORDER:
                    mask.append(w)
                else:
                    return False
        elif k in ('on', 'vis'):
            if v not in ('0', '1'):
                return False
            if k == 'on':
                on = v == '1'
            else:
                vis = v == '1'
        elif k == 'team':
            if v not in TEAMS:
                return False
            team = v
        elif k == 'px':
            try:
                px = int(v)
            except ValueError:
                return False
            if px < 4 or px > 60:
                return False
    if not mask or (on is None and team is None and vis is None and px is None):
        return False
    if dry:
        return True
    with lock:
        for f in set(mask):
            s = state['feat'][f]
            if on is not None:
                s['on'] = on
            if team is not None and s['team'] is not None:
                s['team'] = team
            if vis is not None and s['vis'] is not None:
                s['vis'] = vis
            if px is not None and s['px'] is not None:
                s['px'] = px
    return True


def control(act, qs):
    if act == 'feat':
        return apply_feat(qs)
    with lock:
        if act in ORDER:
            state['feat'][act]['on'] = not state['feat'][act]['on']
        elif act == 'teams':   # apply to all (hbs_sync)
            state['tc'] += 1
            for s in state['feat'].values():
                if s['team'] is not None:
                    s['team'] = TEAMS[state['tc'] % 3]
        elif act == 'visible':
            state['visible'] = not state['visible']
            for s in state['feat'].values():
                if s['vis'] is not None:
                    s['vis'] = state['visible']
        elif act in ('textup', 'textdown'):
            state['text_px'] = max(4, min(60, state['text_px'] + (1 if act == 'textup' else -1)))
            for s in state['feat'].values():
                if s['px'] is not None:
                    s['px'] = state['text_px']
        elif act in ('bounds', 'invwalls'):   # host switches (games you host with PELOAD_ALLOW_TELEPORT=1)
            v = dict(x.split('=', 1) for x in qs.split('&') if '=' in x).get('off', '')
            if v not in ('0', '1'):
                return False
            state[act + '_off'] = v == '1'
        elif act in ('zonetype', 'weaktype'):
            t = dict(x.split('=', 1) for x in qs.split('&') if '=' in x).get('t', '')
            ok = {'zonetype': ('1', '2', '4', '8', '16'), 'weaktype': ('1', '2')}[act]
            if t not in ok:
                return False
            key = 'zone_show' if act == 'zonetype' else 'weak_show'
            state[key] ^= int(t)
    return True


def player(slot, name, team, x, y, you=False):
    tn = ['red', 'blue'][team]
    return {'slot': slot, 'name': name, 'team': team, 'team_name': tn, 'alive': True, 'you': you, 'host': slot == 0, 'talking': False,
            'muted': False, 'voice': '', 'shield': 1.0, 'health': 0.8, 'weapon': 'Assault Rifle', 'weapons': ['Assault Rifle', 'Magnum'],
            'kills': 3 - slot, 'deaths': slot, 'shots': 40, 'hits': 12, 'accuracy': 0.3, 'grenades': {'frag': 2, 'plasma': 0, 'spike': 0, 'firebomb': 0},
            'vehicle': '', 'pos': [x, y, 2.0], 'speed': 0.0, 'top_speed': 2.1, 'traits': {}, 'effects': [], 'camo': 0, 'life': 30, 'longest_life': 60,
            'distance': 120, 'facing': 90, 'handle': '0x%08x' % (0xe0000000 + slot)}


def snapshot():
    with lock:
        feats = json.loads(json.dumps(state['feat']))
        o = {k: feats[k]['on'] for k in ORDER}
        o.update({'visible': state['visible'], 'teams': TEAMS[state['tc'] % 3], 'teleport': True, 'host': True, 'tp_forward': False,
                  'controls': not ARGS.readonly, 'text_px': state['text_px'], 'spar': False, 'spar_aim': -1.0, 'spar_available': False,
                  'bounds_off': state['bounds_off'], 'invwalls_off': state['invwalls_off'], 'mic': 'live', 'mic_in_use': False, 'spots': []})
        if not ARGS.old:
            o['features'] = feats
        return {'time': time.time() % 600, 'in_match': True, 'inst': 4242, 'feed_seq': 0, 'map': 'riverworld',
                'zone_show': state['zone_show'], 'weak_show': state['weak_show'], 'overlay': o,
                'players': [player(0, 'You', 0, 10, 20, True), player(1, 'Target', 1, -30, 5)],
                'vehicles': [], 'projectiles': [], 'session': [], 'variant': None, 'mute': False}


class H(BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'

    def log_message(self, fmt, *a):
        if ARGS.verbose:
            sys.stderr.write('  [mock] ' + (fmt % a) + '\n')

    def send(self, code, body, ctype='application/json'):
        b = body if isinstance(body, bytes) else body.encode()
        self.send_response(code)
        self.send_header('Content-Type', ctype)
        self.send_header('Content-Length', str(len(b)))
        self.send_header('Connection', 'close')
        self.end_headers()
        self.wfile.write(b)

    def do_POST(self):
        u = urlsplit(self.path)
        if u.path != '/control':
            return self.send(405, '')
        qs = u.query
        act = ''
        for tok in qs.split('&'):
            if tok.startswith('do='):
                act = ''.join(c for c in tok[3:] if 'a' <= c <= 'z')[:31]
        if not self.headers.get('X-Reclaimer-Control'):
            return self.send(403, '{"ok":false,"error":"needs POST with an X-Reclaimer-Control header"}')
        if ARGS.readonly:
            return self.send(403, '{"ok":false,"error":"controls are off"}')
        if ARGS.refuse and act == 'feat':
            return self.send(400, '{"ok":false,"error":"unknown or disabled action"}')
        sys.stderr.write('  [mock] control: %s\n' % qs)
        if ARGS.lag:
            # answer at once (like the loader), apply later: the page shows its own pending value meanwhile
            ok = apply_feat(qs, dry=True) if act == 'feat' else True   # validate now, apply later
            if ok:
                threading.Timer(ARGS.lag, control, (act, qs)).start()
        else:
            ok = control(act, qs)
        self.send(200 if ok else 400, '{"ok":true}' if ok else '{"ok":false,"error":"unknown or disabled action"}')

    def do_GET(self):
        p = urlsplit(self.path).path
        if p == '/':
            with open(PAGE, 'rb') as f:
                return self.send(200, f.read(), 'text/html; charset=utf-8')
        if p == '/stats.json':
            return self.send(200, json.dumps(snapshot()))
        if p == '/feed.json':
            return self.send(200, '{"seq":0,"events":[]}')
        if p == '/heat.json':
            return self.send(200, json.dumps({'map': 'riverworld', 'match': 1, 'cell': 0.5, 'cells': [], 'deaths': [], 'maptop': ''}))
        if p == '/option-map.json':
            return self.send(200, '[]')
        if p == '/events':
            self.send_response(200)
            self.send_header('Content-Type', 'text/event-stream')
            self.send_header('Cache-Control', 'no-cache')
            self.end_headers()
            try:
                while True:
                    self.wfile.write(('data: ' + json.dumps(snapshot()) + '\n\n').encode())
                    self.wfile.flush()
                    time.sleep(0.1)   # 10 snapshots a second, like the loader
            except (BrokenPipeError, ConnectionResetError):
                self.close_connection = True
                return
        return self.send(404, '')


class Srv(ThreadingHTTPServer):
    daemon_threads = True

    def handle_error(self, request, client_address):   # a page reload drops its event stream: not an error
        if isinstance(sys.exc_info()[1], (BrokenPipeError, ConnectionResetError)):
            return
        super().handle_error(request, client_address)


def main():
    global ARGS
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--port', type=int, default=8799)
    ap.add_argument('--lag', type=float, default=0.0, help='seconds before a control request takes effect')
    ap.add_argument('--readonly', action='store_true')
    ap.add_argument('--old', action='store_true', help='snapshot without overlay.features')
    ap.add_argument('--refuse', action='store_true', help='answer every do=feat with 400')
    ap.add_argument('--verbose', action='store_true')
    ARGS = ap.parse_args()
    if ARGS.port == 8765:
        sys.exit('port 8765 is the live game\'s stats server: pick another')
    srv = Srv(('127.0.0.1', ARGS.port), H)
    sys.stderr.write('overlay panel mock: http://127.0.0.1:%d/  (page %s)\n' % (ARGS.port, os.path.normpath(PAGE)))
    srv.serve_forever()


if __name__ == '__main__':
    main()
