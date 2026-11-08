#!/usr/bin/env python3
"""The "How players fight" page (2026-11-08): engage-model.py's report (--json) + engage-compare.py's walker comparisons as one
HTML page (no names: players are never named, walkers are "walkers").
    python3 shim/tools/engage-report.py <report.json> <out.html> [--compare "label=cmp.json" ...] [--lanes "Map=png" ...]
--compare: engage-compare.py --json outputs, in order (each a row of the walkers-vs-players table, the last one in full).
--lanes: pictures of the stats page's Lanes view (PNG files; embedded)."""
import base64, html, json, math, os, sys

def args(k):
    a = sys.argv; return [a[i + 1] for i in range(len(a) - 1) if a[i] == k]
REP, OUT = sys.argv[1], sys.argv[2]
t = open(REP).read(); R = json.loads(t[:t.rindex('}') + 1])
CMP = [(s.split('=', 1)[0], json.load(open(s.split('=', 1)[1]))) for s in args('--compare')]
LANES = [(s.split('=', 1)[0], s.split('=', 1)[1]) for s in args('--lanes')]
E = html.escape
def pct(x, d=0): return '-' if x is None else ('%.' + str(d) + 'f%%') % (100 * x)
def num(x, d=1): return '-' if x is None else ('%.' + str(d) + 'f') % x
A = R.get('all_but_infection', {}); M = A.get('multi', {}); C = R.get('by_class', {})
src = R.get('source', {})

def bars(rows, w=560, label_w=150, fmt=lambda v: pct(v), maxv=1.0, cls='pl', note=None):
    """rows: [(label, value, n)] -> horizontal bar chart"""
    h = 26 * len(rows) + 10; o = ['<svg viewBox="0 0 %d %d" class="chart" role="img">' % (w, h)]
    for i, (lab, v, n) in enumerate(rows):
        y = 6 + 26 * i; bw = 0 if v is None else max(1, (w - label_w - 90) * min(1.0, v / maxv))
        o.append('<text x="0" y="%d" class="lab">%s</text>' % (y + 14, E(lab)))
        o.append('<rect x="%d" y="%d" width="%.1f" height="16" class="%s"/>' % (label_w, y + 2, bw, cls))
        o.append('<text x="%.1f" y="%d" class="val">%s%s</text>' % (label_w + bw + 6, y + 14, fmt(v), '' if n is None else ' · %d' % n))
    o.append('</svg>'); return ''.join(o)

def ranges(d, w=560, label_w=110, maxd=45):
    """distance bands: {cls: {p25, median, p75, n}}"""
    items = [(k, v) for k, v in d.items() if v.get('n', 0) >= 10]; h = 26 * len(items) + 30; sx = lambda x: label_w + (w - label_w - 20) * min(x, maxd) / maxd
    o = ['<svg viewBox="0 0 %d %d" class="chart" role="img">' % (w, h)]
    for x in range(0, maxd + 1, 10): o.append('<line x1="%.1f" x2="%.1f" y1="4" y2="%d" class="grid"/><text x="%.1f" y="%d" class="ax">%d</text>' % (sx(x), sx(x), h - 18, sx(x) - 4, h - 4, x))
    for i, (k, v) in enumerate(items):
        y = 8 + 26 * i
        o.append('<text x="0" y="%d" class="lab">%s</text>' % (y + 13, E(k)))
        o.append('<rect x="%.1f" y="%d" width="%.1f" height="14" class="pl" opacity=".55"/>' % (sx(v['p25']), y + 2, max(1, sx(v['p75']) - sx(v['p25']))))
        o.append('<rect x="%.1f" y="%d" width="3" height="18" class="ink"/>' % (sx(v['median']) - 1.5, y))
        o.append('<text x="%.1f" y="%d" class="val">%s u · %d</text>' % (sx(v['p75']) + 6, y + 13, num(v['median']), v['n']))
    o.append('</svg>'); return ''.join(o)

def tbl(head, rows):
    o = ['<div class="tbl"><table><thead><tr>'] + ['<th%s>%s</th>' % (' class="n"' if i else '', E(h)) for i, h in enumerate(head)] + ['</tr></thead><tbody>']
    for r in rows: o.append('<tr>' + ''.join('<td%s>%s</td>' % (' class="n"' if i else '', c if i else E(c)) for i, c in enumerate(r)) + '</tr>')
    o.append('</tbody></table></div>'); return ''.join(o)

mv = A.get('move', {}); tc = (M.get('target_choice') or {}); sa = M.get('second_attacker') or {}; on = M.get('outnumbered') or {}; sk = M.get('skirmishes') or {}
nmatch = src.get('recordings', 0); nduel = A.get('duels', 0)
classes = {k: v.get('duels', 0) for k, v in C.items()}
parts = []
parts.append('''<header><div class="eyebrow">Project Reclaimer · native 0.10.1 · 08 Nov 2026</div><h1>How players fight</h1>
<p class="lede">Every public match the observer sat in, cut into duels and multi-player fights: who sees whom first, what they do with
their feet and their aim, which enemy they pick when several are near, and how it ends. The walkers learn from these numbers.</p></header>''')
parts.append('<div class="strip">' + ''.join('<div class="fig"><span class="k">%s</span><span class="v">%s</span><span class="s">%s</span></div>' % x for x in [
    ('Matches recorded', '%d' % nmatch, ', '.join('%s %d' % (k, v) for k, v in sorted(classes.items(), key=lambda kv: -kv[1])) + ' duels'),
    ('Duels (no Infection)', '{:,}'.format(nduel), '%s decided by a kill between the two' % pct((A.get('outcome', {}).get('a', 0) + A.get('outcome', {}).get('b', 0)) / max(1, nduel))),
    ('First attacker wins', pct(A.get('first_attacker_wins')), 'of the duels that ended in a kill'),
    ('Target choice rule', pct((tc.get('top1') or {}).get('logit')), 'names the enemy picked (nearest: %s)' % pct((tc.get('top1') or {}).get('nearest')))]) + '</div>')
vl = A.get('a_wins_by_vitality_lead', {}); order = ['<-0.5', '-0.5..-0.1', '-0.1..0.1', '0.1..0.5', '>0.5']
names = {'<-0.5': 'far behind', '-0.5..-0.1': 'behind', '-0.1..0.1': 'even', '0.1..0.5': 'ahead', '>0.5': 'far ahead'}
hb = A.get('a_wins_by_height', {}); horder = ['a 2+ below', 'a 0.5..2 below', 'level', 'a 0.5..2 above', 'a 2+ above']
parts.append('''<section><h2>Who wins</h2><div class="two"><div style="display:grid;gap:10px">
<p>A duel starts when one of two enemies within 45 units attacks the other: aiming within 8° while firing, or within 12° while the other loses shield or health. The one who attacks first wins %s of the duels that end in a kill.</p>
<p>%s of the time the defender was looking more than 60° away when the attack began, and then won only %s of the time (%s when facing the attacker). Losers never fired back in %s of duels.</p>
<p class="note">The attacker's chance by shield + health against the defender's (left) and by height (right).</p></div>
<div style="display:grid;gap:14px">%s%s</div></div></section>''' % (pct(A.get('first_attacker_wins')), pct(A.get('b_caught_looking_away')), pct(A.get('b_wins_if_looking_away')),
    pct(A.get('b_wins_if_facing')), pct(A.get('loser_never_fired')),
    bars([(names[k], vl[k][0], vl[k][1]) for k in order if k in vl], label_w=90),
    bars([(k.replace('a ', 'attacker '), hb[k][0], hb[k][1]) for k in horder if k in hb], label_w=150, cls='wk')))
parts.append('''<section><h2>Distance</h2><div class="two"><div style="display:grid;gap:10px"><p>Where the attacker stood when the fight began, by the weapon in its hands: the bar is the middle half of the duels, the tick the median.</p>
<p class="note">Time to kill, median, by the winner's weapon: %s.</p></div>%s</div></section>''' % (
    ', '.join('%s %s s' % (k, num(v['median'], 2)) for k, v in (A.get('time_to_kill_by_weapon') or {}).items() if v.get('n', 0) >= 10), ranges(A.get('attack_distance_by_weapon') or {})))
gr = A.get('grenades') or {}
parts.append('<section><h2>In the fight</h2>' + tbl(['Measure', 'Players'], [
    ['strafe speed across the line to the enemy (RMS, units/s)', num(mv.get('strafe_rms'), 2)], ['time between strafe direction changes (s)', num(mv.get('strafe_switch_s'), 2)],
    ['closing speed with shields up (units/s)', num(mv.get('closing_shield_up'), 2)], ['closing speed with shields low', num(mv.get('closing_shield_low'), 2)],
    ['closing speed when the enemy\'s shield is broken', num(mv.get('closing_vs_broken_shield'), 2)], ['share of the fight firing (0.1-s rows)', pct(mv.get('fire_share'))],
    ['aim error while attacking (median / 75%, deg)', '%s / %s' % (num(mv.get('aim_err_median')), num(mv.get('aim_err_p75')))], ['jumps a minute in fights', num(mv.get('jumps_per_min'), 2)],
    ['attacked: time to aim back (median, s)', num((A.get('b_turns_to_aim_s') or {}).get('median'), 2)],
    ['retreat when out-shielded', pct((A.get('retreat_when_outshielded') or {}).get('share'))]] + (
    [['grenades: throws a minute of fighting', num(gr.get('throws_per_fight_min'), 2)], ['grenades: throw distance (25..75%, units)', '%s..%s' % (num(gr.get('throw_dist_p25')), num(gr.get('throw_dist_p75')))]] if gr else [])) + '</section>')
wts = tc.get('weights_standardized') or {}
wl = {'off_aim': 'how far off the aim', 'log_dist': 'distance (log)', 'just_hurt': 'just hurt', 'attacked_me': 'attacked me', 'previous_target': 'previous target',
      'height': 'height above me', 'vitality': 'its shield + health', 'teammate_on_it': 'a teammate on it'}
wrows = sorted(wts.items(), key=lambda kv: -abs(kv[1]))
def wbars(rows, w=560, lw=160):
    h = 26 * len(rows) + 10; mx = max([abs(v) for _, v in rows] + [0.1]); cx = lw + (w - lw - 60) / 2; o = ['<svg viewBox="0 0 %d %d" class="chart" role="img">' % (w, h), '<line x1="%.1f" x2="%.1f" y1="2" y2="%d" class="grid"/>' % (cx, cx, h - 2)]
    for i, (k, v) in enumerate(rows):
        y = 6 + 26 * i; bw = (w - lw - 60) / 2 * abs(v) / mx
        o.append('<text x="0" y="%d" class="lab">%s</text>' % (y + 14, E(wl.get(k, k))))
        o.append('<rect x="%.1f" y="%d" width="%.1f" height="16" class="%s"/>' % (cx if v >= 0 else cx - bw, y + 2, max(1, bw), 'pl' if v >= 0 else 'wk'))
        o.append('<text x="%.1f" y="%d" class="val">%+.2f</text>' % (cx + bw + 6 if v >= 0 else cx + 6, y + 14, v))
    o.append('</svg>'); return ''.join(o)
t1 = tc.get('top1') or {}
two = on.get('two_plus') or {}; one = on.get('one_attacker') or {}
parts.append('''<section><h2>Several enemies at once</h2><div class="two"><div style="display:grid;gap:10px">
<p>Each time a player takes a new target with two or more enemies within 45 units, every enemy's features are recorded and a choice model fitted to which one was picked. Across %s such choices (%s enemies each on average) it names the picked enemy %s of the time, against %s for "the nearest" and %s for "the one nearest the aim".</p>
<p>Weights (standardized: longer bar = more influence; orange = less likely): what is already in front of the gun dominates; a just-hurt enemy draws fire; distance matters less than you'd think.</p>
<p>A second enemy opening fire mid-duel turns the player %s of the time: %s when it is in view, %s when it is behind, after %s s (median). Being outnumbered kills: %s die under two or more attackers against %s with one, and they still get %s kills per stretch (vs %s).</p></div>%s</div></section>''' % (
    '{:,}'.format(t1.get('sets', 0)), num(t1.get('mean_candidates')), pct(t1.get('logit')), pct(t1.get('nearest')), pct(t1.get('nearest_aim')),
    pct(sa.get('turned_to_it')), pct((sa.get('turned_if_new_one_in_view') or [None])[0]), pct((sa.get('turned_if_new_one_behind') or [None])[0]), num(sa.get('turn_s_median'), 2),
    pct(two.get('died')), pct(one.get('died')), num(two.get('kills_per_stretch'), 2), num(one.get('kills_per_stretch'), 2), wbars(wrows)))
bh = A.get('behaviour') or {}; rdr = bh.get('radar') or {}; rvg = bh.get('revenge') or {}; itm = bh.get('items') or {}
if rdr or rvg or itm:
    rows2 = []
    if rdr: rows2 += [['radar: turned to an unseen MOVING enemy within 8 units (2.5 s)', pct(rdr.get('turn_moving_in_range'))], ['... to a STILL one within 8 units (the tracker does not show it)', pct(rdr.get('turn_still_in_range'))],
                      ['... moving / still beyond 8 units (outside the 25 m tracker)', '%s / %s' % (pct(rdr.get('turn_moving_out_of_range')), pct(rdr.get('turn_still_out_of_range')))], ['radar: time to turn (median, s)', num(rdr.get('turn_s_median'), 2)]]
    if rvg: rows2 += [['after a respawn: next target is the killer', '%s (chance %s)' % (pct(rvg.get('next_target_killer')), pct(rvg.get('chance')))],
                      ['after a respawn: back within 8 units of the death spot in 40 s', '%s (another spot as far: %s)' % (pct(rvg.get('back_at_death_spot')), pct(rvg.get('baseline_spot')))],
                      ['respawn wait (median, s)', num(rvg.get('respawn_s_median'))]]
    if itm: rows2 += [['power items: reached by someone', '%s, median %s s after spawning' % (pct(itm.get('reached')), num(itm.get('reached_s_median')))], ['... first by the player nearest when it spawned', pct(itm.get('first_was_nearest'))],
                      ['... players 10-20 units away heading straight in', '%s (to another point as far: %s)' % (pct(itm.get('heading_in_10_20')), pct(itm.get('baseline_10_20')))]]
    parts.append('<section><h2>Radar, revenge and power items</h2><p>An enemy that comes up more than 75° off a player\'s view without attacking: does the player turn to face it, and does it matter whether the enemy moves (the motion tracker shows movement only) and how far it is? After a death: does the player go back, or after the killer? When a power weapon or power-up appears: who goes for it?</p>' + tbl(['Measure', 'Players'], rows2) + '</section>')
jt = M.get('join_teammate') or {}
if jt: parts.append('<section><h2>Teamwork</h2><p>When a teammate starts on an enemy within 25 units of a player without a target, the player joins on the same enemy within 3 s %s of the time (%s within 10 units), after %s s. %d cases.</p></section>' % (pct(jt.get('joined')), pct(jt.get('joined_if_within_10')), num(jt.get('join_s_median'), 2), jt.get('n', 0)))
if sk: parts.append('<section><h2>How big fights get</h2><p>Every 3 seconds, the players linked by attacks form a skirmish.</p>' + bars([('%s players' % k, v, None) for k, v in (sk.get('size_share') or {}).items()], label_w=90, cls='wk') + '</section>')
rows = []
for k in ('slayer', 'ctf', 'infection', 'other'):
    c = C.get(k)
    if not c or not c.get('duels'): continue
    m2 = c.get('multi', {}); rows.append([k, '{:,}'.format(c['duels']), pct(c.get('first_attacker_wins')), pct(c.get('b_caught_looking_away')), num(c.get('attack_distance_all')),
                                          num((c.get('move') or {}).get('strafe_rms'), 2), pct(((m2.get('outnumbered') or {}).get('two_plus') or {}).get('died')), pct(((m2.get('target_choice') or {}).get('top1') or {}).get('logit'))])
parts.append('<section><h2>By game type</h2>' + tbl(['Game type', 'Duels', 'First attacker wins', 'Caught looking away', 'Attack distance', 'Strafe RMS', 'Outnumbered: died', 'Choice rule hits'], rows) +
             '<p class="note">Infection is kept apart: zombies rush with swords and survivors camp, so its fights are not the walkers\' model (its lanes are).</p></section>')
if CMP:
    last = CMP[-1][1]
    parts.append('<section><h2>Walkers next to players</h2><p>The same measures over the walkers\' own battles (four walkers, Guardian, the server\'s view), against the players on the same map. Fight-likeness = 100 × exp(−mean |log(walker ÷ player)|).</p>' +
                 tbl(['Walkers', 'Fight-likeness'], [[lab, '%.0f%%' % c['score'] if c.get('score') is not None else '-'] for lab, c in CMP]) +
                 tbl(['Measure (%s)' % CMP[-1][0], 'Players', 'Walkers'], [[r['metric'], num(r['players'], 2) if isinstance(r['players'], (int, float)) else '-', num(r['walkers'], 2) if isinstance(r['walkers'], (int, float)) else '-'] for r in last['rows']]) + '</section>')
if LANES:
    figs = ''.join('<figure><img src="data:image/png;base64,%s" alt="%s lanes"><figcaption>%s</figcaption></figure>' % (base64.b64encode(open(p, 'rb').read()).decode(), E(m), E(m)) for m, p in LANES)
    parts.append('<section><h2>Lanes</h2><p>The routes players wear into each map: every crossing of a half-unit cell deepens it a little; the deepest ruts are the busiest routes (the stats page\'s heatmap "Lanes").</p><div class="lanes">%s</div></section>' % figs)
parts.append('<p class="note">Tools: <code>engage-extract.py</code> (duels + multi-player records), <code>engage-model.py</code> (this report and the walkers\' model), <code>engage-compare.py</code> (walkers vs players), <code>observe-public.sh</code> + <code>match-recorder.py</code> (the recordings; no names).</p>')

CSS = '''<title>How Players Fight</title>
<meta name="claude:viewport-fit-sides" content="cover">
<link rel="stylesheet" href="https://fonts.googleapis.com/css2?family=Barlow+Condensed:wght@500;600;700&family=IBM+Plex+Sans:wght@400;500;600&family=IBM+Plex+Mono:wght@400;500&display=swap">
<style>
:root { --bg: #f6f7f4; --panel: #ffffff; --ink: #1d2422; --dim: #5d6863; --line: #d9ded9; --accent: #5f6f22; --pl: #3a6fb0; --wk: #b0621f;
  --display: "Barlow Condensed", "Arial Narrow", sans-serif; --body: "IBM Plex Sans", system-ui, sans-serif; --mono: "IBM Plex Mono", ui-monospace, monospace; }
@media (prefers-color-scheme: dark) { :root:not([data-theme="light"]) { --bg: #121614; --panel: #1a201d; --ink: #e5e9e6; --dim: #9aa59f; --line: #2c3530; --accent: #b4c66a; --pl: #78a9ec; --wk: #e49a5c; color-scheme: dark } }
:root[data-theme="dark"] { --bg: #121614; --panel: #1a201d; --ink: #e5e9e6; --dim: #9aa59f; --line: #2c3530; --accent: #b4c66a; --pl: #78a9ec; --wk: #e49a5c; color-scheme: dark }
body { background: var(--bg); color: var(--ink); font: 15px/1.55 var(--body); }
.wrap { max-width: 940px; margin: 0 auto; padding-block: 28px 56px; padding-left: max(16px, env(safe-area-inset-left, 0px)); padding-right: max(16px, env(safe-area-inset-right, 0px)); display: grid; gap: 34px; }
header { display: grid; gap: 6px; }
.eyebrow { font: 500 12px/1 var(--mono); letter-spacing: .12em; text-transform: uppercase; color: var(--accent); }
h1 { font: 700 44px/1.02 var(--display); letter-spacing: .01em; margin: 0; text-wrap: balance; }
h2 { font: 600 26px/1.1 var(--display); letter-spacing: .02em; margin: 0 0 4px; text-wrap: balance; }
p { margin: 0; max-width: 66ch; } .lede { color: var(--dim); font-size: 16px; }
.strip { display: grid; grid-template-columns: repeat(auto-fit, minmax(190px, 1fr)); gap: 1px; background: var(--line); border: 1px solid var(--line); }
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
.chart .pl { fill: var(--pl); } .chart .wk { fill: var(--wk); } .chart .ink { fill: var(--ink); } .chart .grid { stroke: var(--line); stroke-width: 1; }
.note { font-size: 13px; color: var(--dim); }
.lanes { display: grid; grid-template-columns: repeat(auto-fit, minmax(240px, 1fr)); gap: 14px; }
.lanes figure { margin: 0; background: #0b0b0b; border: 1px solid var(--line); } .lanes img { display: block; width: 100%; height: auto; }
.lanes figcaption { font: 500 11px/1.2 var(--mono); letter-spacing: .08em; text-transform: uppercase; color: #c9cfca; padding: 8px 10px; }
code { font: 12.5px var(--mono); background: var(--panel); border: 1px solid var(--line); padding: 1px 5px; }
</style>
'''
open(OUT, 'w').write(CSS + '<div class="wrap">' + '\n'.join(parts) + '</div>\n')
print('wrote %s (%d KB)' % (OUT, os.path.getsize(OUT) // 1024))
