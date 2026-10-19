#!/bin/bash
# Build Reclaimer.app: a double-clickable launcher for shim/play.sh (no Terminal window). shim/build.sh runs this after every
# build, install.sh with --applications, make-release.sh with --portable (the app in the release bundle).
#
#   bash shim/tools/make-app.sh                  # -> <this folder>/Reclaimer.app
#   bash shim/tools/make-app.sh --applications   # ... and a copy in ~/Applications (Launchpad / Spotlight find it there)
#   bash shim/tools/make-app.sh --portable       # no folder path written in: the app uses the folder it sits in
#   RECLAIMER_SIGN_IDENTITY="<name>" bash shim/tools/make-app.sh   # sign with one of your identities instead of ad hoc
#                                                # (security find-identity -v -p codesigning lists them)
#
# The app's program is a tiny signed native launcher (shim/src/launcher.c, built by shim/build.sh) that runs the bundle's
# Contents/Resources/launch.sh. (A script as the bundle's program
# was silently refused by macOS privacy protection -- "/bin/bash: shim/play.sh: Operation not permitted" -- when the folder
# is on the Desktop / in Documents / Downloads; a signed Mach-O app gets macOS's one-time "would like to access files in your
# Desktop folder" prompt instead, and the game inherits the permission.) launch.sh uses the folder the app sits in when it
# holds shim/play.sh (a release bundle, a checkout), else the folder written in when the app was made (the ~/Applications
# copy), and starts
# shim/play.sh (the menu; RECLAIMER_JOIN etc. still work from a Terminal launch) and writes its output to
# /tmp/reclaimer_launcher.log (the game's own log stays /tmp/reclaimer_play.log). It
#   - adds Homebrew's folders to PATH (an app started from Finder has only /usr/bin:/bin:..., so DepotDownloader -- Workshop
#     mods -- would not be found),
#   - does nothing but a notification while the game already runs (play.sh would close the running game's leftovers),
#   - says so in a dialog when Rosetta is missing or this folder has moved (run this script again in the new folder),
#   - gives the game's Dock icon the app's icon (PELOAD_APP_ICON; the loader itself is a bare executable).
# It has no Dock icon of its own (LSUIElement): the game window's icon is the one you see. Signed ad hoc (codesign -s -) unless
# RECLAIMER_SIGN_IDENTITY names one of yours; the signature is verified. An app made on this Mac is not quarantined; a
# downloaded release needs the README's `xattr -dr com.apple.quarantine <folder>` (install.sh clears it on what it copies).
set -e
cd "$(dirname "$0")/../.."
ROOT="$PWD"; APPS=0; PORTABLE=0
for a in "$@"; do case "$a" in --applications) APPS=1;; --portable) PORTABLE=1;; *) echo "unknown option $a"; exit 1;; esac; done
[ -f shim/play.sh ] || { echo "run this from a Reclaimer checkout / install folder (shim/play.sh not found)"; exit 1; }
APP="$ROOT/Reclaimer.app"
rm -rf "$APP"; mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
VER="${RECLAIMER_APP_VERSION:-$(git -C "$ROOT" describe --always --dirty 2>/dev/null || date +%Y.%m.%d)}"
cat > "$APP/Contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>CFBundleName</key><string>Reclaimer</string>
  <key>CFBundleDisplayName</key><string>Reclaimer</string>
  <key>CFBundleIdentifier</key><string>local.reclaimermac.launcher</string>
  <key>CFBundleExecutable</key><string>Reclaimer</string>
  <key>CFBundleIconFile</key><string>Reclaimer</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>$VER</string>
  <key>CFBundleVersion</key><string>$VER</string>
  <key>LSMinimumSystemVersion</key><string>13.0</string>
  <key>LSUIElement</key><true/>
  <key>NSHighResolutionCapable</key><true/>
  <key>NSDesktopFolderUsageDescription</key><string>Reclaimer runs the game from its install folder.</string>
  <key>NSDocumentsFolderUsageDescription</key><string>Reclaimer runs the game from its install folder.</string>
  <key>NSDownloadsFolderUsageDescription</key><string>Reclaimer runs the game from its install folder.</string>
  <key>NSRemovableVolumesUsageDescription</key><string>Reclaimer can run the game from a USB stick.</string>
</dict></plist>
EOF
# the folder path, quoted for a single-quoted shell string (none in a portable app)
QROOT=$(printf "%s" "$ROOT" | sed "s/'/'\\\\''/g"); [ $PORTABLE = 1 ] && QROOT=""
L="$APP/Contents/Resources/launch.sh"
cat > "$L" <<EOF
#!/bin/bash
# Reclaimer.app (made by shim/tools/make-app.sh): starts shim/play.sh. Output: /tmp/reclaimer_launcher.log
ROOT='$QROOT'   # the folder this app was made for (empty: portable)
EOF
cat >> "$L" <<'EOF'
HERE="$(cd "$(dirname "$0")/.." && pwd)"   # .../Reclaimer.app/Contents
NEXT="$(cd "$HERE/../.." && pwd)"           # the folder the app sits in
[ -f "$NEXT/shim/play.sh" ] && ROOT="$NEXT"
alert(){ /usr/bin/osascript -e "display alert \"Reclaimer\" message \"$1\"" >/dev/null 2>&1; }
note(){ /usr/bin/osascript -e "display notification \"$1\" with title \"Reclaimer\"" >/dev/null 2>&1; }
[ -n "$ROOT" ] && [ -f "$ROOT/shim/play.sh" ] || { alert "Reclaimer's folder was not found${ROOT:+ at $ROOT}. Keep this app in the Reclaimer folder, or run shim/tools/make-app.sh there again."; exit 1; }
/usr/bin/arch -x86_64 /usr/bin/true 2>/dev/null || { alert "Rosetta 2 is needed. In Terminal: softwareupdate --install-rosetta --agree-to-license"; exit 1; }
# the game already running? (anchored: a shell whose command text mentions the loader must not count; our local dedicated
# server's processes are not "the game")
for p in $(/usr/bin/pgrep -f '^[^ ]*build/peload'); do
  c="$(/bin/ps -o command= -p "$p" 2>/dev/null)"
  case "$c" in *" dedicated "*|*" local-server "*|*"probe-engine-worker C:\\Rec"*) continue;; esac
  note "Reclaimer is already running."; exit 0
done
export PATH="/opt/homebrew/bin:/usr/local/bin:$PATH"
export PELOAD_APP_ICON="$HERE/Resources/Reclaimer.icns"
cd "$ROOT" && exec /bin/bash shim/play.sh > /tmp/reclaimer_launcher.log 2>&1 < /dev/null
EOF
chmod +x "$L"
if [ -x shim/build/reclaimer-launcher ]; then cp shim/build/reclaimer-launcher "$APP/Contents/MacOS/Reclaimer"
else   # (no launcher built: the script itself as the program -- works unless the folder is privacy-protected)
  printf '#!/bin/bash\nexec /bin/bash "$(dirname "$0")/../Resources/launch.sh"\n' > "$APP/Contents/MacOS/Reclaimer"; fi
chmod +x "$APP/Contents/MacOS/Reclaimer"
[ -f shim/assets/Reclaimer.icns ] && cp shim/assets/Reclaimer.icns "$APP/Contents/Resources/"
SIGN="${RECLAIMER_SIGN_IDENTITY:--}"
if ! /usr/bin/codesign --force --sign "$SIGN" "$APP" >/dev/null 2>&1; then
  [ "$SIGN" != - ] && echo "note: could not sign with \"$SIGN\": signing ad hoc instead"; SIGN=-
  /usr/bin/codesign --force --sign - "$APP" >/dev/null 2>&1 || echo "note: could not sign the app (codesign); it may be refused access to a protected folder"; fi
if /usr/bin/codesign --verify --strict "$APP" 2>/dev/null; then SIGNED="signed $( [ "$SIGN" = - ] && echo 'ad hoc' || echo "by $SIGN")"; else SIGNED="NOT signed"; fi
/usr/bin/touch "$APP"   # Finder / LaunchServices pick up the icon
echo "made $APP ($SIGNED$( [ $PORTABLE = 1 ] && echo ', portable'))"
if [ $APPS = 1 ]; then mkdir -p "$HOME/Applications"; rm -rf "$HOME/Applications/Reclaimer.app"; /usr/bin/ditto "$APP" "$HOME/Applications/Reclaimer.app"
  echo "copied to ~/Applications/Reclaimer.app"; fi
