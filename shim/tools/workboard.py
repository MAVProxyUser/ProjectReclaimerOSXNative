#!/usr/bin/env python3
"""The work board (2026-11-08; the user: "foreground your current progress so I can tell there are jobs running"): a small local
page that shows what is running and what is being worked on, refreshed every 3 s.
    python3 shim/tools/workboard.py [--port 8790]            serve http://127.0.0.1:8790/
    python3 shim/tools/workboard.py task "<text>" doing|next|done|drop [--note "..."]    the task list (ReclaimerMac/workboard/tasks.json)
It reads only local things: running processes (the observer, recorders, walker battles, the local server, Firefight runs, the
analysis tools, every Project Reclaimer instance and what it was started with), the observer's newest log and recorder lines, the
live game's stats page (127.0.0.1:8765) when one answers, the recordings in ReclaimerMac/tracks (totals by game type, cached a
minute) and the task list. Bound to 127.0.0.1 only."""
import glob, gzip, json, os, subprocess, sys, threading, time, urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

SUP = os.path.expanduser('~/Library/Application Support/ReclaimerMac'); TR = os.path.join(SUP, 'tracks'); WB = os.path.join(SUP, 'workboard')
TASKS = os.path.join(WB, 'tasks.json'); DIRECTION = os.path.join(WB, 'direction.json'); TOME = os.path.join(TR, 'tome.json')
def arg(k, d):
    a = sys.argv; return a[a.index(k) + 1] if k in a else d

def direction_cmd():   # workboard.py direction "line" "line" ...  (the plan in words, shown on top)
    os.makedirs(WB, exist_ok=True); tmp = DIRECTION + '.tmp'; json.dump({'t': time.time(), 'lines': sys.argv[2:]}, open(tmp, 'w'), indent=1); os.replace(tmp, DIRECTION); print('direction: %d lines' % (len(sys.argv) - 2))
# the public walker as it learns (2026-11-09, the user: "are we no longer learning and updating the work board, etc?"): its K/D now (its
# own stats page, 8771), why it died (nbot.c why_note -> battles/follow/why-<nick>.jsonl), the death tuner's steps / verdicts
# (shim/tools/death-tuner.py -> tuner-<nick>.jsonl) and the brain's current lesson (its log's last "[brain] lesson:")
PUBWALKER = os.environ.get('WB_PUBWALKER', 'AIWalkerTest')
def jl_tail(path, n):
    try:
        with open(path, 'rb') as f:
            f.seek(0, 2); sz = f.tell(); f.seek(max(0, sz - 65536)); lines = f.read().decode('utf-8', 'replace').splitlines()
    except OSError: return []
    out = []
    for l in lines[-n:]:
        try: out.append(json.loads(l))
        except Exception: pass
    return out
def pubwalker():
    r = {'nick': PUBWALKER, 'why': jl_tail(os.path.join(SUP, 'battles', 'follow', 'why-%s.jsonl' % PUBWALKER), 60),
         'tune': jl_tail(os.path.join(SUP, 'battles', 'follow', 'tuner-%s.jsonl' % PUBWALKER), 12)}
    try:
        with urllib.request.urlopen('http://127.0.0.1:8771/stats.json', timeout=1.5) as h: s = json.loads(h.read())
        me = [p for p in s.get('players') or [] if p.get('you')]
        r['live'] = {'map': s.get('map'), 'players': len(s.get('players') or []), 'kills': me[0].get('kills') if me else None, 'deaths': me[0].get('deaths') if me else None,
                     'on': (me[0].get('aim') or {}).get('on') if me else None, 'head': (me[0].get('aim') or {}).get('head') if me else None}
    except Exception: r['live'] = None
    try:
        with open(os.path.join(SUP, 'battles', 'aiwalker.log'), 'rb') as f:
            f.seek(0, 2); sz = f.tell(); f.seek(max(0, sz - 2000000)); t = f.read().decode('utf-8', 'replace')
        i = t.rfind('[brain] lesson: '); r['lesson'] = t[i + 16:t.find('\n', i)].strip() if i >= 0 else None
        j = t.rfind('[brain] code built '); r['brain'] = t[j + 19:t.find(' is stepping', j)] if j >= 0 else None
    except OSError: r['lesson'] = r['brain'] = None
    return r
# THE BRAIN TAB (2026-11-09; the user: "there should be a regular brain reload monitoring session in the work board tabs so this doesn't
# happen again"): the loaded brain (build, lesson), its revisions (nbot_goals.c brain_review -> review-<nick>.jsonl: per map, every
# 3 minutes, on a monitor trip), the monitor's triggers (shim/tools/brain-monitor.py -> monitor-<nick>.json / .jsonl) and the tuner's
# lever resets; ALARMS when the monitor stops writing or no revision came for two review periods in a match
def brainmon():
    F = os.path.join(SUP, 'battles', 'follow')
    try: st = json.load(open(os.path.join(F, 'monitor-%s.json' % PUBWALKER)))
    except Exception: st = None
    tune = [r for r in jl_tail(os.path.join(F, 'tuner-%s.jsonl' % PUBWALKER), 200) if r.get('event') in ('reset', 'good set')][-12:]
    return {'state': st, 'reviews': jl_tail(os.path.join(F, 'review-%s.jsonl' % PUBWALKER), 40), 'events': jl_tail(os.path.join(F, 'monitor-%s.jsonl' % PUBWALKER), 40), 'tune': tune}
def tome_load():
    try: return json.load(open(TOME))
    except Exception: return None
def tasks_load():
    try: return json.load(open(TASKS))
    except Exception: return []
def tasks_cmd():
    text, state = sys.argv[2], sys.argv[3]; note = arg('--note', ''); T = tasks_load(); now = time.time()
    for t in T:
        if t['text'] == text:
            if state == 'drop': T.remove(t)
            else: t['state'] = state; t['t'] = now; t['note'] = note or t.get('note', '')
            break
    else:
        if state != 'drop': T.append({'text': text, 'state': state, 't': now, 'note': note})
    os.makedirs(WB, exist_ok=True); tmp = TASKS + '.tmp'; json.dump(T, open(tmp, 'w'), indent=1); os.replace(tmp, TASKS); print('%s: %s' % (state, text))

KINDS = [('observe-public.sh', 'Observer (public games)'), ('match-recorder.py', 'Match recorder'), ('walker-battle.sh', 'Walker battle'),
         ('walker-train.py', 'Walker training'), ('native-firefight.sh', 'Firefight run'), ('engage-extract.py', 'Engagement extraction'),
         ('engage-model.py', 'Engagement model'), ('lanes-import.py', 'Lanes import'), ('engage-compare.py', 'Walkers vs players'), ('learnings.py', 'Learnings'),
         ('refresh: new recordings', 'Learning refresh (every 20 min)'), ('learn-loop.py', 'Learn loop (a public game, then coached walkers)'),
         ('walker-coach.py', 'Walker self-coaching'), ('game-review.py', 'Game review'), ('coach.py', 'Coach (you vs the winners)'),
         # (2026-11-09, the user: "the jobs in the work page are still like hung": only the refit showed -- the public walker, its tuner,
         #  the release watch and the learning refresh loop were never listed)
         ('walker-public.sh', 'Public walker (rejoins the approved server)'), ('death-tuner.py', 'Death tuner (a setting step per cause, lead A/B)'),
         ('release-watch.sh', 'Release watch (ports nhook to a new client)'), ('fire-ab.sh', 'Live A/B of a walker setting'),
         ('refresh-loop.sh', 'Learning refresh (models, learnings, coach)'), ('stats-proxy.py', 'Stats page relay (8765)'),
         ('brain-monitor.py', 'Brain monitor (trips new brain revisions)')]
def procs():
    try: out = subprocess.run(['ps', '-axo', 'pid,etime,command'], capture_output=True, text=True, timeout=5).stdout.splitlines()[1:]
    except Exception: return [], []
    jobs, apps = [], []
    for l in out:
        parts = l.strip().split(None, 2)
        if len(parts) < 3: continue
        pid, et, cmd = parts
        if '/Applications/Project Reclaimer.app/Contents/MacOS/project-reclaimer' in cmd.split(' --')[0] and cmd.startswith('/Applications/'):
            a = cmd.split('project-reclaimer', 1)[1].strip()
            what = 'local server' if '--server' in a else ('joined ' + a.split('--join-server', 1)[1].split()[0]) if '--join-server' in a else ('Firefight ' + a.split('--reach-firefight', 1)[1].split()[0]) if '--reach-firefight' in a else (a[:60] or 'menu')
            apps.append({'pid': int(pid), 'elapsed': et, 'what': what, 'background': '--background' in a})
            continue
        for key, name in KINDS:
            if key in cmd and 'workboard' not in cmd and not cmd.startswith('grep') and 'pgrep -f' not in cmd and 'until ' not in cmd:   # (not a watcher's shell)
                jobs.append({'pid': int(pid), 'elapsed': et, 'name': name, 'args': cmd.split(key, 1)[1].strip()[:80]}); break
    return jobs, apps

_tot = {'t': 0, 'v': None}
def totals():
    if _tot['v'] and time.time() - _tot['t'] < 60: return _tot['v']
    files = 0; by = {}; maps = {}
    for p in glob.glob(os.path.join(TR, '*.jsonl.gz')):
        try:
            with gzip.open(p, 'rt') as f:
                h = json.loads(f.readline()); n = 0
                while True:
                    try: l = f.readline()
                    except Exception: break
                    if not l: break
                    if l.startswith('{"t"') and '"p"' in l: n += 1
        except Exception: continue
        files += 1; m = ((h.get('mode') or '') + ' ' + (h.get('variant') or '')).lower()
        c = 'infection' if ('infection' in m or 'braaa' in m) else 'ctf' if ('ctf' in m or 'flag' in m) else 'slayer' if any(k in m for k in ('slayer', 'br starts', 'swat', 'snipe')) else 'other'
        by[c] = by.get(c, 0) + n; maps[h.get('map') or '?'] = maps.get(h.get('map') or '?', 0) + n
    v = {'files': files, 'minutes_by_type': {k: round(n / 600) for k, n in sorted(by.items(), key=lambda kv: -kv[1])}, 'minutes': round(sum(by.values()) / 600),
         'maps': {k: round(n / 600) for k, n in sorted(maps.items(), key=lambda kv: -kv[1])[:10]}}
    try:
        m = json.load(open(os.path.join(TR, 'engage-model.json'))); v['model'] = {'duels': m.get('source', {}).get('duels'), 'pieces': sum(len(x) for x in (m.get('library') or {}).values()),
                                                                          'radar_contacts': ((m.get('behaviour') or {}).get('radar') or {}).get('contacts'), 'updated': os.path.getmtime(os.path.join(TR, 'engage-model.json'))}
    except Exception: pass
    _tot.update(t=time.time(), v=v); return v

def tail(path, n):
    try:
        with open(path, 'rb') as f:
            f.seek(0, 2); sz = f.tell(); f.seek(max(0, sz - 8000)); return [l.decode('utf-8', 'replace') for l in f.read().splitlines()[-n:]]
    except Exception: return []
def status():
    jobs, apps = procs()
    obs = sorted(glob.glob(os.path.join(TR, 'observe-*.txt')), key=os.path.getmtime)[-1:] ; rec = sorted(glob.glob(os.path.join(TR, 'logs', '*.rec')), key=os.path.getmtime)[-1:]
    live = None
    try:
        with urllib.request.urlopen('http://127.0.0.1:8765/stats.json', timeout=1.5) as r: s = json.loads(r.read())
        v = s.get('variant') or {}; live = {'map': s.get('map'), 'in_match': s.get('in_match'), 'time': s.get('time'), 'players': len(s.get('players') or []),
                                            'alive': sum(1 for p in s.get('players') or [] if p.get('alive')), 'mode': v.get('mode') if isinstance(v, dict) else None, 'variant': v.get('name') if isinstance(v, dict) else None}
    except Exception: pass
    try: direction = json.load(open(DIRECTION))
    except Exception: direction = None
    per_map = sorted(os.path.basename(p)[:-len('.engage.json')] for p in glob.glob(os.path.join(TR, 'maps', '*.engage.json')))
    try: learn = json.load(open(os.path.join(TR, 'learnings.json')))
    except Exception: learn = None
    try: coach = json.load(open(os.path.join(TR, 'coach.json')))
    except Exception: coach = None
    rv = sorted(glob.glob(os.path.join(TR, 'reviews', '*.json')), key=os.path.getmtime)
    try: review = json.load(open(rv[-1])) if rv else None
    except Exception: review = None
    try: ll = json.load(open(os.path.join(TR, 'learn-loop.json'))); ll = {'updated': ll.get('updated'), 'turns': ll.get('turns', [])[-12:]}
    except Exception: ll = None
    try:
        cs = json.load(open(os.path.join(SUP, 'battles', 'coach', 'state.json')))
        wc = {'knobs': cs.get('knobs', {}), 'rounds': [{'t': r['t'], 'label': r.get('label'), 'map': r.get('map'), 'class': r.get('class'), 'winners': r.get('winners'),
              'walkers': {w: {'likeness': v.get('likeness'), 'kd': v.get('kd'), 'duels_won': v.get('duels_won'), 'duels': v.get('duels'), 'changes': v.get('changes')} for w, v in r.get('walkers', {}).items()}}
              for r in cs.get('rounds', [])[-16:]]}
    except Exception: wc = None
    try: wk = json.load(open(os.path.join(TR, 'weapon-kills.json')))
    except Exception: wk = None
    pw = pubwalker()
    return {'pubwalker': pw, 'wkills': wk, 'review': review, 'learnloop': ll, 'wcoach': wc, 'now': time.time(), 'jobs': jobs, 'apps': apps, 'direction': direction, 'tome': tome_load(), 'per_map_models': per_map, 'learnings': learn, 'coach': coach, 'observer_log': tail(obs[0], 8) if obs else [], 'recorder': tail(rec[0], 4) if rec else [],
            'live': live, 'totals': totals(), 'tasks': tasks_load(), 'you_sessions': you_load()[-10:], 'brainmon': brainmon(),
            'observer_age': (time.time() - os.path.getmtime(obs[0])) if obs else None}

PAGE = r'''<!doctype html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>Work board</title>
<style>:root{--bg:#111416;--panel:#1a1f22;--fg:#e4e8ea;--dim:#97a3a8;--line:#2b3338;--ok:#7fd18b;--go:#f0c060;--acc:#78a9ec}
body{background:var(--bg);color:var(--fg);font:14px/1.5 system-ui,sans-serif;margin:0;padding:18px max(16px,env(safe-area-inset-left))}
h1{font-size:20px;margin:0 0 4px}h2{font-size:13px;letter-spacing:.08em;text-transform:uppercase;color:var(--dim);margin:0 0 8px}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(320px,1fr));gap:14px}.p{background:var(--panel);border:1px solid var(--line);border-radius:6px;padding:12px 14px;min-width:0}
.row{display:flex;gap:8px;align-items:baseline;padding:3px 0;border-bottom:1px solid var(--line)}.row:last-child{border:0}.k{color:var(--dim);font-size:12px}
.dot{width:9px;height:9px;border-radius:50%;background:var(--ok);display:inline-block;animation:pulse 1.6s infinite}@keyframes pulse{50%{opacity:.35}}
.st-doing{color:var(--go)}.st-next{color:var(--acc)}.st-done{color:var(--dim);text-decoration:line-through}pre{margin:0;font:12px ui-monospace,monospace;white-space:pre-wrap;color:var(--dim)}
.big{font-size:26px;font-weight:600}.none{color:var(--dim)}
.lgrid{display:grid;grid-template-columns:repeat(auto-fit,minmax(340px,1fr));gap:12px}.lcard{border:1px solid var(--line);border-radius:5px;padding:8px 10px;min-width:0}
.lcard h3{margin:0 0 6px;font-size:13px;color:var(--go)}.li{padding:4px 0;border-bottom:1px solid var(--line);font-size:13px}.li .wk{font-size:11.5px;margin-top:3px;padding-left:8px;border-left:2px solid var(--line)}.li .wkset{color:var(--acc);border-color:var(--acc)}.li .wkok{color:var(--ok);border-color:var(--ok)}.li .wkno{color:var(--go);border-color:var(--go)}.li:last-child{border:0}.li b{color:var(--fg)}
.li .n{color:var(--dim);font-size:11px;margin-left:4px}.li.weak{opacity:.55}
#tabs{display:inline-flex;gap:4px;margin-left:12px;vertical-align:middle}#tabs a{font-size:13px;font-weight:500;color:var(--dim);text-decoration:none;padding:3px 10px;border:1px solid var(--line);border-radius:4px}
#tabs a.on{color:var(--fg);border-color:var(--acc)}.yb{font:inherit;font-weight:600;padding:5px 16px;border-radius:5px;border:1px solid var(--acc);background:transparent;color:var(--fg);cursor:pointer}.yb:disabled{opacity:.35;cursor:default}#tabs a:hover{color:var(--fg)}</style></head><body>
<h1>Work board <nav id="tabs"><a href="#" data-v="work">Work</a><a href="#learnings" data-v="learnings">Learnings</a><a href="#you" data-v="you">You</a><a href="#walkers" data-v="walkers">Walkers</a><a href="#brain" data-v="brain">Brain</a></nav> <span class="k" id="upd"></span></h1>
<div class="k">What is running on this Mac for Reclaimer, what is being worked on, and what the game is learning from real players. Refreshes every 3 s.</div><br>
<div id="learnview" hidden><div class="p" style="margin-bottom:14px"><h2>Kills by weapon <span class="k" id="wksrc"></span></h2>
<div class="k" style="margin-bottom:8px">Every kill in the game's own kill feed of every recorded public match (our local servers left out), by what made it: weapons, vehicles (the driver's weapon or a splatter), vehicle guns (gunner seats), turrets, grenades, melee. Headshots are counted in their weapon. Click a column to sort, a row for its kills game by game; the line is its kills per game, oldest to newest. Recounted every minute (the match being recorded is in it).</div>
<div id="wkf" style="display:flex;flex-wrap:wrap;gap:6px;margin-bottom:8px"></div><div id="wk2" style="overflow-x:auto"></div></div><div class="p"><h2>What the game is learning <span class="k" id="lrsrc"></span></h2><div class="k" style="margin-bottom:8px">Plain-language findings from every recorded public match (Infection left out of the fighting findings), each with the number of cases it rests on; thin ones (under 30 cases) are dimmed. Rebuilt every 20 minutes while sampling runs.</div><div id="lrn" class="lgrid"></div></div></div>
<div id="youview" hidden><div class="p"><h2>You vs the winners <span class="k" id="ysrc"></span></h2>
<div style="display:flex;gap:10px;align-items:center;flex-wrap:wrap;margin-bottom:8px"><button id="ystart" class="yb">Start</button><button id="ystop" class="yb">Stop</button><span id="ystate" class="k"></span></div>
<div class="k" style="margin-bottom:8px">Press Start when you begin playing and Stop when you are done. Only your matches that START inside a session count -- recorded by nhook (the game launched through our launcher), never a walker's game -- next to the WINNERS (the top quarter of players by K/D in non-Infection matches) and everyone, measure by measure, the biggest gaps first. A match is read a couple of minutes after it ends; Stop reads them at once. shim/tools/coach.py.</div><div id="ysess" class="k" style="margin-bottom:8px"></div><div id="you"></div></div></div>
<div id="brainview" hidden>
<div class="p" style="margin-bottom:14px"><h2>The walker's brain <span class="k" id="bmsrc"></span></h2><div id="bmalarm"></div><div id="bmbrain" style="margin:6px 0 10px"></div>
<div class="k" style="margin-bottom:8px">A new revision comes on every map change, every review period (3 min), and whenever the monitor trips a trigger below: the brain then reads its death map again, rebuilds the paths around its own recent deaths and sets its caution from the window; the triggers that reset the levers also put the tuner's settings back to its last good set. A tripped trigger re-arms when its number recovers. shim/tools/brain-monitor.py, nbot_goals.c brain_review.</div>
<div class="grid"><div><h2>Triggers</h2><div id="bmtrig"></div></div><div><h2>Monitor events</h2><div id="bmev"></div></div></div></div>
<div class="p"><h2>Revisions</h2><div id="bmrev" style="overflow-x:auto"></div></div></div>
<div id="walkview" hidden>
<div class="p" style="margin-bottom:14px"><h2>Public walker -- learning while it plays <span class="k" id="pwsrc"></span></h2><div id="pwlive" style="margin-bottom:8px"></div>
<div class="grid"><div><div class="k" style="margin-bottom:4px">Why it died (the last 60 deaths: one cause each)</div><div id="pwwhy"></div></div>
<div><div class="k" style="margin-bottom:4px">The death tuner (one setting every 6 deaths for the commonest recent cause; put back if K/D fell)</div><div id="pwtune"></div></div></div>
<div class="k" style="margin-top:8px">The brain now: <span id="pwlesson"></span></div></div>
<div class="p" style="margin-bottom:14px"><h2>The last public game vs what we assumed <span class="k" id="rvsrc"></span></h2><div class="k" style="margin-bottom:8px">One whole public match (watched, never played by a walker) measured like the map's earlier matches and like everything; a SURPRISE = this game is off the map's number by more than a game this size explains (shares: 2+ standard errors; other numbers: 30%+ with 15+ cases). The per-map model takes the game in right after.</div><div id="rv" style="overflow-x:auto"></div></div>
<div class="grid"><div class="p"><h2>Learn loop</h2><div class="k" style="margin-bottom:6px">Each turn: a random public game observed whole -> reviewed -> models refit -> walkers coached on that map against that game's winners. Distance to the winners: 0 = their numbers (lower is better).</div><div id="ll"></div></div>
<div class="p"><h2>Walker self-coaching <span class="k">(battles/coach/state.json)</span></h2><div id="wk"></div><div id="wr" style="margin-top:8px"></div></div></div></div>
<div id="workview">
<div class="p" id="dirp" style="margin-bottom:14px"><h2>Direction</h2><div id="dir"></div></div>
<div class="grid"><div class="p"><h2>Running now</h2><div id="jobs"></div></div><div class="p"><h2>Game instances</h2><div id="apps"></div><div id="live"></div></div>
<div class="p"><h2>Tasks</h2><div id="tasks"></div></div><div class="p"><h2>Data collected</h2><div id="tot"></div></div>
<div class="p" style="grid-column:1/-1"><h2>Coverage: the tome (maps x game types sampled from real players)</h2><div id="cov" style="overflow-x:auto"></div></div>
<div class="p" style="grid-column:1/-1"><h2>Observer</h2><pre id="obs"></pre><br><pre id="rec"></pre></div></div></div>
<script>
const E=s=>String(s??'').replace(/[&<>"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]));
function ago(t){const s=Math.round(Date.now()/1000-t);return s<90?s+' s ago':s<5400?Math.round(s/60)+' min ago':Math.round(s/3600)+' h ago'}
async function tick(){try{const j=await (await fetch('/status.json')).json();
 document.getElementById('upd').textContent='· '+new Date(j.now*1000).toLocaleTimeString();
 document.getElementById('jobs').innerHTML=j.jobs.length?j.jobs.map(x=>`<div class="row"><span class="dot"></span><b>${E(x.name)}</b><span class="k">running ${E(x.elapsed)}</span><span class="k">${E(x.args)}</span></div>`).join(''):'<div class="none">no jobs running</div>';
 document.getElementById('apps').innerHTML=j.apps.length?j.apps.map(x=>`<div class="row"><span class="dot"></span>${E(x.what)}<span class="k">${x.background?'background window':''} · ${E(x.elapsed)}</span></div>`).join(''):'<div class="none">no game running</div>';
 const l=j.live; document.getElementById('live').innerHTML=l?`<div class="row"><span class="k">live game (127.0.0.1:8765)</span> <b>${E(l.mode||'')}</b> ${E(l.variant||'')} on <b>${E(l.map)}</b> · ${l.in_match?Math.round(l.time/60)+' min in':'not in a match'} · ${l.alive}/${l.players} players alive</div>`:'';
 const T=j.tasks||[], ord={doing:0,next:1,done:2}; T.sort((a,b)=>(ord[a.state]-ord[b.state])||(b.t-a.t));
 document.getElementById('tasks').innerHTML=T.length?T.map(t=>`<div class="row"><span class="st-${t.state}">${E(t.state)}</span><span class="st-${t.state}">${E(t.text)}</span><span class="k">${E(t.note||'')} · ${ago(t.t)}</span></div>`).join(''):'<div class="none">no tasks listed</div>';
 const o=j.totals||{}; const m=o.model||{};
 document.getElementById('tot').innerHTML=`<div class="big">${o.files??'-'} matches · ${o.minutes??'-'} min</div><div class="k">${Object.entries(o.minutes_by_type||{}).map(([k,v])=>k+' '+v+' min').join(' · ')}</div>
  <div class="k">maps (min): ${Object.entries(o.maps||{}).map(([k,v])=>k+' '+v).join(', ')}</div><div class="k">engagement model: ${m.duels??'-'} duels, ${m.radar_contacts??'-'} radar contacts, ${m.pieces??'-'} movement pieces${m.updated?' · updated '+ago(m.updated):''}</div>`;
 const d=j.direction; document.getElementById('dir').innerHTML=d&&d.lines?d.lines.map(l=>`<div class="row">${E(l)}</div>`).join('')+`<div class="k">set ${ago(d.t)}</div>`:'<div class="none">-</div>';
 const tm=j.tome, cls=['slayer','ctf','infection','other'], pm=new Set(j.per_map_models||[]);
 if(tm&&tm.maps){ const rows=Object.entries(tm.maps).sort((a,b)=>Object.values(b[1].classes).reduce((s,x)=>s+x.minutes,0)-Object.values(a[1].classes).reduce((s,x)=>s+x.minutes,0));
   const cell=x=>x?`${Math.round(x.minutes)} min · ${x.recordings} rec · ${x.duels} duels<br><span class="k">max ${x.players_max} players · last ${ago(x.last)}</span>`:'<span class="none">-</span>';
   document.getElementById('cov').innerHTML=`<table style="border-collapse:collapse;width:100%;font-size:12.5px"><tr><th style="text-align:left">map</th>${cls.map(c=>`<th style="text-align:left">${c}</th>`).join('')}<th style="text-align:left">walker model</th></tr>`+
    rows.map(([m,e])=>`<tr style="border-top:1px solid var(--line)"><td><b>${E(e.name)}</b> <span class="k">${E(m)}</span></td>${cls.map(c=>`<td>${cell(e.classes[c])}</td>`).join('')}<td>${pm.has(m)?'<span style="color:var(--ok)">per-map</span>':'<span class="k">aggregate</span>'}</td></tr>`).join('')+'</table>'+
    `<div class="k" style="margin-top:8px">never sampled yet: ${E((tm.never_sampled||[]).join(', ')||'none')} · tome updated ${ago(tm.updated)}</div>`; }
 else document.getElementById('cov').innerHTML='<div class="none">no tome yet (engage-model.py writes it)</div>';
 const L=j.learnings; if(L&&L.sections){ const s=L.source||{};
   document.getElementById('lrsrc').textContent=`· ${s.recordings} matches, ${s.minutes} min, ${(s.duels||0).toLocaleString()} duels (${(s.decided||0).toLocaleString()} ending in a kill), ${s.maps} maps · refreshed ${ago(L.updated)}`;
   document.getElementById('lrn').innerHTML=L.sections.map(sc=>`<div class="lcard"><h3>${E(sc.topic)}</h3>${sc.items.map(i=>`<div class="li${i.weak?' weak':''}"><b>${E(i.title)}</b> ${E(i.text)}<span class="n">n=${(i.n||0).toLocaleString()}${i.weak?' · thin':''}</span>${i.walker?`<div class="wk ${/^not yet/.test(i.walker)?'wkno':/^sets /.test(i.walker)?'wkset':'wkok'}">in the walker: ${E(i.walker)}</div>`:''}</div>`).join('')||'<div class="none">nothing yet</div>'}</div>`).join(''); }
 else document.getElementById('lrn').innerHTML='<div class="none">no findings yet (shim/tools/learnings.py)</div>';
 { const B=j.brainmon||{}, st=B.state, R=(B.reviews||[]).slice().reverse(), now=Date.now()/1000; const al=[];
   if(!st||now-st.t>60) al.push('the brain monitor is not running (shim/tools/brain-monitor.py)'+(st?' -- its last look '+ago(st.t):''));
   const rv=R[0], per=(st&&st.review_period)||180; if(st&&st.metrics&&st.metrics.in_match&&(!rv||now-rv.t>2*per+30)) al.push('STALE: no brain revision for '+(rv?ago(rv.t).replace(' ago',''):'ever')+' in a match');
   document.getElementById('bmalarm').innerHTML=al.map(x=>`<div style="color:var(--go);font-weight:600">${E(x)}</div>`).join('');
   const bi=(st&&st.brain)||{}; document.getElementById('bmsrc').textContent=rv?`· revision r${rv.r} [${rv.trigger}] ${ago(rv.t)} on ${rv.map}`:'· no revision yet';
   document.getElementById('bmbrain').innerHTML=`<div><b>Brain code built ${E(bi.built||'?')}</b></div><div class="k">${E(bi.lesson||'')}</div>`;
   const T=(st&&st.triggers)||[], M=(st&&st.metrics)||{};
   document.getElementById('bmtrig').innerHTML=(st&&st.metrics?`<div class="k" style="margin-bottom:4px">${E(M.map)}: K ${M.kills} D ${M.deaths}, accuracy ${M.acc_match??'-'} (lobby ${M.lobby_acc!=null?M.lobby_acc.toFixed(3):'-'}), lobby K/D ${M.lobby_kd!=null?M.lobby_kd.toFixed(2):'-'}</div>`:`<div class="none">${E((st&&st.note)||'no numbers yet')}</div>`)+
     T.map(t=>`<div class="row"><span style="color:${t.armed?(t.bad?'var(--go)':'var(--ok)'):'var(--go)'}">${t.armed?'armed':'TRIPPED'}</span><b>${E(t.name)}</b><span>${E(t.value??'-')}</span><span class="k">trips ${E(t.threshold)}${t.resets?' · resets the levers':''}${t.last_trip?' · last '+ago(t.last_trip):''}</span></div>`).join('')+
     (T.length?'':'<div class="k">(each trigger appears once its window has numbers: 2-5 minutes into a match)</div>');
   const ev=[...(B.events||[]).filter(e=>e.event!=='start'),...(B.tune||[]).map(e=>({...e,tuner:1}))].sort((a,b)=>b.t-a.t).slice(0,16);
   document.getElementById('bmev').innerHTML=ev.length?ev.map(e=>`<div class="row"><span class="k">${new Date(e.t*1000).toLocaleTimeString()}</span><b>${E(e.tuner?(e.event==='reset'?'levers reset':'good set kept'):e.event)}</b><span class="k">${E(e.tuner?(e.event==='reset'?'to '+e.to+' ('+Object.keys(e.changed||{}).length+' settings)':e.why):e.event==='trip'?e.trigger+' '+e.value+' '+e.threshold:e.event==='map'?(e.from||'-')+' -> '+e.map:e.trigger+' '+(e.value??''))}</span></div>`).join(''):'<div class="none">no trips yet</div>';
   document.getElementById('bmrev').innerHTML=R.length?`<table style="border-collapse:collapse;width:100%;font-size:13px"><tr><th style="text-align:left">when</th><th>r</th><th style="text-align:left">trigger</th><th style="text-align:left">map</th><th>kills</th><th>deaths</th><th>empty</th><th>deaths/min</th><th>caution</th><th>press</th><th>death map</th><th style="text-align:left">hottest own spot</th></tr>`+
     R.map(r=>`<tr style="border-top:1px solid var(--line)"><td>${new Date(r.t*1000).toLocaleTimeString()}</td><td style="text-align:center">${r.r}</td><td>${E(r.trigger)}</td><td>${E(r.map)}</td><td style="text-align:center">${r.kills}</td><td style="text-align:center">${r.deaths}</td><td style="text-align:center">${r.empty}</td><td style="text-align:center">${r.dpm}</td><td style="text-align:center">${r.caution}</td><td style="text-align:center">x${r.press_x}</td><td style="text-align:center">${r.deathmap??'-'}</td><td>${r.hot&&r.hot[2]>=2?`(${r.hot[0].toFixed(0)}, ${r.hot[1].toFixed(0)}): ${r.hot[2]} deaths`:'-'}</td></tr>`).join('')+'</table>':'<div class="none">no revision yet</div>'; }
 { const S=j.you_sessions||[], on=S.length&&!S[S.length-1].end; document.getElementById('ystart').disabled=!!on; document.getElementById('ystop').disabled=!on;
   document.getElementById('ystate').textContent=on?`session running since ${new Date(S[S.length-1].start*1000).toLocaleTimeString()}`:(S.length?'no session running':'no session yet');
   const cs=(j.coach&&j.coach.sessions)||[]; document.getElementById('ysess').textContent=S.length?'sessions: '+S.slice(-5).map(x=>{const c=cs.find(y=>y.start===x.start);return `${new Date(x.start*1000).toLocaleString()} - ${x.end?new Date(x.end*1000).toLocaleTimeString():'now'}${c?' ('+c.matches+' match'+(c.matches===1?'':'es')+')':''}`}).join(' · '):''; }
 const C=j.coach; if(C&&C.rows&&!C.pretend){ document.getElementById('ysrc').textContent=`· ${C.you.matches} match(es) from your sessions, K/D ${C.you.kd??'-'} · winners: ${C.winners.players} players, K/D ${C.winners.kd_from}+ · refreshed ${ago(C.updated)}`;
   document.getElementById('you').innerHTML=`<table style="border-collapse:collapse;width:100%;font-size:13px"><tr><th style="text-align:left">measure</th><th>you</th><th>winners</th><th>everyone</th><th style="text-align:left">verdict</th></tr>`+
    C.rows.map(r=>`<tr style="border-top:1px solid var(--line)${r.thin?';opacity:.55':''}"><td>${E(r.measure)}</td><td style="text-align:center"><b>${E(r.you)}</b></td><td style="text-align:center">${E(r.winners)}</td><td style="text-align:center" class="k">${E(r.everyone)}</td><td style="color:${r.advice?'var(--go)':'var(--ok)'}">${E(r.verdict)}${r.thin?' <span class="k">(thin)</span>':''}${r.advice?`<div class="k">-> ${E(r.advice)}</div>`:''}</td></tr>`).join('')+'</table>'; }
 else document.getElementById('you').innerHTML=`<div class="none">${E((C&&C.note)||'no report yet (shim/tools/coach.py)')}</div>`;
 if(j.wkills&&(!WKD||j.wkills.updated!==WKD.updated)){ WKD=j.wkills; wkRender(); } else if(!WKD) wkRender();
 const R=j.review, pf=(v,sh)=>v==null?'-':sh?Math.round(v*100)+'%':(+v).toFixed(2);
 if(R&&R.rows){ document.getElementById('rvsrc').textContent=`· ${R.map} ${R.class}${R.team_game?' (teams)':''}, ${R.mode||''}, ${R.players} players, ${R.minutes} min · the map before it: ${R.games_on_map_before} game(s) · ${ago(R.updated)}`;
   document.getElementById('rv').innerHTML=`<table style="border-collapse:collapse;width:100%;font-size:12.5px"><tr><th style="text-align:left">measure</th><th>this game</th><th>the map before</th><th>everything</th><th style="text-align:left">surprise</th></tr>`+
    R.rows.map(r=>{const s=r.surprise_vs_map; return `<tr style="border-top:1px solid var(--line)"><td>${E(r.measure)}</td><td style="text-align:center"><b>${pf(r.game,r.share)}</b> <span class="k">${r.game_n}</span></td><td style="text-align:center">${pf(r.map,r.share)} <span class="k">${r.map_n}</span></td><td style="text-align:center" class="k">${pf(r.all,r.share)} ${r.all_n}</td><td style="color:var(--go)">${s==null?'':(r.share?(s>0?'+':'')+s+' SE':(s>0?'+':'')+Math.round(s*100)+'%')}</td></tr>`}).join('')+'</table>'; }
 else document.getElementById('rv').innerHTML='<div class="none">no game reviewed yet (shim/tools/game-review.py)</div>';
 const LL=j.learnloop; document.getElementById('ll').innerHTML=LL&&LL.turns&&LL.turns.length?LL.turns.slice().reverse().map(t=>`<div class="row"><b>${E(t.map)}</b><span class="k">${E(t.class)} · ${t.players} players · ${ago(t.t)}</span><span class="k">${(t.surprises||[]).length} surprise(s)${Array.isArray(t.battle)?' · '+t.battle.map(b=>Object.entries(b.walkers).map(([w,v])=>'w'+w+' '+(v.likeness??'-')).join(' ')).join(' | '):t.battle?' · '+E(t.battle):''}</span></div>`).join(''):'<div class="none">not started yet (shim/tools/learn-loop.py runs after Firefight)</div>';
 const PW=j.pubwalker; if(PW){ const L=PW.live; document.getElementById('pwsrc').textContent=PW.nick+(PW.brain?` · brain built ${PW.brain}`:'');
   document.getElementById('pwlive').innerHTML=L?`<span class="row"><b>${L.kills} kills / ${L.deaths} deaths</b> <span class="k">(K/D ${(L.kills/Math.max(1,L.deaths)).toFixed(2)}) on ${E(L.map)} with ${L.players} players · ${L.on!=null?Math.round(L.on*100)+'% of its shots on target, '+Math.round((L.head||0)*100)+'% of hits on the head':''}</span></span>`:'<span class="k">not in a game</span>';
   const cnt={}; for(const r of PW.why) cnt[r.cause]=(cnt[r.cause]||0)+1; const tot=PW.why.length||1;
   document.getElementById('pwwhy').innerHTML=PW.why.length?Object.entries(cnt).sort((a,b)=>b[1]-a[1]).map(([c,n])=>`<div class="row"><b>${E(c)}</b> <span class="k">${n} (${Math.round(100*n/tot)}%)</span></div>`).join('')+`<div class="k" style="margin-top:4px">last: ${E(PW.why[PW.why.length-1].cause)} -- ${E(PW.why[PW.why.length-1].detail)}</div>`:'<span class="k">no deaths reviewed yet</span>';
   const st=(PW.tune||[]).filter(r=>r.event!=='start').slice().reverse();
   document.getElementById('pwtune').innerHTML=st.length?st.map(r=>r.event==='step'?`<div class="row"><span class="k">${ago(r.t)}</span> for <b>${E(r.cause)}</b>: ${E(r.knob)} ${r.from} → ${r.to}</div>`
      :`<div class="row"><span class="k">${ago(r.t)}</span> ${E(r.knob)} → ${r.to}: K/D ${r.kd_before??'?'} → ${r.kd_after} <b style="color:${r.kept?'var(--ok)':'#e66'}">${r.kept?'kept':'put back'}</b></div>`).join(''):'<span class="k">no steps yet (one every 6 deaths)</span>';
   document.getElementById('pwlesson').textContent=PW.lesson||'-'; }
 const W=j.wcoach; if(W){ const ks=Object.entries(W.knobs||{}); document.getElementById('wk').innerHTML=ks.length?ks.map(([w,k])=>`<div class="row"><b>walker ${E(w)}</b><span class="k">${Object.entries(k).map(([n,v])=>n.toLowerCase()+' '+(+v).toFixed(2)).join(' · ')}</span></div>`).join(''):'<div class="none">all knobs at their defaults</div>';
   document.getElementById('wr').innerHTML=(W.rounds||[]).slice().reverse().map(r=>`<div class="row"><span class="k">${ago(r.t)}</span><b>${E(r.map)}</b><span class="k">${Object.entries(r.walkers).map(([w,v])=>'w'+w+': dist '+(v.likeness??'-')+', K/D '+(v.kd??'-')+', duels won '+(v.duels_won==null?'-':Math.round(v.duels_won*100)+'%')).join(' · ')}</span></div>`).join('')||'<div class="none">no coached battle yet</div>'; }
 else document.getElementById('wk').innerHTML='<div class="none">no coached battle yet (shim/tools/walker-coach.py)</div>';
 document.getElementById('obs').textContent=(j.observer_age!=null&&j.observer_age>1800)?`not running -- the last observer run ended ${Math.round(j.observer_age/3600)} h ago (observe-public.sh; the learn loop is waiting for your go)`:((j.observer_log||[]).join('\n')||'(no observer log)'); document.getElementById('rec').textContent=(j.observer_age!=null&&j.observer_age>1800)?'':(j.recorder||[]).join('\n');
}catch(e){document.getElementById('upd').textContent='· not answering'} setTimeout(tick,3000)}
let WKS={key:'kills',dir:-1}, WKC='all', WKK=new Set(), WKOPEN=null, WKD=null;
function wkRender(){ const D=WKD, el=document.getElementById('wk2'); if(!D||!D.games){ el.innerHTML='<div class="none">no kills counted yet (shim/tools/weapon-kills.py)</div>'; return; }
  const gi=D.games.map((g,i)=>i).filter(i=>WKC==='all'||D.games[i].class===WKC), G=gi.map(i=>D.games[i]), tot=G.reduce((s,g)=>s+g.kills,0);
  document.getElementById('wksrc').textContent=`· ${G.length} games, ${tot.toLocaleString()} kills${G.some(g=>g.live)?' · a match is being recorded':''} · counted ${ago(D.updated)}`;
  const kinds=['weapon','vehicle','vehicle gun','turret','grenade','melee','other'], cls=['all','slayer','ctf','infection','other'];
  document.getElementById('wkf').innerHTML=cls.map(c=>`<a href="#learnings" data-c="${c}" style="font-size:12px;padding:2px 8px;border:1px solid ${WKC===c?'var(--acc)':'var(--line)'};border-radius:4px;color:${WKC===c?'var(--fg)':'var(--dim)'};text-decoration:none">${c}</a>`).join('')+'<span class="k" style="margin:0 6px">|</span>'+
    kinds.map(k=>`<a href="#learnings" data-k="${k}" style="font-size:12px;padding:2px 8px;border:1px solid var(--line);border-radius:4px;color:${WKK.has(k)?'var(--dim)':'var(--fg)'};text-decoration:${WKK.has(k)?'line-through':'none'}">${k}</a>`).join('');
  const R=Object.entries(D.weapons).filter(([n,w])=>!WKK.has(w.kind)).map(([n,w])=>{ const pg=gi.map(i=>w.per_game[i]||0), k=pg.reduce((a,b)=>a+b,0), used=pg.filter(x=>x).length, last=pg.length?pg[pg.length-1]:0;
    return {n, kind:w.kind, kills:k, share:tot?k/tot:0, hs:k?w.headshots*(k/Math.max(1,w.kills))/k:0, used, per:used?k/used:0, best:Math.max(0,...pg), last, pg, betr:w.betrayals}; }).filter(r=>r.kills>0);
  R.sort((a,b)=>{const x=a[WKS.key],y=b[WKS.key]; return (typeof x==='string'?x.localeCompare(y):(x-y))*WKS.dir;});
  const spark=pg=>{ const m=Math.max(1,...pg), W=180, H=22; if(pg.length<2) return ''; const pts=pg.map((v,i)=>`${(i*(W/(pg.length-1))).toFixed(1)},${(H-1-v/m*(H-2)).toFixed(1)}`).join(' ');
    return `<svg width="${W}" height="${H}" style="vertical-align:middle"><polyline points="${pts}" fill="none" stroke="var(--acc)" stroke-width="1.5"/></svg>`; };
  const cols=[['n','weapon'],['kind','kind'],['kills','kills'],['share','share'],['hs','headshots'],['used','games'],['per','kills a game'],['best','best game'],['last','last game'],['pg','game by game']];
  el.innerHTML=`<table style="border-collapse:collapse;width:100%;font-size:12.5px"><tr>${cols.map(([k,l])=>`<th data-s="${k}" style="text-align:${k==='n'||k==='kind'||k==='pg'?'left':'right'};cursor:${k==='pg'?'default':'pointer'};white-space:nowrap;padding:3px 6px">${l}${WKS.key===k?(WKS.dir<0?' ▾':' ▴'):''}</th>`).join('')}</tr>`+
    R.map(r=>`<tr data-w="${E(r.n)}" style="border-top:1px solid var(--line);cursor:pointer"><td style="padding:3px 6px"><b>${E(r.n)}</b></td><td class="k" style="padding:3px 6px">${E(r.kind)}</td><td style="text-align:right;padding:3px 6px"><b>${r.kills.toLocaleString()}</b></td>
      <td style="text-align:right;padding:3px 6px">${(r.share*100).toFixed(1)}%</td><td style="text-align:right;padding:3px 6px" class="k">${r.hs?Math.round(r.hs*100)+'%':''}</td><td style="text-align:right;padding:3px 6px">${r.used}</td>
      <td style="text-align:right;padding:3px 6px">${r.per.toFixed(1)}</td><td style="text-align:right;padding:3px 6px">${r.best}</td><td style="text-align:right;padding:3px 6px">${r.last}</td><td style="padding:3px 6px">${spark(r.pg)}</td></tr>`+
      (WKOPEN===r.n?`<tr><td colspan="10" style="padding:4px 6px 10px"><div style="display:flex;gap:3px;align-items:flex-end;overflow-x:auto;padding-bottom:4px">${r.pg.map((v,i)=>{const g=G[i], m=Math.max(1,...r.pg);
        return `<div title="${E(g.map)} · ${E(g.mode||g.class)} · ${g.players} players · ${v} of ${g.kills} kills" style="display:flex;flex-direction:column;align-items:center;min-width:34px"><span class="k" style="font-size:10px">${v}</span><div style="width:22px;height:${Math.max(1,Math.round(v/m*60))}px;background:${g.live?'var(--go)':'var(--acc)'};border-radius:2px"></div><span class="k" style="font-size:9.5px;max-width:40px;overflow:hidden;white-space:nowrap">${E((g.map||'?').slice(0,7))}</span></div>`}).join('')}</div></td></tr>`:'')).join('')+'</table>'; }
document.addEventListener('click',e=>{ const t=e.target.closest('#wk2 th[data-s],#wk2 tr[data-w],#wkf a'); if(!t) return;
  if(t.dataset.s&&t.dataset.s!=='pg'){ WKS=WKS.key===t.dataset.s?{key:WKS.key,dir:-WKS.dir}:{key:t.dataset.s,dir:t.dataset.s==='n'||t.dataset.s==='kind'?1:-1}; }
  else if(t.dataset.w!==undefined){ WKOPEN=WKOPEN===t.dataset.w?null:t.dataset.w; }
  else if(t.dataset.c){ e.preventDefault(); WKC=t.dataset.c; } else if(t.dataset.k){ e.preventDefault(); WKK.has(t.dataset.k)?WKK.delete(t.dataset.k):WKK.add(t.dataset.k); }
  try{ localStorage.setItem('wk', JSON.stringify({s:WKS,c:WKC,k:[...WKK]})); }catch(_){} wkRender(); });
try{ const v=JSON.parse(localStorage.getItem('wk')||'null'); if(v){ WKS=v.s||WKS; WKC=v.c||WKC; WKK=new Set(v.k||[]); } }catch(_){}
async function youDo(d){ try{ await fetch('/you?do='+d,{method:'POST',headers:{'X-Workboard':'1'}}); }catch(e){} tick(); }
document.addEventListener('click',e=>{ if(e.target.id==='ystart') youDo('start'); else if(e.target.id==='ystop') youDo('stop'); });
function showView(){ const v=location.hash==='#learnings'?'learnings':location.hash==='#you'?'you':location.hash==='#walkers'?'walkers':location.hash==='#brain'?'brain':'work';
  document.getElementById('brainview').hidden=v!=='brain'; document.getElementById('walkview').hidden=v!=='walkers'; document.getElementById('learnview').hidden=v!=='learnings'; document.getElementById('youview').hidden=v!=='you'; document.getElementById('workview').hidden=v!=='work';
  for(const a of document.querySelectorAll('#tabs a')) a.classList.toggle('on',a.dataset.v===v); }
window.addEventListener('hashchange',showView); showView();
tick();</script></body></html>'''

# YOUR SESSIONS (2026-11-09; the user: "the you page should only be activated by a start and stop button on that page btw. and clear
# stats that are manufactured and only use real stats from started sessions"): Start / Stop on the You tab -> workboard/you-sessions.json
# ({"sessions": [{"start", "end"}]}); coach.py counts only matches that start inside one (never a walker's recording: walkerrec.py).
# Stop (and Start) re-read the recordings at once: engage-extract.py + coach.py at background priority.
YOU = os.path.join(WB, 'you-sessions.json')
def you_load():
    try: return json.load(open(YOU)).get('sessions', [])
    except Exception: return []
def you_save(S):
    os.makedirs(WB, exist_ok=True); tmp = YOU + '.tmp'; json.dump({'sessions': S}, open(tmp, 'w'), indent=1); os.replace(tmp, YOU)
_you_job = {'p': None}
def you_refresh():
    if _you_job['p'] and _you_job['p'].poll() is None: return
    here = os.path.dirname(os.path.abspath(__file__))
    _you_job['p'] = subprocess.Popen(['bash', '-c', 'taskpolicy -b nice -n 20 python3 "$0"/engage-extract.py --quiet >/dev/null 2>&1; taskpolicy -b nice -n 20 python3 "$0"/coach.py --quiet', here],
                                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, cwd=os.path.dirname(os.path.dirname(here)))
def you_control(do):
    S = you_load(); now = time.time(); open_ = S and not S[-1].get('end')
    if do == 'start' and not open_: S.append({'start': now, 'end': None})
    elif do == 'stop' and open_: S[-1]['end'] = now
    else: return False
    you_save(S); you_refresh(); return True

class H(BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def do_POST(self):   # (the page's Start / Stop; a custom header: a page on another site cannot send it without a preflight we never answer)
        ok = self.path.startswith('/you?do=') and self.headers.get('X-Workboard') == '1' and you_control(self.path.split('do=', 1)[1].split('&')[0])
        b = json.dumps({'ok': bool(ok), 'sessions': you_load()}).encode()
        self.send_response(200 if ok else 400); self.send_header('Content-Type', 'application/json'); self.send_header('Content-Length', str(len(b))); self.end_headers(); self.wfile.write(b)
    def do_GET(self):
        if self.path.startswith('/status.json'): b = json.dumps(status()).encode(); ct = 'application/json'
        else: b = PAGE.encode(); ct = 'text/html; charset=utf-8'
        self.send_response(200); self.send_header('Content-Type', ct); self.send_header('Content-Length', str(len(b))); self.send_header('Cache-Control', 'no-store'); self.end_headers(); self.wfile.write(b)

def wk_loop():   # the weapon kills table, recounted every minute (shim/tools/weapon-kills.py: only new / growing recordings are read)
    import importlib.util, threading
    sp = importlib.util.spec_from_file_location('weapon_kills', os.path.join(os.path.dirname(os.path.abspath(__file__)), 'weapon-kills.py')); m = importlib.util.module_from_spec(sp); sp.loader.exec_module(m)
    def run():
        while True:
            try: m.update(quiet=True)
            except Exception as e: print('weapon kills:', e, flush=True)
            time.sleep(60)
    threading.Thread(target=run, daemon=True).start()

if __name__ == '__main__':
    if len(sys.argv) > 3 and sys.argv[1] == 'task': tasks_cmd(); sys.exit(0)
    if len(sys.argv) > 1 and sys.argv[1] == 'direction': direction_cmd(); sys.exit(0)
    wk_loop()
    port = int(arg('--port', '8790')); srv = ThreadingHTTPServer(('127.0.0.1', port), H); srv.daemon_threads = True
    print('work board: http://127.0.0.1:%d/' % port, flush=True); srv.serve_forever()
