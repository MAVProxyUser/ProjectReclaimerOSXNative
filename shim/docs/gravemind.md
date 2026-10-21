# Project Gravemind (Halo 2) on the loader

Project Gravemind (`h2gravemind.exe`, releases at github.com/BriRoz1/h2gravemind-releases) runs Halo 2 from MCC's `halo2.dll`
outside MCC, the way Project Reclaimer runs Halo 3. It is a beta: it checks a status file on GitHub at every start and won't
start when the beta is closed.

## Status (2026-10-21, branch h2-gravemind)

- **An offline match runs and renders**: `bash shim/gravemind.sh --map-id 44` (Lockout) -- the map, the first-person weapon, the
  HUD and motion tracker, ~60 fps. Gravemind's own patches to halo2.dll all apply; Miles sound plays through waveOut.
- **Launcher and menus**: they are Direct2D / DirectWrite / WIC, which the loader now has (shim/src/d2d.c, dwrite.c, wic.c; checked
  offline by `bash shim/tools/d2d-test.sh`). Not yet seen running Gravemind's own launcher: its first run will show which
  Direct2D calls it makes that aren't implemented (`[d2d] ... not implemented` in the log). Its back buffer is now its swap chain's
  1280x720 (it was 1920x1080 with the game in a corner).
- **Input**: keyboard, mouse and pads work once the window is active (2026-10-21: halo2.dll's hidden window had kept the
  foreground; FIXES.md section 7). F1 releases the mouse, a click in the window captures it again.
- **Sound**: works since 2026-10-21 (FindFirstFileA could not find Miles' DSP plugin, so halo2 never started a sound; FIXES.md
  section 4). Miles plays through the loader's DirectSound (dsound.c) like on Windows -- on waveOut it ran ~0.2 s behind the picture.
  `PELOAD_TRACE_DSOUND=1` logs the stream (peak > 0 = sound); `PELOAD_DSOUND=0` = waveOut.
- **Loading screen**: the screen is black while a map loads; halo2 asks for `loading_dx11.bin`, which isn't in MCC's files (MCC
  draws its own loading screens). Gravemind's own loading screen is Direct2D (to be checked with the launcher).
- Not yet: the LAN game list (GetAdaptersAddresses), campaign (needs the 31 GB campaign depot).
- GRAVEMIND LIVE (its online service) claims the player's name as a gamertag when it starts: choose your own name.

## Setup

1. MCC's Halo 2 multiplayer files with DepotDownloader (your Steam login), depot **976736**, without the campaign's Anniversary
   graphics and terminal videos (~4.5 GB instead of 21 GB):
   ```bash
   printf '%s\n' 'regex:^halo2[\\/](?!preload[\\/]|terminals[\\/]).*$' > /tmp/h2-files.txt
   "$HOME/Library/Application Support/ReclaimerMac/depotdownloader/DepotDownloader" -app 976730 -depot 976736 -os windows \
     -username <you> -remember-password -filelist /tmp/h2-files.txt \
     -dir "$HOME/Library/Application Support/ReclaimerMac/prefix/drive_c/Program Files/Steam/steamapps/common/Halo The Master Chief Collection"
   ```
   (976735 = the campaign, 31 GB; 1064229 = Chinese / Japanese / Korean voices.)
2. Gravemind's zip (`h2gravemind*.zip`) in `client/`; `shim/gravemind.sh` installs it into `C:\Gravemind` (and again when a newer zip appears).

## Running

```bash
bash shim/gravemind.sh --map-id 44              # offline Lockout (map ids: LEEME.txt "MAPAS", 44..68)
bash shim/gravemind.sh --host --map-id 44       # host (UDP 11774)
bash shim/gravemind.sh --connect <ip>:11774     # join
```

Settings and saves: `C:\ReclaimerProfile\Documents\My Games\Project Gravemind` (`h2gravemind.ini`: `[launcher] name=` is your
name). Logs: `/tmp/gravemind_play.log` (loader) and `C:\ReclaimerCache\Project Gravemind\logs\h2gravemind.log` (Gravemind).
`GM_TEST=1` = an input-less test run with its own profile (`C:\ReclaimerProfileGMTest`).
