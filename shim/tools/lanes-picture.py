#!/usr/bin/env python3
"""A map's lanes as a picture (2026-11-08): the stats page's Lanes view (shim/src/stats_page.html buildLanes) drawn offline over
the map picture -- the same smoothing, the ruts relative to the map's own traffic (typical crossed spot -> busiest half percent,
log scale), deeper overall as traffic grows (against 400 passes), lit from the top left, worn-earth floor, dust wash below the
typical spot. For reports (shim/tools/engage-report.py --lanes).
    python3 shim/tools/lanes-picture.py <map> <out.png> [--width 900] [--vehicles] [--heatmaps DIR]"""
import importlib.util, json, math, os, sys
import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__)); SUP = os.path.expanduser('~/Library/Application Support/ReclaimerMac')
def arg(k, d):
    a = sys.argv; return a[a.index(k) + 1] if k in a else d
MAP, OUT = sys.argv[1], sys.argv[2]; W = int(arg('--width', '900')); HM = arg('--heatmaps', os.path.join(SUP, 'heatmaps')); VEH = '--vehicles' in sys.argv
spec = importlib.util.spec_from_file_location('li', os.path.join(HERE, 'lanes-import.py')); li = importlib.util.module_from_spec(spec); _a = sys.argv; sys.argv = ['x']; spec.loader.exec_module(li); sys.argv = _a
g, src, since = li.read_lanes(os.path.join(HM, MAP + '.lanes'))
if not g: sys.exit('no lanes for %s' % MAP)
CS = 0.5
mj = os.path.join(SUP, 'maptop', MAP + '.json'); mp = os.path.join(SUP, 'maptop', MAP + '.png')
xs = np.array([k[0] for k in g]) * CS; ys = np.array([k[1] for k in g]) * CS
x0, x1 = np.percentile(xs, 0.5) - 4, np.percentile(xs, 99.5) + 4; y0, y1 = np.percentile(ys, 0.5) - 4, np.percentile(ys, 99.5) + 4
pic = None
if os.path.exists(mj) and os.path.exists(mp):
    j = json.load(open(mj)); pic = (Image.open(mp).convert('RGBA'), j)
    x0, x1, y0, y1 = min(x0, j['x0']), max(x1, j['x1']), min(y0, j['y0']), max(y1, j['y1'])
sc = W / (x1 - x0); H = int(math.ceil((y1 - y0) * sc))
k = max(1, int(0.6 / (CS * sc))); cs = CS * k
X0, Y0 = int(math.floor(x0 / cs)) - 8, int(math.floor(y0 / cs)) - 8; gw, gh = int(math.ceil(x1 / cs)) + 8 - X0 + 1, int(math.ceil(y1 / cs)) + 8 - Y0 + 1
F = np.zeros((gh, gw))
for (cx, cy), (fo, ve, fx, fy) in g.items():
    v = fo + (ve if VEH else 0)
    if v <= 0: continue
    gx, gy = cx // k - X0, cy // k - Y0
    if 0 <= gx < gw and 0 <= gy < gh: F[gy, gx] += v / k
s = max(0.9, 0.55 / cs); R = int(math.ceil(s * 3)); kern = np.exp(-np.arange(-R, R + 1) ** 2 / (2 * s * s)); kern /= kern.sum()
F = np.apply_along_axis(lambda r: np.convolve(r, kern, mode='same'), 1, F); F = np.apply_along_axis(lambda c: np.convolve(c, kern, mode='same'), 0, F)
F *= math.sqrt(2 * math.pi) * s
vals = F[F > 0.5]; lo = max(1.0, float(np.percentile(vals, 50))) if vals.size else 1.0; hi = max(lo * 4, float(np.percentile(vals, 99.5))) if vals.size else 4.0
grow = min(1.0, 0.3 + 0.7 * math.log1p(hi) / math.log1p(400))
D = np.where(F > lo, np.minimum(1.0, np.log(np.maximum(F, 1e-9) / lo) / math.log(hi / lo)) * grow, 0.0)
Wsh = np.where(F > 0.05, np.minimum(1.0, np.log1p(F) / math.log1p(lo)), 0.0)
gx_, gy_ = np.gradient(D) [1], np.gradient(D)[0]
gfac = 0.9 / cs; hx, hy = gx_ * gfac, gy_ * gfac; n = 1 / np.sqrt(hx * hx + hy * hy + 1)
Lx, Ly, Lz = -0.55, 0.55, 0.63; S = (hx * Lx + hy * Ly + Lz) * n - Lz
# screen pixels -> world -> grid (bilinear by nearest+interp via map_coordinates-free sampling)
sx = np.arange(W) + 0.5; sy = np.arange(H) + 0.5
wx = x0 + sx / sc; wy = y1 - sy / sc
fx = wx / cs - X0 - 0.5; fy = wy / cs - Y0 - 0.5
ix = np.clip(np.floor(fx).astype(int), 0, gw - 2); iy = np.clip(np.floor(fy).astype(int), 0, gh - 2); tx = np.clip(fx - ix, 0, 1); ty = np.clip(fy - iy, 0, 1)
def samp(A):
    a00 = A[iy[:, None], ix[None, :]]; a01 = A[iy[:, None], ix[None, :] + 1]; a10 = A[iy[:, None] + 1, ix[None, :]]; a11 = A[iy[:, None] + 1, ix[None, :] + 1]
    return (a00 * (1 - tx[None, :]) + a01 * tx[None, :]) * (1 - ty[:, None]) + (a10 * (1 - tx[None, :]) + a11 * tx[None, :]) * ty[:, None]
d, w, sh = samp(D), samp(Wsh), samp(S)
t = np.power(d, 0.8); r = 222 - 94 * t; gg = 196 - 116 * t; b = 150 - 110 * t
a = np.maximum(0.05 + 0.17 * w, np.where(d > 0.01, 0.3 + 0.62 * np.power(d, 0.7), 0))
f = np.minimum(1, np.maximum(sh, 0) * 2.6); r = r + (255 - r) * f; gg = gg + (244 - gg) * f; b = b + (222 - b) * f; a = np.minimum(1, a + 0.25 * f)
f2 = np.minimum(1, np.maximum(-sh, 0) * 2.8); r = r * (1 - 0.85 * f2); gg = gg * (1 - 0.88 * f2); b = b * (1 - 0.9 * f2); a = np.minimum(1, a + 0.35 * f2)
a = np.where((d < 0.01) & (w < 0.08), 0, a)
base = Image.new('RGBA', (W, H), (11, 11, 11, 255))
if pic:
    im, j = pic; ax, ay = int((j['x0'] - x0) * sc), int((y1 - j['y1']) * sc); bw, bh = int((j['x1'] - j['x0']) * sc), int((j['y1'] - j['y0']) * sc)
    m = im.resize((max(1, bw), max(1, bh))); m.putalpha(Image.eval(m.getchannel('A'), lambda v: int(v * 0.85))); base.alpha_composite(m, (ax, ay))
ov = np.dstack([r, gg, b, a * 255]).clip(0, 255).astype(np.uint8)
base.alpha_composite(Image.fromarray(ov))
base.convert('RGB').save(OUT, optimize=True)
tot = sum(c[0] for c in g.values())
print('%s: %dx%d, %.1f km on foot, typical spot %.1f passes, busiest %.0f+, %.0f%% deep -> %s' % (MAP, W, H, tot * CS * 3.048 / 1000, lo, hi, grow * 100, OUT))
