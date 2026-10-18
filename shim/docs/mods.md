# Joining big and modded servers (100+ player games, custom maps)

The largest games (64-128 players) are ordinary Reclaimer servers. Many need no mods at all; the ones on custom maps
(Forge worlds, "Huge" maps, CHAOS-style servers) need the server's **mods**: extra map files that the client checks
byte for byte before it lets you in.

## 1. Find the big games

- **In the client:** Server Browser -> click the **PLAYERS** column to sort. A cube icon with a number next to a
  server's name means it needs that many mods.
- **From the terminal** (before starting a game -- it is a short loader run of its own):

  ```bash
  bash shim/tools/servers.sh 25
  ```

  prints the busiest servers with player count, address, map/mode and the mods each needs, e.g.

  ```
  123/127 136.59.9.233:49176     [US-W] Infection - Sandtrap   Sandtrap   Infection
   64/64  75.98.127.229:49400    [IBIGS.us] WL Custom Maps     Savannah Sandbox · Custom   Assault
          mod: Ultimate Forge 2.0 (shared, 7.8 GB; maps: savannah_sandbox, mombasa_sandbox, ...)
  ```

  It wraps the client's own `discover` command, which prints the full JSON (every server's `mods` list: `title`,
  the map `stem`s with their SHA-256 `digest`, `size`, and `shared`).

Join any of them with its address:

```bash
RECLAIMER_JOIN=136.59.9.233:49176 bash shim/play.sh
```

## 2. What a "mod" is

A mod is a Steam Workshop item for Halo: The Master Chief Collection (Steam app **976730**), or the same files served
by the game server itself. What we have seen in them is game data only, no programs:

| File | What |
|---|---|
| `ModInfo.json` | the mod's description (title, version, the maps it adds) |
| `maps/<stem>.map` | the custom map (~0.8-1 GB each) |
| `halo3/maps/shared.map`, `campaign.map` | replacements for the game's shared map resources (Ultimate Forge ships its own) |
| `multiplayer/<stem>.json` | map metadata for the menus |
| `halo3/fmod/pc/*.fsb` | extra sound banks |
| `images/*.png` | loading screens and thumbnails |

The server lists, per mod, the map stems it actually plays and each file's SHA-256 digest; the client only accepts
files that match. A Workshop item can be much bigger than what one server needs (Ultimate Forge 2.0 is 9.6 GB for ten
Forge worlds; a server that only plays `desert_sandbox` needs about 1.7 GB of it).

## 3. Shared mods: nothing to do

If the listing says **shared**, the server hands the files out itself. The client asks before downloading (or does it
automatically with `"auto_download_mods": true` in the profile's `settings.json`) and stores them in
`C:\ReclaimerContent\Mods\Downloaded` inside the loader's prefix
(`~/Library/Application Support/ReclaimerMac/prefix/drive_c/ReclaimerContent/Mods/Downloaded`).

## 4. Workshop-only mods: the client's REQUIRED MODS screen uses DepotDownloader

Since 2026-10-17 the loader runs `shim/tools/workshop-helper.sh` in place of the client's own Workshop helper (which
needs a running Steam client): when a server needs a Workshop mod you don't have, the client's REQUIRED MODS screen
(DOWNLOAD / TRY AGAIN, or straight away with "Download the mods servers share without asking") fetches it with
DepotDownloader into the client's Workshop folder and shows its progress; then the join goes on. It needs:
- DepotDownloader installed and signed in once (steps 1 and 3 below; it never asks for or passes a password -- when
  the saved login expires the screen says so, and you sign in again in a terminal);
- your Steam account name in `~/Library/Application Support/ReclaimerMac/steam_user` (one line), or `STEAM_USER` set.
Log: `$TMPDIR/reclaimer_workshop_<id>.log`. `PELOAD_NO_WORKSHOP_HELPER=1` = the client's own helper (Steam only).
Verified end to end 2026-10-17 (HugeGrass Rebalance fetched from the screen, then the join). After a download the
helper also makes the stats page's top-down pictures of the item's maps in the background (`shim/tools/maptop.sh`;
`PELOAD_NO_MAPTOP=1` skips it). How it works: shim/docs/workshop-helper.md.
The steps below do the same by hand.

### By hand

If the listing says **workshop** (the client shows "Direct sharing is disabled" or asks you to start Steam), the files
only come from Steam Workshop. There is no Steam client in this setup; DepotDownloader fetches Workshop items with your
Steam account instead. Your account needs Halo: The Master Chief Collection.

1. **Install it** (once):

   ```bash
   brew install --cask depotdownloader
   ```

2. **Find the item's Workshop id** (PublishedFileId), either way:
   - Try to join the server once. The client starts its Workshop helper for every missing item, and the loader logs
     that command line:

     ```bash
     grep -ao 'subscribe [0-9]*' /tmp/reclaimer_play.log | sort -u
     ```

   - Or search the mod's title on the MCC Workshop
     (`https://steamcommunity.com/workshop/browse/?appid=976730&searchtext=<title>`); the id is the `id=` number in
     the item's address.

   Known ids: Ultimate Forge 2.0 = `2933266434`, Permafrost - Beta = `3663541185`, Hugegrass = `2984061723`.

3. **Sign in once and list the item's files** (downloads nothing). Run it in a terminal yourself: it asks for your
   Steam password and the Steam Guard code, then remembers the login so later runs only need the account name
   (`-qr` instead signs in by scanning a code with the Steam mobile app). Saved logins expire after a while; when
   DepotDownloader says "Access token was rejected (Expired)", sign in this way again.

   ```bash
   DepotDownloader -app 976730 -pubfile 2933266434 -username "$STEAM_USER" -remember-password -manifest-only -dir /tmp/workshop-list
   ```

   `/tmp/workshop-list/manifest_976730_*.txt` lists every file with its size.

4. **Download what the server needs** into the client's Workshop folder. Make a file list with the server's map
   stems from step 1 (lines starting with `regex:` are regular expressions):

   ```
   ModInfo.json
   maps/desert_sandbox.map
   multiplayer/desert_sandbox.json
   halo3/maps/shared.map
   halo3/maps/campaign.map
   regex:^images/.*desert
   regex:^halo3/fmod/
   ```

   ```bash
   W="$HOME/Library/Application Support/ReclaimerMac/prefix/drive_c/Program Files/Steam/steamapps/workshop/content/976730"
   DepotDownloader -app 976730 -pubfile 2933266434 -username "$STEAM_USER" -remember-password -filelist files.txt -dir "$W/2933266434"
   ```

   Leave out `-filelist` to fetch the whole item (every map it has). The client looks for Workshop content in
   `steamapps\workshop\content\976730\<id>\` (with a `ModInfo.json` in it; it goes by the id in `ModInfo.json`, so
   the folder name doesn't matter); `Program Files (x86)\Steam\steamapps` in the prefix is a link to the same folder.
   For the map pictures on the stats page: `bash shim/tools/maptop.sh "$W"/2933266434/maps/*.map`.

5. **Join** with `RECLAIMER_JOIN=<address> bash shim/play.sh`. If a file is missing or differs from the server's
   copy, the client names it (a verify error: it does not download it); download that file too.

## Notes

- A mod's maps are large: check free disk space (`df -h ~`) before downloading whole items.
- Servers come and go, and change maps between games. `servers.sh` shows what each one is playing right now. A
  server that does not answer (`Public join timed out`) is usually restarting or down.
