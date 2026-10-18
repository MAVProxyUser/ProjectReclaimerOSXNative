# Machine profile, sourced by shim/play.sh: what this Mac is, and the launch decisions that depend on it.
# One line goes to the terminal and the top of /tmp/reclaimer_play.log, e.g.
#   [machine] Apple M1: 4 performance + 4 efficiency cores, 8 GPU cores (base), 16 GB -> particle workers 2, ...
#
# What differs between Macs and what is decided here:
#   - CPU: the game's main thread is the frame-rate limit, so the particle-compute workers (PELOAD_CS_WORKERS) leave it two
#     performance cores: performance cores - 2, at least 2, at most 8 (M1: 2; M4 Max: 8).
#   - GPU: a base-tier GPU (<= 10 cores: the M1, M2, M3 and M4 base chips) draws a big map at a fraction of a Max's speed
#     (menu: M1 78 fps vs M4 Max ~390). A profile that never chose a graphics preset (no "quality" in its settings.json)
#     starts at the client's own Medium preset there -- the Xbox 360's detail; High is the client's default. A preset you
#     choose in Settings > Advanced graphics (or the stats page Quality) is never touched. RECLAIMER_KEEP_QUALITY=1 = never.
#   - Memory: a big match uses ~10 GB (halo3's reserve + caches) plus ~3 GB in the GL helper; at 8 GB a note is printed and
#     the background walking-graph tool is capped at 1 GB (PELOAD_NAV_TOOL_MB; it may take 4 GB otherwise).
#   - The display (Retina 2x vs 1x) is not decided here: the loader reads the window's pixel scale itself, every frame
#     (moving the window to another screen included).
MACH_CHIP="$(sysctl -n machdep.cpu.brand_string 2>/dev/null)"; [ -n "$MACH_CHIP" ] || MACH_CHIP="unknown chip"
MACH_PCORES=$(sysctl -n hw.perflevel0.physicalcpu 2>/dev/null || sysctl -n hw.physicalcpu 2>/dev/null || echo 4)
MACH_ECORES=$(sysctl -n hw.perflevel1.physicalcpu 2>/dev/null || echo 0)
MACH_MEMGB=$(( $(sysctl -n hw.memsize 2>/dev/null || echo 0) / 1073741824 ))
MACH_GPU=$(ioreg -rc IOAccelerator -d1 2>/dev/null | sed -n 's/.*"gpu-core-count" = \([0-9]*\).*/\1/p' | head -1)
if [ -n "$MACH_GPU" ]; then
  if [ "$MACH_GPU" -le 10 ]; then MACH_TIER=base; elif [ "$MACH_GPU" -le 20 ]; then MACH_TIER=pro; else MACH_TIER=max; fi
else MACH_GPU="?"; [ "$MACH_PCORES" -le 4 ] && MACH_TIER=base || MACH_TIER=pro; fi

CSW=$((MACH_PCORES-2)); [ "$CSW" -lt 2 ] && CSW=2; [ "$CSW" -gt 8 ] && CSW=8
MACH_DO="particle workers ${PELOAD_CS_WORKERS:-$CSW}"

# first-run graphics preset on a base-tier GPU (only a profile with no "quality" block; only with a working python3 --
# /usr/bin/python3 without the Command Line Tools would open an install dialog instead)
mach_profile_dir(){ local p="${RECLAIMER_PROFILE_DIR:-C:\\ReclaimerProfile}"; p="${p#C:\\}"; p="${p#c:\\}"; echo "$PFX/${p//\\//}"; }
if [ "$MACH_TIER" = base ] && [ -z "${RECLAIMER_KEEP_QUALITY:-}" ]; then
  SET="$(mach_profile_dir)/settings.json"; PY=""
  if command -v python3 >/dev/null 2>&1; then
    if [ "$(command -v python3)" != /usr/bin/python3 ] || xcode-select -p >/dev/null 2>&1; then PY=python3; fi; fi
  if [ -f "$SET" ] && [ -n "$PY" ]; then
    R=$("$PY" - "$SET" <<'EOF' 2>/dev/null
import json, sys, shutil
p = sys.argv[1]
try:
    d = json.load(open(p))
except Exception:
    print('unreadable'); sys.exit()
if not isinstance(d, dict) or 'quality' in d:
    print('kept'); sys.exit()
shutil.copyfile(p, p + '.before-medium')
# the client's own Medium preset (Settings > Advanced graphics > Medium, as it writes it)
d['quality'] = {'one': 'medium', 'detail': {'decals': 'high', 'draw_distance': 'medium', 'effects': 'medium',
                'level_of_detail': 'medium', 'lighting': 'medium', 'shadows': 'medium', 'water': 'medium'}}
json.dump(d, open(p, 'w'), indent=2)
print('medium')
EOF
)
    case "$R" in
      medium) MACH_DO="$MACH_DO, graphics preset Medium (first run on a base-tier GPU; change it any time in Settings > Advanced graphics)";;
      kept) MACH_DO="$MACH_DO, graphics preset as chosen";;
    esac
  fi
fi
# 8 GB: the background walking-graph tool (re-tools/h3nav.py, made once per new map) may take up to 4 GB -- capped at 1 GB here
if [ "$MACH_MEMGB" -gt 0 ] && [ "$MACH_MEMGB" -le 8 ]; then export PELOAD_NAV_TOOL_MB="${PELOAD_NAV_TOOL_MB:-1024}"
  MACH_DO="$MACH_DO, map-tool memory cap 1 GB; note: 8 GB is tight for big matches (~13 GB in use) -- close other apps, prefer smaller games"; fi
MACH_LINE="[machine] $MACH_CHIP: $MACH_PCORES performance + $MACH_ECORES efficiency cores, $MACH_GPU GPU cores ($MACH_TIER), ${MACH_MEMGB} GB -> $MACH_DO"
