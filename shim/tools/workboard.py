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
         ('walker-coach.py', 'Walker self-coaching'), ('game-review.py', 'Game review'), ('coach.py', 'Coach (you vs the winners)')]
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
    return {'wkills': wk, 'review': review, 'learnloop': ll, 'wcoach': wc, 'now': time.time(), 'jobs': jobs, 'apps': apps, 'direction': direction, 'tome': tome_load(), 'per_map_models': per_map, 'learnings': learn, 'coach': coach, 'observer_log': tail(obs[0], 8) if obs else [], 'recorder': tail(rec[0], 4) if rec else [],
            'live': live, 'totals': totals(), 'tasks': tasks_load()}

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
.lcard h3{margin:0 0 6px;font-size:13px;color:var(--go)}.li{padding:4px 0;border-bottom:1px solid var(--line);font-size:13px}.li:last-child{border:0}.li b{color:var(--fg)}
.li .n{color:var(--dim);font-size:11px;margin-left:4px}.li.weak{opacity:.55}
#tabs{display:inline-flex;gap:4px;margin-left:12px;vertical-align:middle}#tabs a{font-size:13px;font-weight:500;color:var(--dim);text-decoration:none;padding:3px 10px;border:1px solid var(--line);border-radius:4px}
#tabs a.on{color:var(--fg);border-color:var(--acc)}#tabs a:hover{color:var(--fg)}</style></head><body>
<h1>Work board <nav id="tabs"><a href="#" data-v="work">Work</a><a href="#learnings" data-v="learnings">Learnings</a><a href="#you" data-v="you">You</a><a href="#walkers" data-v="walkers">Walkers</a></nav> <span class="k" id="upd"></span></h1>
<div class="k">What is running on this Mac for Reclaimer, what is being worked on, and what the game is learning from real players. Refreshes every 3 s.</div><br>
<div id="learnview" hidden><div class="p" style="margin-bottom:14px"><h2>Kills by weapon <span class="k" id="wksrc"></span></h2>
<div class="k" style="margin-bottom:8px">Every kill in the game's own kill feed of every recorded public match (our local servers left out), by what made it: weapons, vehicles (the driver's weapon or a splatter), vehicle guns (gunner seats), turrets, grenades, melee. Headshots are counted in their weapon. Click a column to sort, a row for its kills game by game; the line is its kills per game, oldest to newest. Recounted every minute (the match being recorded is in it).</div>
<div id="wkf" style="display:flex;flex-wrap:wrap;gap:6px;margin-bottom:8px"></div><div id="wk2" style="overflow-x:auto"></div></div><div class="p"><h2>What the game is learning <span class="k" id="lrsrc"></span></h2><div class="k" style="margin-bottom:8px">Plain-language findings from every recorded public match (Infection left out of the fighting findings), each with the number of cases it rests on; thin ones (under 30 cases) are dimmed. Rebuilt every 20 minutes while sampling runs.</div><div id="lrn" class="lgrid"></div></div></div>
<div id="youview" hidden><div class="p"><h2>You vs the winners <span class="k" id="ysrc"></span></h2><div class="k" style="margin-bottom:8px">Your public matches (played through our launcher: the recorder marks your slot) next to the WINNERS -- the top quarter of players by K/D in non-Infection matches -- and everyone, measure by measure; the biggest gaps first, each with what the winners do differently. shim/tools/coach.py.</div><div id="you"></div></div></div>
<div id="walkview" hidden>
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
   document.getElementById('lrn').innerHTML=L.sections.map(sc=>`<div class="lcard"><h3>${E(sc.topic)}</h3>${sc.items.map(i=>`<div class="li${i.weak?' weak':''}"><b>${E(i.title)}</b> ${E(i.text)}<span class="n">n=${(i.n||0).toLocaleString()}${i.weak?' · thin':''}</span></div>`).join('')||'<div class="none">nothing yet</div>'}</div>`).join(''); }
 else document.getElementById('lrn').innerHTML='<div class="none">no findings yet (shim/tools/learnings.py)</div>';
 const C=j.coach; if(C&&C.rows){ document.getElementById('ysrc').textContent=`· ${C.pretend?'EXAMPLE (a recorded player stands in for you until your own matches are recorded) · ':''}${C.you.matches} match(es), K/D ${C.you.kd??'-'} · winners: ${C.winners.players} players, K/D ${C.winners.kd_from}+ · refreshed ${ago(C.updated)}`;
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
 const W=j.wcoach; if(W){ const ks=Object.entries(W.knobs||{}); document.getElementById('wk').innerHTML=ks.length?ks.map(([w,k])=>`<div class="row"><b>walker ${E(w)}</b><span class="k">${Object.entries(k).map(([n,v])=>n.toLowerCase()+' '+(+v).toFixed(2)).join(' · ')}</span></div>`).join(''):'<div class="none">all knobs at their defaults</div>';
   document.getElementById('wr').innerHTML=(W.rounds||[]).slice().reverse().map(r=>`<div class="row"><span class="k">${ago(r.t)}</span><b>${E(r.map)}</b><span class="k">${Object.entries(r.walkers).map(([w,v])=>'w'+w+': dist '+(v.likeness??'-')+', K/D '+(v.kd??'-')+', duels won '+(v.duels_won==null?'-':Math.round(v.duels_won*100)+'%')).join(' · ')}</span></div>`).join('')||'<div class="none">no coached battle yet</div>'; }
 else document.getElementById('wk').innerHTML='<div class="none">no coached battle yet (shim/tools/walker-coach.py)</div>';
 document.getElementById('obs').textContent=(j.observer_log||[]).join('\n')||'(no observer log)'; document.getElementById('rec').textContent=(j.recorder||[]).join('\n');
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
function showView(){ const v=location.hash==='#learnings'?'learnings':location.hash==='#you'?'you':location.hash==='#walkers'?'walkers':'work';
  document.getElementById('walkview').hidden=v!=='walkers'; document.getElementById('learnview').hidden=v!=='learnings'; document.getElementById('youview').hidden=v!=='you'; document.getElementById('workview').hidden=v!=='work';
  for(const a of document.querySelectorAll('#tabs a')) a.classList.toggle('on',a.dataset.v===v); }
window.addEventListener('hashchange',showView); showView();
tick();</script></body></html>'''

class H(BaseHTTPRequestHandler):
    def log_message(self, *a): pass
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
