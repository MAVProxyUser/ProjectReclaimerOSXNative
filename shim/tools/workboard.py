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
         ('refresh: new recordings', 'Learning refresh (every 20 min)')]
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
    return {'now': time.time(), 'jobs': jobs, 'apps': apps, 'direction': direction, 'tome': tome_load(), 'per_map_models': per_map, 'learnings': learn, 'observer_log': tail(obs[0], 8) if obs else [], 'recorder': tail(rec[0], 4) if rec else [],
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
<h1>Work board <nav id="tabs"><a href="#" data-v="work">Work</a><a href="#learnings" data-v="learnings">Learnings</a></nav> <span class="k" id="upd"></span></h1>
<div class="k">What is running on this Mac for Reclaimer, what is being worked on, and what the game is learning from real players. Refreshes every 3 s.</div><br>
<div id="learnview" hidden><div class="p"><h2>What the game is learning <span class="k" id="lrsrc"></span></h2><div class="k" style="margin-bottom:8px">Plain-language findings from every recorded public match (Infection left out of the fighting findings), each with the number of cases it rests on; thin ones (under 30 cases) are dimmed. Rebuilt every 20 minutes while sampling runs.</div><div id="lrn" class="lgrid"></div></div></div>
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
 document.getElementById('obs').textContent=(j.observer_log||[]).join('\n')||'(no observer log)'; document.getElementById('rec').textContent=(j.recorder||[]).join('\n');
}catch(e){document.getElementById('upd').textContent='· not answering'} setTimeout(tick,3000)}
function showView(){ const l=location.hash==='#learnings'; document.getElementById('learnview').hidden=!l; document.getElementById('workview').hidden=l;
  for(const a of document.querySelectorAll('#tabs a')) a.classList.toggle('on',(a.dataset.v==='learnings')===l); }
window.addEventListener('hashchange',showView); showView();
tick();</script></body></html>'''

class H(BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def do_GET(self):
        if self.path.startswith('/status.json'): b = json.dumps(status()).encode(); ct = 'application/json'
        else: b = PAGE.encode(); ct = 'text/html; charset=utf-8'
        self.send_response(200); self.send_header('Content-Type', ct); self.send_header('Content-Length', str(len(b))); self.send_header('Cache-Control', 'no-store'); self.end_headers(); self.wfile.write(b)

if __name__ == '__main__':
    if len(sys.argv) > 3 and sys.argv[1] == 'task': tasks_cmd(); sys.exit(0)
    if len(sys.argv) > 1 and sys.argv[1] == 'direction': direction_cmd(); sys.exit(0)
    port = int(arg('--port', '8790')); srv = ThreadingHTTPServer(('127.0.0.1', port), H); srv.daemon_threads = True
    print('work board: http://127.0.0.1:%d/' % port, flush=True); srv.serve_forever()
