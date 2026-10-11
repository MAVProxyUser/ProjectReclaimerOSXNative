#!/bin/bash
# Reclaimer-Mac: a tiny Wine wrapper for Project Reclaimer (https://projectreclaimer.dev)
# Usage: ./reclaimer-mac.sh <command>
#   setup   install Wine + create prefix + install Windows Steam (one time)
#   steam   open Steam in the prefix (sign in, install Halo MCC + Halo 3 Campaign & Multiplayer)
#   play    launch Project Reclaimer
#   reset   delete the prefix (keeps the .exe)
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
export WINEPREFIX="${WINEPREFIX:-$HOME/Library/Application Support/ReclaimerMac/prefix}"
export WINEARCH=win64
export WINEDEBUG=-all
# Wine's macOS OpenGL path; Reclaimer needs GL 3.0+, macOS provides 4.1
export WINEESYNC=1
export MTL_HUD_ENABLED=0

WINE="$(command -v wine64 || command -v wine || true)"
if [ -z "$WINE" ]; then
  for c in "/Applications/Game Porting Toolkit.app/Contents/Resources/wine/bin/wine64" \
           "/Applications/Game Porting Toolkit.app/Contents/Resources/wine/bin/wine"; do
    [ -x "$c" ] && WINE="$c" && break
  done
fi

STEAM_EXE="$WINEPREFIX/drive_c/Program Files/Steam/steam.exe"

cmd_setup() {
  command -v brew >/dev/null || { echo "Install Homebrew first: https://brew.sh"; exit 1; }
  if [ "$(uname -m)" = arm64 ] && ! /usr/bin/pgrep oahd >/dev/null; then
    echo "Installing Rosetta 2 (needed for 64-bit Windows x86 apps)..."
    softwareupdate --install-rosetta --agree-to-license
  fi
  if [ -z "$WINE" ]; then
    brew tap gcenx/wine
    brew install --cask gcenx/wine/game-porting-toolkit
    xattr -dr com.apple.quarantine "/Applications/Game Porting Toolkit.app" 2>/dev/null || true
    exec "$0" setup
  fi
  mkdir -p "$WINEPREFIX"
  echo "Creating Wine prefix..."
  "$WINE" wineboot --init
  local dl="$HOME/Library/Application Support/ReclaimerMac/SteamSetup.exe"
  if [ ! -x "$STEAM_EXE" ] && [ ! -f "$STEAM_EXE" ]; then
    curl -L -o "$dl" "https://cdn.cloudflare.steamstatic.com/client/installer/SteamSetup.exe"
    echo "Installing Steam (click through the installer)..."
    "$WINE" "$dl"
  fi
  echo
  echo "Done. Next: ./reclaimer-mac.sh steam   (install MCC + Halo 3 Campaign & Multiplayer)"
  echo "Then download project-reclaimer-<version>.exe from projectreclaimer.dev, verify its SHA-256,"
  echo "and put it in: $HERE/client/"
}

cmd_steam() {
  # Flags work around Steam's embedded browser crashing under Wine
  "$WINE" "$STEAM_EXE" -no-cef-sandbox -cef-disable-gpu -cef-disable-d3d11 &
  echo "Steam is starting. Sign in yourself, then install Halo: The Master Chief Collection"
  echo "(Properties -> DLC: tick Halo 3 Campaign and Halo 3 Multiplayer)."
}

# Download MCC's Windows files natively with DepotDownloader (no Windows Steam needed).
# Login: set STEAM_USER=<account name> to log in with password (+ emailed Steam Guard code); otherwise a QR code for the Steam mobile app.
# Default depots: 976731 MCC base, 976738 H3 Campaign, 976739 H3 Multiplayer (~23.6 GB). Override with DEPOTS="..."
cmd_download() {
  local tooldir="$HOME/Library/Application Support/ReclaimerMac/depotdownloader"
  local dd="$tooldir/DepotDownloader"
  if [ ! -x "$dd" ]; then
    mkdir -p "$tooldir"
    curl -L -o "$tooldir/dd.zip" "https://github.com/SteamRE/DepotDownloader/releases/download/DepotDownloader_3.4.0/DepotDownloader-macos-arm64.zip"
    unzip -o "$tooldir/dd.zip" -d "$tooldir" && chmod +x "$dd"
    xattr -dr com.apple.quarantine "$tooldir" 2>/dev/null || true
  fi
  local dest="$WINEPREFIX/drive_c/Program Files/Steam/steamapps/common/Halo The Master Chief Collection"
  mkdir -p "$dest"
  local args=(-app 976730 -os windows -dir "$dest" -remember-password)
  if [ -n "${STEAM_USER:-}" ]; then
    args+=(-username "$STEAM_USER")   # password + Steam Guard code are prompted in the terminal
  else
    args+=(-qr)
  fi
  args+=(-depot ${DEPOTS:-976731 976738 976739})   # one -depot flag, space-separated IDs
  "$dd" "${args[@]}"
}

cmd_play() {
  local exe
  exe="$(ls -t "$HERE"/client/project-reclaimer-*.exe 2>/dev/null | head -1 || true)"
  [ -n "$exe" ] || { echo "Put project-reclaimer-<version>.exe in $HERE/client/"; exit 1; }
  # Steam must be running and signed in
  if ! pgrep -fq "steam.exe"; then
    "$WINE" "$STEAM_EXE" -silent -no-cef-sandbox -cef-disable-gpu &
    sleep 15
  fi
  cd "$HERE/client"
  exec "$WINE" "$exe"
}

cmd_reset() { read -rp "Delete $WINEPREFIX? [y/N] " a; [ "$a" = y ] && rm -rf "$WINEPREFIX"; }

case "${1:-}" in
  setup) cmd_setup ;;
  download) cmd_download ;;
  steam) cmd_steam ;;
  play)  cmd_play ;;
  reset) cmd_reset ;;
  *) sed -n '2,8p' "$0" ;;
esac
