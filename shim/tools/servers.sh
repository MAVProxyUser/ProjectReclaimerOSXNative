#!/bin/bash
# List the busiest Project Reclaimer servers: players, address, name, map/mode and the mods each one needs.
#     bash shim/tools/servers.sh [count]          # default 25
# Join one with:  RECLAIMER_JOIN=<address> bash shim/play.sh
# Uses the client's own `discover` command (asks the master servers, then queries every server; ~20 s). It is a
# short-lived loader process: run it before starting a game, not beside one.
# Mods: "shared" = the server hands them out itself (the client downloads them when you join);
#       "workshop" = Steam Workshop only -- see shim/docs/mods.md for downloading those with DepotDownloader.
cd "$(dirname "$0")/../.."
N="${1:-25}"
EXE="${RECLAIMER_EXE:-}"
if [ -z "$EXE" ]; then for c in client/project-reclaimer-client-v0.9.18.exe; do [ -f "$c" ] && EXE="$c" && break; done; fi
[ -n "$EXE" ] && [ -f "$EXE" ] || { echo "Project Reclaimer client exe not found (see README)"; exit 1; }
[ -x shim/build/peload ] || { echo "shim/build/peload missing: run 'bash shim/build.sh'"; exit 1; }
OUT="$(mktemp -t reclaimer_servers)"
arch -x86_64 shim/build/peload "$EXE" discover < /dev/null 2>/dev/null | sed 's/\x1b\[[0-9;]*m//g' > "$OUT"
python3 - "$OUT" "$N" <<'EOF'
import json, sys
try: servers = json.load(open(sys.argv[1], errors='replace'))
except Exception as e: sys.exit(f"could not read the server list ({e})")
live = [s for s in servers if 'players' in s]
live.sort(key=lambda s: -s.get('players', 0))
print(f"{len(live)} servers answered ({len(servers) - len(live)} did not)\n")
for s in live[:int(sys.argv[2])]:
    mods = s.get('mods') or []
    print(f"{s.get('players',0):>3}/{s.get('capacity','?'):<3} {s['address']:<22} {s.get('name','')[:44]:<44} {s.get('map','')[:22]:<22} {s.get('mode','')}"
          + ("  [password]" if s.get('password_required') else ""))
    for m in mods:
        stems = ", ".join(x.get('stem', '') for x in m.get('maps', []))
        print(f"{'':8}mod: {m.get('title','?')} ({'shared' if m.get('shared') else 'workshop'}, {m.get('size',0)/1e9:.1f} GB; maps: {stems})")
EOF
rm -f "$OUT"
