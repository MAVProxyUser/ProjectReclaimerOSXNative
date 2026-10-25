#!/bin/bash
# What a new Project Reclaimer client build means for the loader, without playing it (step 1 of every port; the rest is in
# shim/docs/patch-addresses.md section 6):
#   bash shim/tools/client-survey.sh                      # the newest client/ exe against the one before it
#   bash shim/tools/client-survey.sh 0.9.22 0.9.23        # two given versions (client/project-reclaimer-client-v<ver>.exe)
# 1. runs the loader on each exe with --version (the client prints its version and exits; no window, no game, nothing
#    written) and shows every [sigpatch] / [stats] line: which signature patches and hooks were found and where;
# 2. diffs the two import tables (re-tools/peimps.py): a new import the loader lacks becomes a logging stub returning 0;
# 3. checks the client still has the `dedicated` subcommand (shim/dedicated.sh uses it).
# Nothing is started besides those --version / --help runs; it refuses while a game runs (one client at a time).
cd "$(dirname "$0")/../.."
if [ -n "$(pgrep -f '^[^ ]*build/peload')" ]; then echo "a game is running - not starting"; exit 3; fi
if [ $# -ge 2 ]; then OLD="client/project-reclaimer-client-v$1.exe"; NEW="client/project-reclaimer-client-v$2.exe"
else set -- $(ls client/project-reclaimer-client-v*.exe 2>/dev/null | sort -V | tail -2); OLD="$1"; NEW="$2"; fi
[ -f "$OLD" ] && [ -f "$NEW" ] || { echo "need two client exes (got: $OLD $NEW)"; exit 1; }
[ -x shim/build/peload ] || { echo "build the loader first: bash shim/build.sh"; exit 1; }
T=$(mktemp -d "${TMPDIR:-/tmp}/survey.XXXXXX"); trap 'rm -rf "$T"' EXIT
for E in "$OLD" "$NEW"; do
  echo "== $E ($(shasum -a 256 "$E" | cut -c1-16)...)"
  # PELOAD_STATS_HTTP on (the hooks install only with the stats page) on a port nothing uses; it binds only once halo3 loads
  PELOAD_RECLAIMER_PATCHES=all PELOAD_STATS_HTTP=8799 PELOAD_STATS_OPEN=0 perl -e 'alarm 30; exec @ARGV' \
    arch -x86_64 shim/build/peload "$E" --version > "$T/v.log" 2>&1
  grep -a -o -E '\[(sigpatch|stats)\][^{]{0,200}' "$T/v.log" | sed 's/^/  /'
  grep -a -m1 '^project-reclaimer ' "$T/v.log" | sed 's/^/  /'
done
echo "== imports: < only in $(basename "$OLD"), > only in $(basename "$NEW")"
python3 re-tools/peimps.py "$OLD" | sort > "$T/a"; python3 re-tools/peimps.py "$NEW" | sort > "$T/b"
diff "$T/a" "$T/b" | grep '^[<>]' | sed 's/^/  /' > "$T/d"; [ -s "$T/d" ] && cat "$T/d" || echo "  (none)"
PELOAD_STATS_HTTP=0 perl -e 'alarm 30; exec @ARGV' arch -x86_64 shim/build/peload "$NEW" --help > "$T/h.log" 2>&1
grep -a -q -E '^\S*\s*\S*dedicated' "$T/h.log" && echo "== dedicated subcommand: present" || echo "== dedicated subcommand: MISSING (shim/dedicated.sh needs another exe)"
