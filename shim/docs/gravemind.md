# Project Gravemind (Halo 2) on the loader

Project Gravemind (`h2gravemind.exe`, releases at github.com/BriRoz1/h2gravemind-releases) runs Halo 2 from MCC's `halo2.dll`
outside MCC, the way Project Reclaimer runs Halo 3. It is a beta: it checks a status file on GitHub at every start and won't
start when the beta is closed.

## Status (2026-10-21, branch h2-gravemind)

- **An offline match runs and renders**: `bash shim/gravemind.sh --map-id 44` (Lockout) -- the map, the first-person weapon, the
  HUD and motion tracker, ~60 fps. Gravemind's own patches to halo2.dll all apply; Miles sound plays through waveOut.
- **The launcher and menus don't draw yet**: they use Direct2D / DirectWrite, which the loader doesn't have yet (in progress).
- **Controller**: Gravemind reads pads with XInputGetState; whether it polls them under the loader is being checked
  (`[xinput] XInputGetState called` in the log when it does).
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
