#!/usr/bin/env python3
"""Recorder and renderer for shim/tools/speedstep-demo.sh (see there). Talks only to 127.0.0.1 stats pages: the local
test client's (it asks it to step its own player) and the local dedicated server's (where the player really is).
Refuses any other address."""
import argparse, csv, json, math, os, subprocess, sys, time, urllib.request

def get(addr, path, timeout=2):
    with urllib.request.urlopen('http://%s%s' % (addr, path), timeout=timeout) as r:
        return json.loads(r.read().decode('utf-8', 'replace'))

def post(addr, path):
    rq = urllib.request.Request('http://%s%s' % (addr, path), data=b'', method='POST', headers={'X-Reclaimer-Control': '1'})
    with urllib.request.urlopen(rq, timeout=3) as r:
        return json.loads(r.read().decode())

def me(snap):
    for p in snap.get('players', []):
        if p.get('you'):
            return p
    return None

def by_name(snap, name):
    for p in snap.get('players', []):
        if p.get('name') == name:
            return p
    return None

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--client', required=True); ap.add_argument('--server', required=True)
    ap.add_argument('--step', type=float, default=2.0); ap.add_argument('--interval', type=float, default=0.25)
    ap.add_argument('--count', type=int, default=24); ap.add_argument('--out', required=True)
    a = ap.parse_args()
    for addr in (a.client, a.server):
        if not addr.startswith('127.0.0.1:'):
            sys.exit('local only: %s is not 127.0.0.1' % addr)
    log = lambda m: print('  ' + m, flush=True)

    # 1. wait for the match: the client's own player alive, and the server holding a player of that name
    t0 = time.time(); name = None
    while time.time() - t0 < 300:
        try:
            c = get(a.client, '/stats.json'); s = get(a.server, '/stats.json')
            p = me(c)
            if c.get('in_match') and p and p.get('alive') and by_name(s, p['name']):
                name = p['name']; break
        except Exception:
            pass
        time.sleep(1)
    if not name:
        sys.exit('the client did not get into the match within 5 minutes')
    log('in the match as "%s" on %s' % (name, c.get('map')))

    rows = []; phase = ['walk']
    def sample():
        try:
            s = by_name(get(a.server, '/stats.json', 1), name); c = me(get(a.client, '/stats.json', 1))
        except Exception:
            return
        if s and c:
            rows.append([round(time.time() - t0, 3), phase[0]] + [round(v, 3) for v in s['pos']] + [round(v, 3) for v in c['pos']])

    # 2. the scripted walk runs 3..9 s after the match starts: record it, then stand still briefly
    t_walk = time.time()
    while time.time() - t_walk < 9:
        sample(); time.sleep(0.1)
    phase[0] = 'stand'
    for _ in range(20):
        sample(); time.sleep(0.1)

    # 3. client-side steps along the player's facing
    c = me(get(a.client, '/stats.json')); f = math.radians(c.get('facing') or 0.0)
    dx, dy = a.step * math.cos(f), a.step * math.sin(f)
    log('stepping %d x %.2f units every %.2f s (client-side test hook)' % (a.count, a.step, a.interval))
    phase[0] = 'step'
    for i in range(a.count):
        try:
            post(a.client, '/control?do=tpby&dx=%.3f&dy=%.3f' % (dx, dy))
        except Exception as e:
            log('step %d: %s' % (i, e))
        end = time.time() + a.interval
        while time.time() < end:
            sample(); time.sleep(0.05)
    phase[0] = 'after'
    for _ in range(30):
        sample(); time.sleep(0.1)

    with open(os.path.join(a.out, 'positions.csv'), 'w', newline='') as fh:
        w = csv.writer(fh); w.writerow(['t', 'phase', 'server_x', 'server_y', 'server_z', 'client_x', 'client_y', 'client_z'])
        w.writerows(rows)

    # 4. speeds the server accepted (horizontal, units per second) per phase
    def speed(ph):
        r = [x for x in rows if x[1] == ph]
        if len(r) < 2:
            return 0.0, 0.0
        d = sum(math.hypot(r[i][2] - r[i - 1][2], r[i][3] - r[i - 1][3]) for i in range(1, len(r)))
        return d / max(r[-1][0] - r[0][0], 1e-3), d
    vw, dw = speed('walk'); vs, ds = speed('step')
    summary = ('Speed-step demonstration on a local Project Reclaimer dedicated server (%s, map %s)\n'
               'Positions are the SERVER\'s (its own memory, read by the loader\'s stats page at ~10 Hz).\n'
               'walking (stick input):          %.2f units/s over %.1f units\n'
               'client-side steps of %.2f units: %.2f units/s over %.1f units (%d steps, every %.2f s)\n'
               'ratio: %.1fx the walking speed, accepted by the server without a correction.\n'
               % (time.strftime('%Y-%m-%d %H:%M'), c.get('map'), vw, dw, a.step, vs, ds, a.count, a.interval, vs / vw if vw > 0 else float('nan')))
    open(os.path.join(a.out, 'summary.txt'), 'w').write(summary); print(summary)
    render(a, rows, c.get('map'), vw, vs)

def render(a, rows, mapname, vw, vs):
    from PIL import Image, ImageDraw, ImageFont
    mt = os.path.expanduser('~/Library/Application Support/ReclaimerMac/maptop/%s' % mapname)
    try:
        meta = json.load(open(mt + '.json')); base = Image.open(mt + '.png').convert('RGB')
    except Exception as e:
        print('  no map picture for %s (%s): no video' % (mapname, e)); return
    xs = [r[2] for r in rows]; ys = [r[3] for r in rows]; pad = 15
    x0, x1, y0, y1 = min(xs) - pad, max(xs) + pad, min(ys) - pad, max(ys) + pad
    ppu = meta['px_per_unit']
    def px(x, y): return ((x - meta['x0']) * ppu, (meta['y1'] - y) * ppu)
    (cx0, cy0), (cx1, cy1) = px(x0, y1), px(x1, y0)
    crop = base.crop((int(cx0), int(cy0), int(cx1), int(cy1))); sc = 720 / max(crop.size)
    crop = crop.resize((max(1, int(crop.size[0] * sc)), max(1, int(crop.size[1] * sc))))
    def cp(x, y):
        X, Y = px(x, y); return ((X - cx0) * sc, (Y - cy0) * sc)
    try:
        font = ImageFont.truetype('/System/Library/Fonts/Supplemental/Arial.ttf', 18)
    except Exception:
        font = ImageFont.load_default()
    fdir = os.path.join(a.out, 'frames'); os.makedirs(fdir, exist_ok=True)
    col = {'walk': (60, 160, 255), 'stand': (200, 200, 200), 'step': (255, 60, 60), 'after': (200, 200, 200)}
    W = crop.size[0]; H = crop.size[1] + 110
    t_start, t_end = rows[0][0], rows[-1][0]; n = int((t_end - t_start) * 10) + 1
    for fi in range(n):
        t = t_start + fi / 10.0; shown = [r for r in rows if r[0] <= t]
        im = Image.new('RGB', (W, H), (16, 16, 16)); im.paste(crop, (0, 0)); dr = ImageDraw.Draw(im)
        for i in range(1, len(shown)):
            dr.line([cp(shown[i - 1][2], shown[i - 1][3]), cp(shown[i][2], shown[i][3])], fill=col[shown[i][1]], width=3)
        if shown:
            X, Y = cp(shown[-1][2], shown[-1][3]); dr.ellipse([X - 6, Y - 6, X + 6, Y + 6], outline=(255, 255, 255), width=2)
            last = [r for r in shown if r[0] >= t - 1.0]
            v = (sum(math.hypot(last[i][2] - last[i - 1][2], last[i][3] - last[i - 1][3]) for i in range(1, len(last))) /
                 max(last[-1][0] - last[0][0], 1e-3)) if len(last) > 1 else 0.0
            ph = shown[-1][1]
        else:
            v, ph = 0.0, 'walk'
        y = crop.size[1] + 8
        dr.text((10, y), 'Server-side position of the player (local dedicated server, %s)' % mapname, fill=(230, 230, 230), font=font)
        dr.text((10, y + 26), 't = %5.1f s   phase: %-5s   speed (last 1 s): %5.2f units/s' % (t - t_start, ph, v), fill=col.get(ph, (230, 230, 230)), font=font)
        dr.text((10, y + 52), 'blue = normal walking (%.2f u/s)   red = client-side %.1f-unit steps (%.2f u/s, %.1fx)' % (vw, a.step, vs, vs / vw if vw > 0 else 0),
                fill=(230, 230, 230), font=font)
        im.save(os.path.join(fdir, 'f%05d.png' % fi))
    out = os.path.join(a.out, 'speedstep-demo.mp4')
    r = subprocess.run(['ffmpeg', '-y', '-loglevel', 'error', '-framerate', '10', '-i', os.path.join(fdir, 'f%05d.png'),
                        '-vf', 'pad=ceil(iw/2)*2:ceil(ih/2)*2', '-c:v', 'libx264', '-pix_fmt', 'yuv420p', out])
    print('  video: %s' % (out if r.returncode == 0 else 'ffmpeg failed (frames are in %s)' % fdir))

if __name__ == '__main__':
    main()
