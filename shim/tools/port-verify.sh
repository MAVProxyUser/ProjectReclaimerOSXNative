#!/bin/bash
# Did the port of a client build work? Every patch point / hook gets PASS, FAIL or SKIP (shim/docs/porting.md, step 5).
#   bash shim/tools/port-verify.sh                       # the newest client/ exe: static + loader + live join
#   bash shim/tools/port-verify.sh client/x.exe          # a given exe
#   PV_LIVE=0 bash shim/tools/port-verify.sh             # static + loader checks only (nothing is started but --version)
# Stages:
#  1 static  re-tools/patchscan.py on the exe: every point found by the signatures in the C sources, with its meaning checks
#  2 loader  the loader itself on the exe (--version: it applies its patches and installs its hooks at exe load, prints
#            [sigpatch] / [stats] lines and exits): every point reported found, at the SAME address patchscan found -- two
#            independent implementations (C at load time, Python on the file) agreeing is the check that neither has a bug
#  3 live    the local dedicated server (shim/dedicated.sh, started on this exe if it isn't running on it) + ONE hidden,
#            input-less client (shim/tools/quiet-run.sh, QR_HIDE) joining it:
#            ui-reentrancy  the menu is reached and draws (the client's own menu, > 50 draws a frame)
#            xaudio1-4      no "XAudio2 import slots differ ... refused" (the client's own check, which our patches bypass)
#            watchdog       static + loader only (it matters for RECLAIMER_DIRECT runs; PV_DIRECT=1 also runs one: no
#                           "still running after" exit within PV_DIRECT_SECS)
#            join           "match loaded in" (the whole path: menu, join, map load)
#            max_players    the client's stats snapshot says slots.src = "server" (read through the hosted-session slot)
#            moderation     the SERVER's stats (8767) list the joined player's address (the host's moderation table)
#            push_history   a [Server] line sent with the server's RCON (`dedicated rcon say`) reaches the client's feed
#            set_muted / voice_recv / cast_vote   static + loader only (need a second talking player / a map vote)
#            no [CRASH], no "NOT applied", no other "... refused" line (listed; the Bink one is a known older one, below)
# Exit 0 = no FAIL. Never runs beside the user's game (quiet-run refuses); stops only its own client (its process group,
# /tmp/quiet-run.pgid) and only a server it started itself. Logs: /tmp/port-verify/.
{   # (one compound command: bash reads all of it before running it, so editing the file meanwhile is safe)
cd "$(dirname "$0")/../.."
EXE="${1:-$(ls client/project-reclaimer-client-v*.exe 2>/dev/null | sort -V | tail -1)}"
[ -f "$EXE" ] || { echo "no client exe ($EXE)"; exit 1; }
case "$(cd "$(dirname "$EXE")" && pwd)" in "$HOME/Library/Application Support/ReclaimerMac/releases"*)
  echo "that is a corpus copy of an old release: data only, never run (servers refuse old clients; use re-tools/patchscan.py)"; exit 1;; esac
[ -x shim/build/peload ] || { echo "build the loader first: bash shim/build.sh"; exit 1; }
STATE="$HOME/Library/Application Support/ReclaimerMac"; OUT=/tmp/port-verify; mkdir -p "$OUT"; rm -f "$OUT"/*
res(){ printf '  %-5s %-14s %s\n' "$1" "$2" "$3" | tee -a "$OUT/summary.txt"; }   # (counted from summary.txt at the end)
echo "== $EXE ($(shasum -a 256 "$EXE" | cut -c1-16)...)"

# ---- 1 static -------------------------------------------------------------------------------------------------------------
echo "== 1 static (re-tools/patchscan.py)"
python3 re-tools/patchscan.py --json "$EXE" > "$OUT/patchscan.json" 2> "$OUT/patchscan.err"; PS_RC=$?
[ $PS_RC -eq 2 ] && { cat "$OUT/patchscan.err"; echo "patchscan does not understand the sources any more: update it first"; exit 2; }

# ---- 2 loader -------------------------------------------------------------------------------------------------------------
echo "== 2 loader (--version run)"
if [ -n "$(pgrep -f '^[^ ]*build/peload.*--version')" ]; then echo "another --version run is going on"; exit 3; fi
PELOAD_RECLAIMER_PATCHES=all PELOAD_STATS_HTTP=8799 PELOAD_STATS_OPEN=0 perl -e 'alarm 60; exec @ARGV' \
  arch -x86_64 shim/build/peload "$EXE" --version > "$OUT/version.log" 2>&1
python3 - "$OUT/patchscan.json" "$OUT/version.log" <<'EOF' | tee -a "$OUT/summary.txt"
import json, re, sys
r = json.load(open(sys.argv[1]))[0]; log = open(sys.argv[2], errors='replace').read()
def out(v, k, t): print(f'  {v:<5} {k:<14} {t}')
sp = {m.group(1): set() for m in re.finditer(r'\[sigpatch\] (\S+) @', log)}
for m in re.finditer(r'\[sigpatch\] (\S+) @ exe\+0x([0-9a-f]+)', log): sp[m.group(1)].add(int(m.group(2), 16))
bad = re.findall(r'\[sigpatch\] (\S+) \((\S+)\): ([^\n]*NOT applied)', log)
def st(k): return r[k]['status'] == 'ok'
for k, name in (('ui-reentrancy', 'ui-reentrancy'), ('xaudio1', 'xaudio'), ('xaudio2', 'xaudio'), ('xaudio3', 'xaudio'), ('xaudio4', 'xaudio'), ('watchdog', 'watchdog')):
    lr = k in [b[1] for b in bad]
    if st(k) and r[k]['rva'] in sp.get(name, ()): out('PASS', k, f"exe+0x{r[k]['rva']:x} (patchscan and loader agree)")
    elif st(k): out('FAIL', k, f"patchscan exe+0x{r[k]['rva']:x}, loader: " + (', '.join(hex(x) for x in sorted(sp.get(name, ()))) or 'not applied'))
    else: out('FAIL', k, f"patchscan: {r[k]['status']}" + ('; loader: NOT applied' if lr else ''))
# the loader applies EVERY variant that matches (patchscan reports the first per slot): a place it patched that patchscan
# did not find = two variants matching one build (a too-loose new signature) or a disagreement between the two
for name, slots in (('ui-reentrancy', ['ui-reentrancy']), ('xaudio', ['xaudio1', 'xaudio2', 'xaudio3', 'xaudio4']), ('watchdog', ['watchdog'])):
    extra = sp.get(name, set()) - {r[k]['rva'] for k in slots if st(k)}
    if extra: out('FAIL', name, 'the loader also patched ' + ', '.join(f'exe+0x{x:x}' for x in sorted(extra)) + ' (patchscan: not a patch place here)')
for name, slot, why in bad: out('FAIL', slot, f'loader: {why}')
def chk(k, ok_loader, detail_ok, why):
    if st(k) and ok_loader: out('PASS', k, detail_ok + ' (patchscan and loader agree)')
    elif st(k): out('FAIL', k, f'patchscan {detail_ok}; loader: {why}')
    else: out('FAIL', k, f"patchscan: {r[k]['status']}; loader: {'found' if ok_loader else why}")
def at(pat):
    m = re.search(pat + r' exe\+0x([0-9a-f]+)', log); return int(m.group(1), 16) if m else None
def hx(v): return 'not found' if v is None else f'exe+0x{v:x}'
chk('set_muted', st('set_muted') and at(r'mute available \(set_muted') == r['set_muted']['rva'], f"exe+0x{r['set_muted'].get('rva', 0):x}", hx(at(r'set_muted')))
chk('voice_recv', st('voice_recv') and at(r'hooked \(voice_recv') == r['voice_recv']['rva'], f"exe+0x{r['voice_recv'].get('rva', 0):x} (entry hook)", hx(at(r'voice_recv')))
chk('push_history', st('push_history') and at(r'push_history') == r['push_history']['rva'], f"exe+0x{r['push_history'].get('rva', 0):x} (entry hook)", hx(at(r'push_history')))
m = re.search(r'table lock exe\+0x([0-9a-f]+), players at lock\+0x([0-9a-f]+)', log); mo = r['moderation']
chk('moderation', bool(m) and st('moderation') and int(m.group(1), 16) == mo['rva'] and int(m.group(2), 16) == mo['players'],
    f"lock exe+0x{mo.get('rva', 0):x}, players lock+0x{mo.get('players', 0):x}", m.group(0) if m else 'host table not found')
m = re.search(r'server max at exe\+0x([0-9a-f]+) \+0x([0-9a-f]+)', log); mp = r['max_players']
chk('max_players', bool(m) and st('max_players') and int(m.group(1), 16) == mp['slot'] and int(m.group(2), 16) == mp['off'],
    f"slot exe+0x{mp.get('slot', 0):x} +0x{mp.get('off', 0):x}", m.group(0) if m else 'getter not found')
m = re.search(r'cast_vote exe\+0x([0-9a-f]+), ballot cache exe\+0x([0-9a-f]+) \(0x([0-9a-f]+) bytes\)', log); cv = r['cast_vote']
chk('cast_vote', bool(m) and st('cast_vote') and int(m.group(1), 16) == cv['rva'] and int(m.group(2), 16) == cv['cache'] and int(m.group(3), 16) == cv['size'],
    f"exe+0x{cv.get('rva', 0):x}, ballot exe+0x{cv.get('cache', 0):x} ({cv.get('size', 0):#x} bytes)", m.group(0) if m else 'not found')
v = re.search(r'^project-reclaimer\S* (\S+)', log, re.M); print(f"  {'note':<5} {'version':<14} {v.group(1) if v else '(no version line: the loader did not reach the client main?)'}")
EOF

# ---- 3 live ---------------------------------------------------------------------------------------------------------------
if [ "${PV_LIVE:-1}" = 0 ]; then echo "== 3 live: skipped (PV_LIVE=0)"; else
echo "== 3 live (local dedicated server + one hidden client)"
PGF="$STATE/dedicated.pgid"; STARTED=""; SPG="$(cat "$PGF" 2>/dev/null)"
srv_exe(){ [ -n "$SPG" ] && ps -o command= -p "$SPG" 2>/dev/null | awk '{print $2}'; }
if [ -n "$SPG" ] && kill -0 "$SPG" 2>/dev/null && [ "$(basename "$(srv_exe)")" != "$(basename "$EXE")" ]; then
  if [ "${PV_RESTART_SERVER:-}" = 1 ]; then bash shim/dedicated.sh stop > /dev/null; SPG=""
  else res SKIP live "the dedicated server runs another exe ($(srv_exe)); PV_RESTART_SERVER=1 restarts it on this one"; SPG=x; fi
fi
if [ "$SPG" != x ]; then
  if [ -z "$SPG" ] || ! kill -0 "$SPG" 2>/dev/null; then
    RECLAIMER_EXE="$EXE" bash shim/dedicated.sh > "$OUT/dedicated-start.txt" 2>&1; STARTED=1; SPG="$(cat "$PGF" 2>/dev/null)"
    for i in $(seq 1 90); do curl -s -m 2 http://127.0.0.1:8767/stats.json > /dev/null 2>&1 && break; sleep 2; done
    sleep 20   # its lobby comes up after the stats page
  fi
  PORT="$(grep -m1 -A3 '^\[\[server\]\]' "$STATE/prefix/drive_c/ReclaimerTestServer/dedicated.toml" 2>/dev/null | grep -m1 '^port' | tr -dc 0-9)"; PORT="${PORT:-49190}"
  # one hidden, input-less client joining it; "Public join timed out" right after a client was killed is the server still
  # holding that one (~45 s): retried once after a minute
  client(){ rm -f /tmp/quiet-run.log /tmp/quiet-run.pgid
    QR_HIDE=1 QR_STATS=8765 QR_STATS_OPEN=0 QR_EXE="$EXE" bash shim/tools/quiet-run.sh "PELOAD_FEED_LOG=1" "${PV_SECS:-300}" \
      "reclaimer://join?server=127.0.0.1:$PORT" > "$OUT/quiet-run.txt" 2>&1 &
    QR=$!; MENU=""; MATCH=""; MDRAWS=0; JOINFAIL=""
    for i in $(seq 1 "${PV_SECS:-300}"); do sleep 1
      grep -aq 'a game is already running' "$OUT/quiet-run.txt" 2>/dev/null && break
      [ -f /tmp/quiet-run.log ] || continue
      [ -z "$MENU" ] && MENU=$(LC_ALL=C grep -a -m1 -o 'menu loaded in [0-9.]* s' /tmp/quiet-run.log)
      if [ -n "$MENU" ]; then d=$(LC_ALL=C grep -a 'menu loaded in' /tmp/quiet-run.log | sed -nE 's/.*\| +([0-9]+) draws\/frame.*/\1/p' | sort -n | tail -1); [ -n "$d" ] && MDRAWS=$d; fi
      MATCH=$(LC_ALL=C grep -a -m1 -o 'match loaded in [0-9.]* s' /tmp/quiet-run.log); [ -n "$MATCH" ] && break
      JOINFAIL=$(LC_ALL=C grep -a -m1 -o 'Joining server failed: [^;]*' /tmp/quiet-run.log); [ -n "$JOINFAIL" ] && break
      LC_ALL=C grep -aq '\[CRASH\]' /tmp/quiet-run.log && break
    done; }
  stop_client(){ [ -f /tmp/quiet-run.pgid ] && kill -9 -- -"$(cat /tmp/quiet-run.pgid)" 2>/dev/null; pkill -P "$QR" 2>/dev/null; kill "$QR" 2>/dev/null; wait "$QR" 2>/dev/null; }
  client
  if [ -n "$JOINFAIL" ]; then res note join "first try: $JOINFAIL -- retrying in 60 s"; stop_client; cp /tmp/quiet-run.log "$OUT/client-try1.log" 2>/dev/null; sleep 60; client; fi
  if grep -aq 'a game is already running' "$OUT/quiet-run.txt"; then res SKIP live "a game is running (one client at a time): not started"
  else
    sleep 10   # in the match: let the snapshot, the feed and the server's table catch up
    # a [Server] chat line through the server's RCON (`dedicated rcon say`; the password is in its dedicated.toml): the
    # client's push_history hook must put it in the feed
    TOKEN="port-verify chat check $$"
    PELOAD_STATS_HTTP=0 perl -e 'alarm 40; exec @ARGV' arch -x86_64 shim/build/peload "$EXE" dedicated rcon \
      --config 'C:\ReclaimerTestServer\dedicated.toml' say "$TOKEN" > "$OUT/rcon.txt" 2>&1
    sleep 5
    curl -s -m 3 http://127.0.0.1:8765/stats.json > "$OUT/client-stats.json"; curl -s -m 3 'http://127.0.0.1:8765/feed.json?since=0' > "$OUT/client-feed.json"
    curl -s -m 3 http://127.0.0.1:8767/stats.json > "$OUT/server-stats.json"
    [ -n "$MENU" ] && [ "$MDRAWS" -gt 50 ] && res PASS ui-reentrancy "live: $MENU, $MDRAWS draws a frame" || res FAIL ui-reentrancy "live: ${MENU:-menu never loaded}${MENU:+, only $MDRAWS draws a frame}"
    LC_ALL=C grep -aq 'XAudio2 import slots differ' /tmp/quiet-run.log && res FAIL xaudio "live: the client refused its XAudio2 check" || res PASS xaudio "live: no XAudio2 refusal"
    [ -n "$MATCH" ] && res PASS join "live: $MATCH" || res FAIL join "live: ${JOINFAIL:-no match within ${PV_SECS:-300} s} (see $OUT/client.log)"
    python3 - "$OUT" "$TOKEN" <<'EOF' | tee -a "$OUT/summary.txt"
import json, sys, os
o, token = sys.argv[1], sys.argv[2]
def load(n):
    try: return json.load(open(os.path.join(o, n)))
    except Exception: return None
c, f, s = load('client-stats.json'), load('client-feed.json'), load('server-stats.json')
def out(v, k, t): print(f'  {v:<5} {k:<14} {t}')
sl = (c or {}).get('slots') or {}
if sl.get('src') == 'server': out('PASS', 'max_players', f"live: the server's limit {sl.get('max')} (slots.src server)")
else: out('FAIL', 'max_players', f'live: slots {sl or "(no client snapshot)"}')
ips = [p for p in (s or {}).get('session', []) if p.get('ip')]
if ips: out('PASS', 'moderation', f'live: the server lists {len(ips)} player address(es) (addr_role {s.get("addr_role")})')
else: out('FAIL', 'moderation', f'live: no address in the server\'s session list (addr_role {(s or {}).get("addr_role")})')
ms = [e for e in (f or {}).get('events', []) if e.get('kind') == 'game' and e.get('text', '').startswith('Match started: ')]
if ms and (c or {}).get('map') and ms[-1]['text'][15:] == c['map']: out('PASS', 'feed map', f'live: "{ms[-1]["text"]}" = the snapshot\'s map')
elif ms: out('FAIL', 'feed map', f'live: "{ms[-1]["text"]}" but the snapshot says {(c or {}).get("map")}')
else: out('SKIP', 'feed map', 'live: no "Match started" line in the feed')
ch = [e for e in (f or {}).get('events', []) if e.get('kind') == 'chat']
mine = [e for e in ch if token in e.get('text', '')]
if mine: out('PASS', 'push_history', f'live: the RCON say line reached the feed ("{mine[0]["text"][:60]}")')
elif ch: out('FAIL', 'push_history', f'live: {len(ch)} other chat line(s) in the feed, not the RCON say line (see {o}/rcon.txt)')
else:
    rc = open(os.path.join(o, 'rcon.txt'), errors='replace').read()
    out('FAIL' if 'password' not in rc.lower() else 'SKIP', 'push_history', 'live: the RCON say line never reached the feed (rcon: ' + (rc.strip().splitlines() or ['no output'])[-1][:80] + ')')
EOF
    # anything the client refused or the loader did not apply, and crashes
    LC_ALL=C grep -a -o -E '[A-Za-z0-9 ]+ (import slots differ|mismatch)[^;]*; refused' /tmp/quiet-run.log | sort -u | while read -r l; do
      case "$l" in *XAudio2*) ;; *) res FAIL refused "$l";; esac; done   # (Bink: fixed 2026-10-24, halo3's Bink delay slots stay lazy)
    LC_ALL=C grep -a -m3 '\[CRASH\]' /tmp/quiet-run.log | while read -r l; do res FAIL log "$l"; done
    LC_ALL=C grep -a -m3 'NOT applied' /tmp/quiet-run.log | while read -r l; do res note log "$l (stage 2 judges it)"; done
  fi
  stop_client
  cp /tmp/quiet-run.log "$OUT/client.log" 2>/dev/null
  [ -n "$STARTED" ] && bash shim/dedicated.sh stop > /dev/null
fi
if [ "${PV_DIRECT:-0}" = 1 ]; then
  rm -f /tmp/quiet-run.log
  QR_HIDE=1 QR_STATS=0 QR_EXE="$EXE" bash shim/tools/quiet-run.sh "PELOAD_LOCAL_RUNTIME=__unset__" "${PV_DIRECT_SECS:-90}" > /dev/null 2>&1
  if LC_ALL=C grep -aq 'still running after' /tmp/quiet-run.log; then res FAIL watchdog "live direct match: the engine watchdog fired"
  elif LC_ALL=C grep -aq 'match loaded in' /tmp/quiet-run.log; then res PASS watchdog "live direct match: loaded, no watchdog exit in ${PV_DIRECT_SECS:-90} s"
  else res FAIL watchdog "live direct match: never loaded (see /tmp/quiet-run.log)"; fi
  cp /tmp/quiet-run.log "$OUT/direct.log" 2>/dev/null
fi
fi
P=$(grep -c '^  PASS' "$OUT/summary.txt"); F=$(grep -c '^  FAIL' "$OUT/summary.txt"); S=$(grep -c '^  SKIP' "$OUT/summary.txt")
echo "== $P PASS, $F FAIL, $S SKIP (summary: $OUT/summary.txt)"
[ "$F" -eq 0 ]; exit
}
