#!/bin/bash
# Who does the client talk to when it joins a server? (README "Who sees your IP address".) Starts ONE hidden, input-less
# client (shim/tools/quiet-run.sh, its own test profile, no microphone) with a join link, records every datagram and TCP
# connection its sockets see (the packet log, GET /packets.json every second; halo3's messages with their peers), the stats
# snapshots (session list: does anything carry an address?) and lsof of the client's processes, stays SECS in the match,
# then stops its own process group and prints: UDP peers before / during the match, TCP connections, halo3 message peers,
# talkers seen, addresses in the session list, sockets per process.
#     bash shim/tools/privacy-check.sh <host:port> [secs in the match, default 200]
#   OUT=<dir>   keep the raw records (default: a temp dir, removed at the end)
# Joining a PUBLIC server shows your address to it (and the client's start-up status queries to every listed server):
# run it only when that is fine. Refuses to start beside another game (quiet-run.sh's check).
set -u
cd "$(dirname "$0")/../.."
SRV="${1:?usage: privacy-check.sh <host:port> [secs]}"; INMATCH="${2:-200}"
D="${OUT:-$(mktemp -d -t reclaimer_privacy)}"; mkdir -p "$D"; KEEP="${OUT:-}"
rm -f "$D"/*.json "$D"/*.jsonl "$D"/*.txt "$D"/loaded
QR_HIDE=1 QR_STATS_OPEN=0 bash shim/tools/quiet-run.sh "PELOAD_PKTLOG_GAME=1" 900 "reclaimer://join?server=$SRV" > /dev/null 2>&1 &
sleep 3; LOG=/tmp/quiet-run.log
seq=0; t0=$(date +%s); loaded=0
# every second: the start-up status burst writes ~2,000 records a second, and /packets.json returns only the newest max
# (4000) after since -- a 5 s poll lost part of it
while :; do
  sleep 1; now=$(date +%s)
  if curl -s -m 4 "http://127.0.0.1:8765/packets.json?max=4000&since=$seq" > "$D/last.json" 2>/dev/null && [ -s "$D/last.json" ]; then
    seq=$(python3 - "$D/last.json" "$D/events.jsonl" "$seq" <<'PY'
import json,sys
try: j=json.load(open(sys.argv[1]))
except Exception: print(sys.argv[3]); sys.exit()
with open(sys.argv[2],'a') as f:
    for e in j.get('events',[]): f.write(json.dumps(e)+'\n')
print(j.get('seq',sys.argv[3]))
PY
)
  fi
  tick=$((${tick:-0}+1)); [ $((tick%5)) = 0 ] || continue   # the rest every ~5 s
  curl -s -m 4 http://127.0.0.1:8765/stats.json > "$D/stats_$now.json" 2>/dev/null
  if [ -f /tmp/quiet-run.pgid ]; then pg=$(cat /tmp/quiet-run.pgid)
    for p in $(pgrep -g "$pg"); do echo "== $now pid $p"; lsof -nP -a -p "$p" -i 2>/dev/null | tail -n +2; done >> "$D/lsof.txt"; fi
  if [ "$loaded" = 0 ] && LC_ALL=C grep -aq "match loaded in" "$LOG" 2>/dev/null; then loaded=$now; echo "loaded $now" > "$D/loaded"; fi
  LC_ALL=C grep -a -o "Joining server failed[^\"]*\|This server is full[^\"]*\|\[CRASH\][^\"]*" "$LOG" 2>/dev/null | head -3 > "$D/fail.txt"
  [ -s "$D/fail.txt" ] && break
  [ "$loaded" != 0 ] && [ $((now-loaded)) -ge "$INMATCH" ] && break
  [ $((now-t0)) -ge 600 ] && { echo "no match within 10 minutes" > "$D/fail.txt"; break; }
done
[ -f /tmp/quiet-run.pgid ] && kill -9 -- -"$(cat /tmp/quiet-run.pgid)" 2>/dev/null; wait 2>/dev/null
[ -s "$D/fail.txt" ] && { echo "join failed: $(cat "$D/fail.txt")"; }
python3 - "$D" "$SRV" <<'PY'
import json,sys,glob,os,collections
D,SRV=sys.argv[1],sys.argv[2]
ev=[json.loads(l) for l in open(f'{D}/events.jsonl')] if os.path.exists(f'{D}/events.jsonl') else []
loaded=float(open(f'{D}/loaded').read().split()[1]) if os.path.exists(f'{D}/loaded') else None
print(f'{len(ev)} records; ' + ('match loaded' if loaded else 'the match never loaded'))
if not ev: sys.exit()
ref=loaded or min(e['t'] for e in ev); T=lambda t:f'{t-ref:+.1f}s'
udp=collections.defaultdict(lambda:{'tx':0,'rx':0,'t0':1e18,'t1':0,'in':0})
tcp=[]
for e in ev:
    if e['layer']!='wire': continue
    if e['type'].startswith('tcp-'): tcp.append(e); continue
    u=udp[e.get('peer','-')]; u[e['dir']]+=1; u['t0']=min(u['t0'],e['t']); u['t1']=max(u['t1'],e['t'])
    if loaded and e['t']>=loaded: u['in']+=1
srv=udp.get(SRV)
if srv: print(f"the server {SRV}: tx {srv['tx']} rx {srv['rx']} ({srv['in']} in the match)")
svc=[k for k in udp if k!=SRV and k.endswith(':49175')]
gs=[k for k in udp if k!=SRV and k not in svc]
if gs: print(f"other game servers: {len(gs)} on {len({k.rsplit(':',1)[0] for k in gs})} machines, {sum(1 for k in gs if udp[k]['rx'])} answered, "
              f"{T(min(udp[k]['t0'] for k in gs))} .. {T(max(udp[k]['t1'] for k in gs))} (relative to the match load)")
for k in svc: print(f"service port 49175: {k} tx {udp[k]['tx']} rx {udp[k]['rx']} at {T(udp[k]['t0'])}")
during=[k for k,u in udp.items() if u['in'] and k!=SRV]
print('UDP peers other than the server DURING the match:', ', '.join(during) if during else 'none')
print('TCP:'); [print(f"  {T(e['t'])} {e['type']:<12} {e.get('peer','-'):<24} {e.get('host','')} {e['len'] if e['type']=='tcp-close' else ''}") for e in tcp]
g=collections.Counter(e.get('peer','(no peer)') for e in ev if e['layer']=='game')
print('halo3 messages by peer:', dict(g) or 'none (game layer off?)')
talk=set(); ips=0; most=0; n=0
for f in sorted(glob.glob(f'{D}/stats_*.json')):
    try: j=json.load(open(f))
    except Exception: continue
    n+=1; s=j.get('session',[]); most=max(most,len(s))
    talk|={p.get('voice') for p in s if p.get('talking')}; ips+=sum('ip' in p for p in s+j.get('players',[]))
print(f'stats snapshots {n}: session up to {most} players, {len(talk)} talkers seen, entries carrying an address: {ips}')
if os.path.exists(f'{D}/lsof.txt'):
    per=collections.defaultdict(set); pid=None
    for l in open(f'{D}/lsof.txt'):
        if l.startswith('=='): pid=l.split()[3]; continue
        p=l.split()
        if len(p)>=9 and p[7] in ('TCP','UDP') and not p[8].startswith('127.0.0.1') : per[pid].add(p[7]+' '+' '.join(p[8:]))
    pids={l.split()[3] for l in open(f'{D}/lsof.txt') if l.startswith('==')}
    for p in sorted(pids): print(f'process {p}: {len(per[p])} non-loopback sockets' + (f" ({', '.join(sorted(per[p])[:4])}{' ...' if len(per[p])>4 else ''})" if per[p] else ''))
PY
[ -z "$KEEP" ] && rm -rf "$D"
