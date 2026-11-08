#!/usr/bin/env python3
"""The walkers' night report (2026-11-08): one HTML page from what the night produced -- the real players' model (tracks/), the
walkers measured the same way (battles/tracks/), the training log (battles/train.jsonl) and the battle scoreboards (battles/*.jsonl).
No player names: walkers are "Walker 1..4" (by test identity), players are never named.
    [WB_TRACKS=<the walkers' folder>] [WB_STEPS="label|folder;label|folder..."] python3 shim/tools/walker-report.py [out.html]
WB_STEPS = the human-likeness after each change (walker-compare.py on each folder, the same score as its own)."""
import glob, gzip, html, json, os, subprocess, sys, tempfile, time

HERE = os.path.dirname(os.path.abspath(__file__)); SUP = os.path.expanduser('~/Library/Application Support/ReclaimerMac')
OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(SUP, 'battles', 'walker-report.html')
IDS = ['test-v2', 'test-second', 'test-third', 'test-fourth']
def nick(p):
    try: return json.load(open(os.path.join(SUP, 'native', p, 'profile.json')))['identity']['nickname']
    except Exception: return None
ALIAS = {nick(p): 'Walker %d' % (i + 1) for i, p in enumerate(IDS) if nick(p)}
def compare(d):
    out = tempfile.mktemp(suffix='.json')
    subprocess.run([sys.executable, os.path.join(HERE, 'walker-compare.py'), d, '--json', out], capture_output=True, text=True)
    try: return json.load(open(out)).get('score')
    except Exception: return None
def model(d):
    out = tempfile.mktemp(suffix='.json')
    subprocess.run([sys.executable, os.path.join(HERE, 'walker-model.py'), d, '--out', out, '--heatmaps', '/nonexistent'], capture_output=True, text=True)
    try: return json.load(open(out))
    except Exception: return None
E = html.escape
WDIR = os.environ.get('WB_TRACKS') or os.path.join(SUP, 'battles', 'tracks')   # (the walkers measured: WB_TRACKS = one set of settings)
real = model(os.path.join(SUP, 'tracks')); bots = model(WDIR)
train = [json.loads(l) for l in open(os.path.join(SUP, 'battles', 'train.jsonl'))] if os.path.exists(os.path.join(SUP, 'battles', 'train.jsonl')) else []
battles = []
for f in sorted(glob.glob(os.path.join(SUP, 'battles', '*-n*.jsonl'))):
    L = [json.loads(l) for l in open(f) if l.strip()]
    if L: battles.append((os.path.basename(f)[:15], L[-1]['players'], len(L) * 10))
best = json.load(open(os.path.join(SUP, 'battles', 'best-tactics.json'))) if os.path.exists(os.path.join(SUP, 'battles', 'best-tactics.json')) else None

def g(m, sec, k):
    try: return m[sec][k]
    except Exception: return None
def fmt(v, d=2): return '–' if v is None else ('%.*f' % (d, v))

# ---- charts (inline SVG, one scale each, colors from the theme tokens)
def pair_bars(rows):
    W, rowh, lw = 560, 34, 210; h = rowh * len(rows) + 10; out = ['<svg viewBox="0 0 %d %d" role="img" aria-label="players and walkers per measure" class="chart">' % (W, h)]
    for i, (label, a, b) in enumerate(rows):
        y = 6 + i * rowh; mx = max([x for x in (a, b) if x is not None] + [1e-9]) * 1.15; sc = (W - lw - 70) / mx
        out.append('<text x="0" y="%d" class="lab">%s</text>' % (y + 14, E(label)))
        for k, (v, cls) in enumerate(((a, 'pl'), (b, 'wk'))):
            if v is None: continue
            yy = y + k * 12; out.append('<rect x="%d" y="%d" width="%.1f" height="10" class="%s"/>' % (lw, yy + 2, max(1.5, v * sc), cls))
            out.append('<text x="%.1f" y="%d" class="val">%s</text>' % (lw + max(1.5, v * sc) + 6, yy + 11, fmt(v)))
    out.append('</svg>'); return ''.join(out)
def runs_of(train):
    r, last, out = 0, None, []
    for t in train:
        if last is not None and t['gen'] <= last: r += 1
        last = t['gen']; out.append(r + 1)
    return out
def fitness_chart(train):
    if not train: return '<p class="note">No training generations yet.</p>'
    W, H, pl, pb = 560, 220, 44, 30; gens = [t['gen'] for t in train]; bestf = [max(w['fitness'] for w in t['walkers']) for t in train]
    meanf = [sum(w['fitness'] for w in t['walkers']) / len(t['walkers']) for t in train]
    lo, hi = min(meanf + bestf + [0]), max(meanf + bestf + [1]); span = (hi - lo) or 1; n = max(1, len(train) - 1)
    X = lambda i: pl + (W - pl - 16) * i / n; Y = lambda v: 12 + (H - pb - 12) * (1 - (v - lo) / span)
    o = ['<svg viewBox="0 0 %d %d" role="img" aria-label="fitness per generation" class="chart">' % (W, H)]
    for k in range(5):
        v = lo + span * k / 4; o.append('<line x1="%d" x2="%d" y1="%.1f" y2="%.1f" class="grid"/><text x="%d" y="%.1f" class="ax" text-anchor="end">%.1f</text>' % (pl, W - 16, Y(v), Y(v), pl - 6, Y(v) + 4, v))
    rn = runs_of(train)
    for i in range(len(train)): o.append('<text x="%.1f" y="%d" class="ax" text-anchor="middle">%d</text>' % (X(i), H - 8, i + 1))
    for i in range(1, len(train)):
        if rn[i] != rn[i - 1]: o.append('<line x1="%.1f" x2="%.1f" y1="12" y2="%d" class="grid" stroke-dasharray="3 3"/><text x="%.1f" y="24" class="ax">run %d</text>' % ((X(i) + X(i - 1)) / 2, (X(i) + X(i - 1)) / 2, H - pb, (X(i) + X(i - 1)) / 2 + 4, rn[i]))
    for series, cls in ((meanf, 'ln-mean'), (bestf, 'ln-best')):
        o.append('<polyline class="%s" points="%s"/>' % (cls, ' '.join('%.1f,%.1f' % (X(i), Y(v)) for i, v in enumerate(series))))
        o.append('<circle cx="%.1f" cy="%.1f" r="4" class="%s-dot"/>' % (X(len(series) - 1), Y(series[-1]), cls))
    o.append('</svg>'); return ''.join(o)

# ---- sections
src = real['source'] if real else {}; bsrc = bots['source'] if bots else {}
summary = [('Free-for-all samples', '{:,}'.format(src.get('samples', 0)), '%s players, %s engagements' % (src.get('players', 0), src.get('engagements', 0))),
           ('Walker battles', str(len(battles)), '%d training generations' % len(train)),
           ('Kills in the last battle', str(sum((p.get('kills') or 0) for p in battles[-1][1])) if battles else '–', '%d s, 4 walkers' % battles[-1][2] if battles else ''),
           ('Champion fitness', fmt(best['fitness'], 1) if best else '–', ('mean of %s battles, %s kills / %s deaths' % (best.get('battles'), best.get('kills', '?'), best.get('deaths', '?'))) if best else '')]
cmp_rows = [('running speed (u/s)', g(real, 'move', 'speed_median'), g(bots, 'move', 'speed_median')),
            ('time standing (share)', g(real, 'move', 'stand_share'), g(bots, 'move', 'stand_share')),
            ('fight distance (u)', g(real, 'fight', 'dist_median'), g(bots, 'fight', 'dist_median')),
            ('strafe speed (u/s)', g(real, 'fight', 'strafe_rms'), g(bots, 'fight', 'strafe_rms')),
            ('strafe switch (s)', g(real, 'fight', 'strafe_switch_s'), g(bots, 'fight', 'strafe_switch_s')),
            ('aim error (deg)', g(real, 'fight', 'aim_err_median'), g(bots, 'fight', 'aim_err_median')),
            ('reaction (s)', g(real, 'fight', 'reaction_s'), g(bots, 'fight', 'reaction_s')),
            ('shots a second in fights', g(real, 'fight', 'shots_per_s'), g(bots, 'fight', 'shots_per_s'))]
human = compare(WDIR)
steps = [x.split('|', 1) for x in (os.environ.get('WB_STEPS') or '').split(';') if '|' in x]
step_sc = [(lab, compare(os.path.expanduser(d))) for lab, d in steps]
summary.insert(1, ('Human-likeness', '%.0f%%' % human if human is not None else '–', 'walkers vs players, walker-compare.py'))
rows_html = ''.join('<tr><td>%s</td><td class="n">%s</td><td class="n">%s</td></tr>' % (E(l), fmt(a), fmt(b)) for l, a, b in cmp_rows)
tact = ''
if best:
    names = {'dist': 'fighting distance (x the players\')', 'strafe': 'strafe strength (x)', 'switch': 'strafe switch time (x)', 'fire': 'aim slack when firing (deg)', 'retreat': 'retreat when shields fall below (share)', 'gren': 'seconds between grenades',
             'press': 'pressing in (throttle)', 'backoff': 'back-off on low shields (throttle)'}
    tact = ''.join('<tr><td>%s</td><td class="n">%s</td></tr>' % (E(names.get(k, k)), fmt(v)) for k, v in best['tactics'].items())
_rn = runs_of(train)
gen_rows = ''.join('<tr><td class="n">%d</td><td class="n">%d · %d</td><td class="n">%s</td><td class="n">%s</td><td class="n">%d</td></tr>' % (i + 1, _rn[i], t['gen'], fmt(max(w['fitness'] for w in t['walkers']), 1), fmt(sum(w['fitness'] for w in t['walkers']) / len(t['walkers']), 1), sum(w['kills'] for w in t['walkers'])) for i, t in enumerate(train))
last_b = ''
if battles:
    stamp, ps, secs = battles[-1]
    last_b = ''.join('<tr><td>%s</td><td class="n">%s</td><td class="n">%s</td><td class="n">%s</td></tr>' % (E(ALIAS.get(p.get('name'), 'Walker ?')), p.get('kills'), p.get('deaths'), p.get('shots')) for p in sorted(ps, key=lambda p: -(p.get('kills') or 0)))
weap = ''.join('<li><span>%s</span><span class="n">%d</span></li>' % (E(w), c) for w, c in (real.get('weapons_in_kills') or {}).items()) if real else ''

TEMPLATE = r'''<title>Walker Training Night</title>
<meta name="claude:viewport-fit-sides" content="cover">
<link rel="stylesheet" href="https://fonts.googleapis.com/css2?family=Barlow+Condensed:wght@500;600;700&family=IBM+Plex+Sans:wght@400;500;600&family=IBM+Plex+Mono:wght@400;500&display=swap">
<style>
/* layout: one reading column, a figure strip, then sections that pair a short text with its table or chart */
:root { --bg: #f6f7f4; --panel: #ffffff; --ink: #1d2422; --dim: #5d6863; --line: #d9ded9; --accent: #5f6f22; --pl: #3a6fb0; --wk: #b0621f;
  --display: "Barlow Condensed", "Arial Narrow", sans-serif; --body: "IBM Plex Sans", system-ui, sans-serif; --mono: "IBM Plex Mono", ui-monospace, monospace; }
@media (prefers-color-scheme: dark) { :root:not([data-theme="light"]) { --bg: #121614; --panel: #1a201d; --ink: #e5e9e6; --dim: #9aa59f; --line: #2c3530; --accent: #b4c66a; --pl: #78a9ec; --wk: #e49a5c; color-scheme: dark } }
:root[data-theme="dark"] { --bg: #121614; --panel: #1a201d; --ink: #e5e9e6; --dim: #9aa59f; --line: #2c3530; --accent: #b4c66a; --pl: #78a9ec; --wk: #e49a5c; color-scheme: dark }
body { background: var(--bg); color: var(--ink); font: 15px/1.55 var(--body); }
.wrap { max-width: 920px; margin: 0 auto; padding-block: 28px 56px; padding-left: max(16px, env(safe-area-inset-left, 0px)); padding-right: max(16px, env(safe-area-inset-right, 0px)); display: grid; gap: 34px; }
header { display: grid; gap: 6px; }
.eyebrow { font: 500 12px/1 var(--mono); letter-spacing: .12em; text-transform: uppercase; color: var(--accent); }
h1 { font: 700 44px/1.02 var(--display); letter-spacing: .01em; margin: 0; text-wrap: balance; }
h2 { font: 600 26px/1.1 var(--display); letter-spacing: .02em; margin: 0 0 4px; text-wrap: balance; }
p { margin: 0; max-width: 66ch; } .lede { color: var(--dim); font-size: 16px; }
.strip { display: grid; grid-template-columns: repeat(auto-fit, minmax(180px, 1fr)); gap: 1px; background: var(--line); border: 1px solid var(--line); }
.fig { background: var(--panel); padding: 14px 16px; display: grid; gap: 2px; }
.fig .k { font: 500 11px/1.2 var(--mono); letter-spacing: .08em; text-transform: uppercase; color: var(--dim); }
.fig .v { font: 600 34px/1 var(--display); font-variant-numeric: tabular-nums; }
.fig .s { font-size: 12.5px; color: var(--dim); }
section { display: grid; gap: 12px; }
.two { display: grid; grid-template-columns: minmax(0, 1fr) minmax(0, 1.2fr); gap: 22px; align-items: start; }
@media (max-width: 720px) { .two { grid-template-columns: minmax(0, 1fr); } h1 { font-size: 36px; } }
.tbl { overflow-x: auto; } table { border-collapse: collapse; width: 100%; font-size: 13.5px; }
th { font: 500 11px/1.2 var(--mono); letter-spacing: .08em; text-transform: uppercase; color: var(--dim); text-align: left; padding: 6px 8px; border-bottom: 1px solid var(--line); }
td { padding: 6px 8px; border-bottom: 1px solid var(--line); } td.n, th.n { text-align: right; font: 13px var(--mono); font-variant-numeric: tabular-nums; }
.chart { width: 100%; max-width: 100%; height: auto; display: block; }
.chart .lab { font: 12px var(--body); fill: var(--ink); } .chart .val, .chart .ax { font: 11px var(--mono); fill: var(--dim); }
.chart .pl { fill: var(--pl); } .chart .wk { fill: var(--wk); } .chart .grid { stroke: var(--line); stroke-width: 1; }
.chart .ln-best { fill: none; stroke: var(--accent); stroke-width: 2.5; } .chart .ln-mean { fill: none; stroke: var(--dim); stroke-width: 1.5; stroke-dasharray: 4 3; }
.chart .ln-best-dot { fill: var(--accent); } .chart .ln-mean-dot { fill: var(--dim); }
.key { display: flex; gap: 16px; flex-wrap: wrap; font-size: 12.5px; color: var(--dim); } .key i { display: inline-block; width: 10px; height: 10px; margin-right: 6px; vertical-align: -1px; }
.note { font-size: 13px; color: var(--dim); }
ul.w { list-style: none; padding: 0; margin: 0; display: grid; gap: 4px; max-width: 360px; } ul.w li { display: flex; justify-content: space-between; border-bottom: 1px solid var(--line); padding: 4px 0; font-size: 13.5px; }
ol.flow { margin: 0; padding-left: 20px; display: grid; gap: 6px; max-width: 70ch; }
code { font: 12.5px var(--mono); background: var(--panel); border: 1px solid var(--line); padding: 1px 5px; }
</style>
<div class="wrap">
<header><div class="eyebrow">Project Reclaimer · native 0.10.1 · @@1@@</div><h1>Walker training night</h1>
<p class="lede">Real players were recorded in public free-for-all games, their movement and fighting measured, and four walkers fought each other on a local server using those numbers while an evolutionary loop tuned their tactics.</p></header>
<div class="strip">@@2@@</div>
<section><h2>What real players do</h2><div class="two"><div style="display:grid;gap:10px">
<p>Measured from the recordings, 10 samples a second per player. A fight counts when a player aims within 10° of another within 40 units for at least 0.3 s.</p>
<p class="note">Kills in the recordings, by weapon:</p><ul class="w">@@3@@</ul></div>
<div class="tbl"><table><thead><tr><th>Measure</th><th class="n">Players</th><th class="n">Walkers</th></tr></thead><tbody>@@4@@</tbody></table></div></div></section>
<section><h2>Walkers next to players</h2><div class="key"><span><i style="background:var(--pl)"></i>real players</span><span><i style="background:var(--wk)"></i>walkers</span></div>@@5@@
<p class="note">The walkers are measured from the server's view of their battles with the same tool. Walkers: @@6@@ samples, @@7@@ engagements.</p></section>
<section><h2>Training</h2><div class="two"><div style="display:grid;gap:10px">
<p>Each generation is one five-minute free-for-all on Guardian. All four walkers share the same skill, so only tactics decide. Fitness is kills minus 0.7 × deaths plus 2 × accuracy. The best two by their last three battles carry over, and two mutants join. The champion is the set with the best mean over all its battles.</p>
<p class="note">The server counted no hits during either run (its hit meter needs rendered frames; fixed afterwards), so kills and deaths alone decided. Run 1 selected on single battles, run 2 on the mean of three.</p>
<div class="key"><span><i style="background:var(--accent)"></i>best walker</span><span><i style="background:var(--dim)"></i>average</span></div>@@8@@</div>
<div class="tbl"><table><thead><tr><th class="n">Battle</th><th class="n">Run · gen</th><th class="n">Best</th><th class="n">Average</th><th class="n">Kills</th></tr></thead><tbody>@@9@@</tbody></table>
<p class="note" style="margin-top:12px">Champion tactics:</p><table><tbody>@@10@@</tbody></table></div></div></section>
<section><h2>Human, step by step</h2><p>The same score after each change, each measured on its own battle with walker-compare.py (100% means the real players' numbers).</p>
<div class="tbl"><table><thead><tr><th>Walkers</th><th class="n">Human-likeness</th></tr></thead><tbody>@@12@@</tbody></table></div></section>
<section><h2>Last battle</h2><div class="tbl"><table><thead><tr><th>Walker</th><th class="n">Kills</th><th class="n">Deaths</th><th class="n">Shots</th></tr></thead><tbody>@@11@@</tbody></table></div></section>
<section><h2>How the walkers play on a server</h2><ol class="flow">
<li>A hook before halo3's control packet builder writes the walker's throttle and aim into the packet the server receives.</li>
<li>Firing sets the control record's trigger bit. The local weapon fires and the packet reports it, so the server counts the shots.</li>
<li>halo3's own collision ray answers "can I see him" ten times a second. The walker only fires with a clear line, and it does not track enemies through walls.</li>
<li>Distance, strafing, back-off and aim noise come from the real players' numbers. Paths prefer the lanes players actually use.</li></ol>
<p class="note">Not yet: weapon pickup (none of the control record's 64 button bits picks a weapon up), the real players' fighting distance on a small map (Guardian fights run closer than the recorded maps), and more real-player fights (the recordings hold @@13@@ engagements; Infection games count only for the lanes).</p>
<p class="note">Run it again: <code>bash shim/tools/walker-battle.sh 4 300</code>, <code>python3 shim/tools/walker-train.py 8 300 --resume</code>, <code>python3 shim/tools/walker-train.py --champion</code>, <code>python3 shim/tools/walker-compare.py</code>.</p></section>
</div>'''
VALUES = [E(time.strftime('%d %b %Y')),
             ''.join('<div class="fig"><span class="k">%s</span><span class="v">%s</span><span class="s">%s</span></div>' % (E(k), E(v), E(s)) for k, v, s in summary),
             weap or '<li><span>none yet</span><span></span></li>', rows_html, pair_bars(cmp_rows),
             '{:,}'.format(bsrc.get('samples', 0)), bsrc.get('engagements', 0), fitness_chart(train), gen_rows or '<tr><td colspan="4" class="note">none yet</td></tr>',
             tact or '<tr><td class="note">none yet</td></tr>', last_b or '<tr><td colspan="4" class="note">none yet</td></tr>',
             ''.join('<tr><td>%s</td><td class="n">%s</td></tr>' % (E(l), '%.0f%%' % v if v is not None else '–') for l, v in step_sc) or '<tr><td colspan="2" class="note">no steps given (WB_STEPS)</td></tr>',
             src.get('engagements', 0)]
page = TEMPLATE
for i, v in enumerate(VALUES, 1): page = page.replace('@@%d@@' % i, str(v))
open(OUT, 'w').write(page); print('report ->', OUT)
