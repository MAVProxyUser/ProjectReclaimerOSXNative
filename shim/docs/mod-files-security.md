# Mod files: every option, every parser, and what can go wrong

Static review of the Reclaimer client v0.9.20 (`client/project-reclaimer-client-v0.9.20.exe`, ImageBase 0x140000000),
MCC's `halo3.dll` (ImageBase 0x180000000) and this loader, 2026-10-19. Addresses are `exe+RVA` / `halo3+RVA`.
CONFIRMED = read in the disassembly / code; INFERRED = from names, files on disk or UI text. Nothing was run against
other people; no exploit files were built. Fixes in the loader: FIXES.md sections 4, 15, 18, 20 (10-19).

## 1. Summary

- **JSON** (ModInfo, map JSON, Forge layouts, packages, server messages) is parsed by serde_json in Rust with 0.5-1 MiB
  read caps and per-field validation: not a memory-corruption surface.
- **Images**: the client decodes only PNG, JPEG and BMP, with pure-Rust crates (`image` 0.25.10, `png` 0.18.1,
  `zune-jpeg` 0.5.15). No PSD decoder exists anywhere (no psd crate, no stb_image in any binary); `.psd` files in mods
  are never opened. halo3.dll has no PNG/JPEG decoder (its textures are BC blocks inside the .map). The only
  memory-corruption candidate is zune-jpeg's hand-written AVX2 code (`unsafe`); the realistic problem is memory: mod
  pictures have no dimension cap (up to 512 MiB per decode).
- **XML**: no XXE anywhere. The client links xml-rs only (UPnP router discovery, LAN), which never fetches external
  entities; halo3.dll has no XML parser. The one OLD XML library -- James Clark Expat 1.0.x (1998) inside MCC's Waves
  audio plug-in -- is NOT reachable from mod content (section 6).
- **The native attack surface is the map and its side files**, parsed by 2008-era C++ in halo3.dll and by FMOD 1.10.x:
  `.map` (tags, counts, Havok 4.5 MOPP bytecode; resource pages go through halo3's own bounded DEFLATE decoder, not
  zlib), `.fsb` sound banks (FMOD sniffs the codec from content: tracker / MIDI / FLAC / Ogg / MP3 decoders reachable),
  BLF `.halo3_mapinfo` / `.mvar` (BLF `_cmp` chunks inflate through zlib 1.1.4, output bounded), and `.bik` (Bink 2.7n;
  refused under this loader).
- **Under this loader** exploitation would be easier than on Windows (halo3.dll at a fixed base, some patched pages and the
  hook pools RWX), and the loader itself parsed several untrusted inputs without checks -- now fixed (section 7).

## 2. Where mod files come from, and what may come

| Source | How | Checks |
|---|---|---|
| Steam Workshop | the client's `workshop-download --subscribe/--update <id>` = `shim/tools/workshop-helper.sh` (DepotDownloader) into `steamapps/workshop/content/976730/<id>` | id digits only; since 10-19 a size cap (RECLAIMER_WORKSHOP_MAX_GB, 32) and a free-space check; the client then checks each file's size + SHA-256 against the server's manifest |
| A game server (`share_mods = true`) | chunked transfer, `C:\ReclaimerContent\Mods\Downloaded\...` | CONFIRMED exe+0x1886be0 / 0x1888030: at most 100 files; paths 1..200 bytes, `/`-separated, <= 4 parts of 1..96 chars from `[A-Za-z0-9 ()-._]`, no `.`/`..`, no trailing `.`/space, no CON/PRN/AUX/NUL/COMx/LPTx; SHA-256 checked BEFORE the temp file is renamed into place (exe+0x74d309) |
| Customization mods | pushed on join when the host requires them | "no way to turn them down" (client UI) |
| Our dedicated server | its own copies, `ReclaimerTestServer/content/Mods/Workshop/<id>` (updates itself) | install filter exe+0xa2c4b0: .map .mapinfo .json .png .jpg .jpeg .fsb .fsb.info .bik .bin .mvar only, <= 4 GiB, <= 4096 files, links refused |

File types a server may hand out, with caps (exe+0x1888d70): `.map` < 4 GiB, `.fsb` / `.bik` <= 2 GiB, `.fsb.info` <= 16 MiB,
`.png` / `.jpg` / `.jpeg` <= 8 MiB, `.json` <= 1 MiB, `.mapinfo` <= 64 KiB. Nothing executable. Integrity = "exactly what the
server meant": a malicious server still chooses every byte.

## 3. Every option in the mod JSON files

### 3.1 ModInfo.json (MCC's manifest; 1 MiB read cap)

The typed parser (exe+0x18913c0) reads two keys; everything else is skipped.

| Field | Type | What the client does |
|---|---|---|
| `Engine` | string, required | must be "halo3" (case-insensitive) or the mod is skipped |
| `GameModContents` | object or null | null = no maps |
| `GameModContents.MultiplayerMaps` | array of strings | relative paths of the map JSONs; trimmed, rejected with `..` or a root; first 64 used |
| `Title.Neutral` | string (untyped reader exe+0x18ba660) | the mod's name in the UI ("HugeGrass Rebalance") |
| `ModIdentifier.HostedModIds.SteamWorkshopId` | number (exe+0x1ba88c0) | the Workshop item id when `reclaimer-workshop.json` is missing; the dedicated installer refuses a mismatch |
| `ModIdentifier.ModGuid` | string | read by the customization-mod code (identity: INFERRED) |

NOT read (the key strings are absent from the client): `ModVersion`, `MinAppVersion`, `MaxAppVersion`, `InheritSharedFiles`,
`ModContents.HasBackgroundVideos` / `HasNameplates`, `GameModContents.HasSharedFiles` / `HasCampaign` / `HasSpartanOps` /
`HasController`, `Description`. "Another version" comes from file digests, not `ModVersion`. Campaign mods are found by
`CampaignInfo.json` (`CampaignGuid`, `CampaignMaps`: strings or objects, same `..`/root rule).

### 3.2 multiplayer/<map>.json (MapJson; 6 fields, all required; 1 MiB cap; parser exe+0x18975e0)

| Field | Type | What it does |
|---|---|---|
| `MapGuid` | string | parsed as a GUID (32 hex, optional braces / hyphens) |
| `ScenarioFile` | string | the map's file stem, matched case-insensitively against the mod's .map files (a name, not a path) |
| `Title` | object (`Neutral`) or null | the map's name, <= 255 chars |
| `Description` | any | kept (use INFERRED) |
| `Images` | object `{Thumbnail, Large, LoadingScreen}`, each string or null | mod pictures (section 4.1); `TopDown` is ignored |
| `InsertionPoints` | array | kept as values |

Ignored MCC keys: `Flags`, `MaximumTeamsByGameCategory`, `MapDefaultPrimaryWeapon`, `MapDefaultPrimaryWeaponForge`.

### 3.3 Custom games / Forge layouts (struct Preset, version 1; 512 KiB cap; validator exe+0x18cbde0)

The client reads these from its Maps / Game Modes folders; a Workshop item's "Project Reclaimer Layouts" folder is NOT read
(its README says to copy them there by hand).

| Field | Type | What it does |
|---|---|---|
| `version` | int | must be 1 |
| `name` | string | 1-48 chars, no control chars |
| `base_map` | object | `id` (map id), `stem`, `title`, `cache` (the author's absolute path; a hint, maps resolve by id/stem), `metadata`, `expanded` (Expanded Forge), `canvas` {`mission`, `source`, `zone_set`}, `guid` |
| `hosting` | object | section 3.4 |
| `mode` | enum | CaptureTheFlag, Slayer, Forge, Oddball, KingOfTheHill, Vip, Juggernaut, Territories, Assault, Infection, EscalationSlayer |
| `rules` | object | `score_to_win` (-1..500), `kill_points`, `death_points`, `suicide_points`, `betrayal_points` (-10..10) |
| `script` | ? | a scripted mode (needs an unpacked multiplayer game variant) |
| `options` | map option-id -> value | game options, e.g. `"player.primary_weapon": "Battle rifle"` (unknown ids refused) |
| `world` | object | `player_scale_percent` 25-300, `exposure` 0.1-4.0, `atmospheric_fog`, `sprint` off/unlimited/stamina, `sprint_speed_percent`, `sprint_stamina_seconds`, `volumes`; total size capped ("exceed the network limit") |
| `map` | object | the map variant, section 3.5 |
| `packed_map`, `packed_game` | base64 (INFERRED) | embedded MCC variants; must match the base map / mode |
| `author` | string | validated |
| `campaign` | object | `campaign`, `mission`, `insertion`, `difficulty` (easy..legendary), `custom_characters`, `modifiers` (3.6), `customization`, `no_vanilla_characters`; co-op <= 16 |
| `odst` | object | `mode`, `mission`, `insertion`, `difficulty`, `variant`, `modifiers` |

### 3.4 Hosting options (30 fields)

`name`, `host_name`, `visibility` (Local / Public), `hosting` (Dedicated / PlayAndHost), `public_address`,
`automatic_port_mapping`, `game_port`, `discovery_port`, `voice_enabled`, `voice_global`, `voice_team`, `voice_proximity`,
`voice_team_only`, `voice_proximity_only`, `friendly_fire`, `text_chat_enabled`, `hidden`, `password` (<= 128 bytes, no
control chars), `max_players` (1-127), `share_mods` (hand mod files out), `share_rate` (Mbit/s, 0 = unlimited),
`team_changing` (game_type / balanced / off), `anti_cheat` (off / detect / enforce), `max_ping`, `mission_joins`,
`load_timeout`, `customization`, `require_customization`, `no_vanilla_characters`, `camera` (first_person / player_choice /
third_person).

### 3.5 Map variant (13 fields; visit_map exe+0x18f23c0)

| Field | Type | What it does |
|---|---|---|
| `name` | string | 1-48 chars |
| `engine_build` | hex | must equal the supported halo3 build hash |
| `data` | base64 | the native Halo 3 map-variant blob (size checked) -- handed to halo3.dll |
| `objects` | array `{code, donor, name}` | objects borrowed from other maps (donor stem + tag path) |
| `boundaries` | `{walls, guardians}` | invisible walls / soft ceilings on or off; Guardians (Sandtrap / Sandbox out-of-bounds killers) on or off |
| `author` | string | validated |
| `frozen` | base64 "FRZ1" | frozen Forge objects (create / delete / lost / pick up / refused timeline) |
| `reuses_map_slots` | bool | placements reuse deleted base-map object slots |
| `more_teams` | bool | INFERRED |
| `sky` | `{donor, name}` | another map's sky |
| `gravity` | int % | 10-300 |
| `atmosphere` | `{look, exposure, fog, sky_height}` | screen look + strength, exposure, fog, sky height (game options win) |
| `scripting` | array `{label, spawn_sequence}` | spawn scripting labels |

### 3.6 Modifiers (campaign) and other structs

Modifiers: `health_percent`, `damage_percent` (25-1000 in 25s), `health_per_player_percent`, `damage_per_player_percent`
(0-100 in 5s), `disable_friendly_fire`, `third_person`, `sprint`, `sprint_speed_percent` (110-200), `sprint_stamina_seconds`
(<= 30), `skulls`.

### 3.7 Files the client writes next to mods

| File | Fields |
|---|---|
| `reclaimer-workshop.json` | `item`, `folder` |
| `reclaimer-workshop-version.json` | `item`, `manifest`, `updated` (from Steam GetPublishedFileDetails) |
| `reclaimer-installed.json` | `item`, `files` [[path, size]] (install receipt) |
| `reclaimer-package.json` | `id`, `manifest` {`title`, `files` [{`path`, `size`, `sha256`, `chunks`}]}, `servers`, `installed` |
| `mod-digests.json` | cached SHA-256 per absolute path (`size`, `modified`, `sha256`) |
| `disabled-mods.json` | `packages` (<= 1024) |

### 3.8 What a server sends about mods

Requirement (per mod): `id` (64 hex), `title` (<= 100), `maps` [{`stem` 1-63 `[a-z0-9_-]`, `digest` 64 hex}], `size`, `shared`,
`objects`, `campaign`, `customization`, `lineage`, `required`, `key`. At most 16 mods / 128 maps. Content requests:
`manifest`, `manifest_sources`, `file{sha256, offset}`, `chunks{sha256}`, `range{sha256, offset, length}`. Map offers:
`id`, `stem`, `digest`, `expanded`, `manifest`, `campaign`, `canvas`. Vote cards carry `preview_image` = "vote-image:<sha256>"
(section 4.2).

### 3.9 Server config parts that control mods (TOML / playlists)

`content` (folder with Maps, Game Modes, Mods, Images); `[workshop]` `items` (<= 64), `provisioner`, `steamcmd`, `username`,
`update_check_minutes`; per server `share_mods`, `share_rate`, `base_map` ("workshop:<id>/<stem>"), `customization`,
`require_customization`, `no_vanilla_characters`. Playlists (`*.playlist.json`, version 2): `name`, `vote_seconds` 5-120,
`games` (3-128) of {`map`, `base_map`, `game`, `weight`, `name`, `rules`, `options`, `world`, `anti_cheat`,
`voice_team_only`, `preview_image`}.

### 3.10 Customization mods (`halo3customization.json`)

`version`, `title` (<= 64), `mod_version`, `thumbnail` (png/jpg in its folder), `shader_prefix` (`[a-z0-9_]` <= 32),
`characters` [{`name`, `title`, `model`, `map`, `biped`, `first_person_hands`, `first_person_body`, `variant`, `armor`
(regions: `region`, `permutations`, `default`, `recolors`, `title`, `bone`, `titles`, `icons`, `with`), `colors` (`channel`,
`name`, `title`, `default`, `shader`, `parameter`, `materials`, `bone`, `team`, `own_in_teams`), `third_person`, `icon`,
`rigid_bodies_when_alive`, `ragdoll_region`, `shader_prefix`}].

## 4. Images

### 4.1 Mod pictures (map JSON `Images.*`) -- the weaker path

Resolve (exe+0x189b0f0): trimmed; refused with `..` anywhere or a root (`\x`, `/x`, `C:\x`, UNC, `\\?\`); joined to the mod
folder; must exist, not be a folder, not be a symlink/junction itself. Not caught: a drive-relative `C:x.png` (INFERRED from
Rust's Windows `PathBuf::push`) and junctions in the middle of the path. Decode (exe+0x455430): file <= 8 MiB, then
`image::ImageReader::open` -> the decoder is chosen by EXTENSION (png / jpg / jpeg / bmp; anything else fails; content is
not sniffed). Limits: the image crate's default 512 MiB allocation, NO width/height cap -> a small hostile PNG/JPEG can
make the client allocate hundreds of MB. Mods' own `mods_ui` picture uses content sniffing instead (same three formats).

### 4.2 Server vote-card previews -- hardened

Server side: a bare png/jpg/jpeg file name in its Images folder, canonicalized inside it, <= 4 MiB, PNG or JPEG magic.
Client side: 8 KiB chunks, sequential, total 1..4 MiB, SHA-256 must match; format sniffed and must match the advertised one;
PNGs rebuilt keeping only IHDR/PLTE/IDAT/tRNS/IEND (unknown critical chunk = error, <= 4096 chunks); <= 4096 px a side,
<= 8.4 Mpx, <= 32 MiB decoded.

### 4.3 PSD, GIF, WebP, TIFF ...

Not compiled in (TIFF / quick-xml / roxmltree / SQLite appear in the client's license list but are not linked). The 5
`.psd` files in installed Workshop items are source art nobody reads; servers may not share them.

## 5. Libraries and versions

| Component | Where | Version | Mod / server content reaching it | Notes |
|---|---|---|---|---|
| image + png + zune-jpeg + bmp | client | 0.25.10 / 0.18.1 / 0.5.15 | map JSON pictures, vote previews, customization thumbnails | Rust; zune-jpeg has unsafe AVX2; no dimension cap on mod pictures |
| serde_json / toml | client | 1.0.151 / 1.1.6 | every JSON above | recursion limit |
| xml-rs + xmltree | client | 0.8.29 / 0.10.3 | UPnP device XML from the LAN | no external entities |
| flate2 / miniz_oxide | client | 1.1.10 / 0.9.1 | packs, PNG | |
| lewton | client | 0.10.2 (unmaintained) | MCC's own menu-music bank only | panics = DoS |
| opus-rs | client | 0.1.34 (pure-Rust port) | every player's voice packets | young; worth fuzzing |
| rustls / ring / hyper | client | 0.23.45 / 0.17.14 / 1.11.1 | network | current |
| FMOD Studio low-level (static) | halo3.dll | 1.10.x (patch level not confirmed) | customization-mod `fmod/pc/sfx*.fsb`; bank names chosen by a map's `snd!` tags | opened with no codec hint (createStream mode 0x2080, exinfo NULL): FSB5, WAV, Ogg, AIFF, FLAC, MOD, S3M, XM, IT, MIDI+DLS, MPEG, playlist, ID3 all try the file |
| `.fsb.info` reader | halo3.dll halo3+0x1727b4 | -- | mod `*.fsb.info` | CHECKED BY HAND: 0x118-byte records read into a stack buffer; the 256-byte name at +0x18 is copied with snprintf(dst, 0x100, "%s"); without a NUL the `%s` scan runs past the record, but the copy is capped at 255 bytes that all come from the record, and the scan stops within the same frame (the "%s.info" path buffer 8 bytes later is NUL-terminated; saved pointers above). No crash, no leak: informational |
| BLF chunk reader | halo3.dll halo3+0xd458/0xd570 | -- | `.halo3_mapinfo`, `.mvar`, map variants | framing checked (size >= 12, <= remaining; signed compare, negatives return "not found"); payloads not audited |
| zlib (static) | halo3.dll | 1.1.4 (CHECKED: inflate_blocks frees `blens` after inflate_codes_new, halo3+0x3e5763/0x3e577d, and only on Z_DATA_ERROR at 0x3e5834 -- the CVE-2002-0059 fix) | BLF `_cmp` chunks (output bounded, halo3+0x600337), game-state blobs, file-share downloads; NOT .map resource pages (own DEFLATE decoder halo3+0x5fd898..0x5ff56c: counts, Kraft sum and window checked) | no known inflate-side memory corruption in 1.1.4 |
| Havok | halo3.dll | 4.5.0.3 | collision + MOPP bytecode in .map | trusts the bytecode it interprets |
| OpenSSL (static) | halo3.dll | 1.1.1-series | no mod path found | EOL |
| Bink | MCC bink2w64.dll | 2.7n (2019) | mod `bink/*.bik` | refused under this loader ("Bink import slots differ") |
| Expat (+ libpng 1.2.5, zlib 1.1.4) | MCC halo3/waves (WavesLibDLL.dll, WaveShell-Xbox.dll) | Expat 1.0.x (1998: 9 handler slots, no error table, 32-bit buffer math) | none: four hard-coded preset names, resolved only in the installed MCC folder | unreachable (section 6); libpng dead (no PNG in any Waves DLL, no UI) |
| Windows image / XML APIs (WIC, GDI+, MSXML, XmlLite, D3DX) | -- | -- | -- | imported by neither binary; this loader serves no image decoder to the game |

## 6. Can downloaded mod content reach the old libraries?

- **Waves / Expat 1.0.x: NOT REACHABLE.** halo3.dll loads the four Waves DLLs from `<halo3.dll folder>\waves\`
  (halo3+0x25aa18, LoadLibraryExA, never through the client's redirect; headless servers never load them). Presets are four
  literals (`radio_eq_q4.xps`, `L360_nochange.xps`, `h3_obstruction_q1.xps`, `h3_occlusion_q1.xps`, halo3+0x1d674a..0x1d738e),
  read whole through the client's asset-path redirect type 6 (exe+0x63f170), which accepts only `waves\<[A-Za-z0-9_-]+>.xps`
  and resolves it ONLY in the installed MCC halo3 folder -- never a mod, Workshop or Mods\Downloaded folder -- then handed to
  `WavesFX_LoadMemPresetByIndex`. No map tag carries XML. Mods may not ship `.xps` (allowlist) and nothing writes into
  `<MCC>/halo3/waves`. The 1998 Expat's CVEs (CVE-2009-3720/3560 over-reads, CVE-2016-0718, CVE-2022-25235, ...) would need
  write access to the MCC install first.
- **libpng 1.2.5 in WavesLibDLL: dead code** (no PNG image in any Waves DLL; no plug-in UI is ever built).
- **zlib: 1.1.4** (fixed), and not on the .map path (section 5).
- **FMOD: REACHABLE.** halo3+0x172fc8 builds `fmod\pc\sfx[.<name>].fsb`, asks the client (redirect type 3: name parts
  `[A-Za-z0-9_-]`, no traversal), which looks in customization-mod folders first (exe+0xa48d90 -> 0x5c7b80; servers push
  customization mods on join, `.fsb` <= 2 GiB) and then MCC's `fmod/pc`; halo3+0x460b60 opens it with
  `System::createStream(name, 0x2080, NULL)` -> FMOD picks the decoder by content. A mod can REPLACE the main bank (`sfx.fsb`,
  opened at init, halo3+0x172984) or ADD banks: at map load halo3+0xa8f80 walks every `snd!` tag and loads `sfx.<string>.fsb`
  for a non-zero string id at +0x20.

## 7. This loader's own parsers -- found and fixed (10-19)

| Finding | Fixed in |
|---|---|
| Stats server: CSRF guard found in the request BODY (any website could run every control), no Host check (DNS rebinding: read chat, IPs, voice archive, recordings), `Access-Control-Allow-Origin: *` on live data | stats.c `st_conn` (FIXES 18) |
| CPU compute interpreter: `(int)idx < ntemps` let a bytecode register index >= 2^31 read / WRITE 32 GB+ past the register file; register counts could go negative | dxbc_cpu.c (FIXES 15) |
| Tag-name offsets from .map files unbounded (crash / heap bytes in the page); collision node index negative; unbounded edge / zone-triangle growth | hitbox.c (FIXES 18) |
| NaN positions broke the JSON; spot labels unescaped; index read before its check; heatmap names not terminated; forensic JSON reader past the end | stats.c, forensic.c (FIXES 18) |
| RemoveDirectoryW deleted non-empty trees; DeleteFileW always succeeded; free space faked (200 GB); Recycle-Bin delete a stub claiming success | peload.c (FIXES 4) |
| Map tools (run on every map, Workshop / server maps too): no memory / time limit, inflate without a cap, unbounded counts / names | stats.c `st_tool_wait`, re-tools/h3map.py, h3bsp.py (FIXES 18) |
| Workshop downloads without a size / disk check; servers.sh printed escape sequences from server names | workshop-helper.sh, servers.sh (FIXES 20) |

Open (FIXES section 22): the loader's path handling is not a sandbox (`..` past the drive root, Z:\ = /, symlinks never
reported as reparse points, GetFinalPathNameByHandleW unresolved -- the client's "escapes its package" checks pass for
symlinks), BCryptVerifySignature always succeeds. Also open: per-map caches keyed by the map stem only (a Workshop map named
`riverworld.map` takes Valhalla's picture, walking graph and spots); `/tmp` logs world-readable.

## 8. Leads for memory corruption via downloaded mods (ranked; none is a confirmed bug in the game's binaries)

1. **FMOD 1.10.x secondary codecs via a customization mod's `.fsb`** (or a bank a map names): a file that does not start
   with `FSB5` falls through to WAV / Ogg / AIFF / FLAC / MOD / S3M / XM / IT / MIDI+DLS / MPEG / playlist / ID3; only the
   codec's open() has to run. Classic bug classes: tracker order / pattern / sample tables and lengths past the file, MIDI
   track lengths, DLS wave-pool offsets, FLAC metadata lengths / rice partitions, Ogg codebook headers, ID3v2 frame sizes.
   Versions of FMOD's bundled Vorbis / FLAC / MPEG decoders are unknown (no vendor strings). Fuzz target #1.
2. **The FSB5 container itself** (the first parser of every `.fsb`): sample count, header / name-table / data sizes,
   per-sample offsets, extra chunks (seek tables, DSP coefficients). Vorbis-in-FSB uses FMOD's built-in setup headers
   chosen by CRC, so only packet data is attacker-controlled.
3. **Havok 4.5.0.3 collision / MOPP bytecode and halo3's 2008 tag code in a downloaded `.map`**: MOPP jump offsets, shape
   keys, BSP node / plane / triangle indices -- the resource pages reach them intact (the page decoder looked bounded).
4. **What halo3.dll does with the pre-validated map variant** -- the only residue of the custom-game pipeline (section 9):
   unused palette datum entries and the trailing fields of the 0xE0A0-byte struct are copied into halo3 without being
   resolved. PLAUSIBLE, low likelihood (halo3's tag handles are salt-checked); follow-up = halo3's variant -> object spawn.
5. **zune-jpeg's unsafe AVX2 code** for mod / server JPEGs (no known CVE), and memory exhaustion from mod pictures (no
   dimension cap).

Not leads: Expat 1.0.x and libpng 1.2.5 (unreachable), zlib (1.1.4, not on the .map path), the `.fsb.info` over-read
(bounded within its own frame), PSD / GIF / WebP / TIFF (no decoder), XXE (no external entities anywhere).

## 9. Custom games, map variants, Megalo scripts, frozen objects (CONFIRMED by static RE)

The whole pipeline is parsed, bounds-checked and tag-resolved by Rust in the client before anything reaches halo3.dll, and
a SERVER's variant (join ticket: exe+0x82a440 -> 0x5f3310 -> variant builder 0x625390) runs the same validators as a local file.

- **`script`** = Reclaimer's own Megalo (the Halo: Reach game-type language, written as ReachVariantTool source), compiled by
  the client (`mode-compile` / `megalo-check`; at most 16 variables, 32 actions per event, two index levels) and run by a
  Rust VM (`custom_modes/megalo/vm.rs`). It acts on the game through an adapter (action dispatcher exe+0x1698610) that checks
  generational entity handles ("Stale Megalo entity reference") and HUD / placement limits and calls engine functions with
  validated handles -- no raw object-table writes.
- **`frozen` ("FRZ1")**: decoder exe+0x186ee80: magic, count <= 5000, second count <= count, length-prefixed UTF-8 names
  (exe+0x186f910, overflow-checked), fixed 0x41-byte records range-checked, reference tag 0/1 with index < count; applied
  through the same adapter calls. "The map's frozen objects are damaged" otherwise.
- **`map.data`** = the retail s_map_variant struct, exactly 0xE0A0 bytes (nv_validate exe+0x18e0160): version 0x0E, placements
  < 641 and >= the scenario-object count, palette <= 256, every placement's object handles -1, quota index -1 or < palette
  count, finite transforms, the 14 scenario-slot words in range. Every USED palette slot is resolved by name against the
  installed maps (exe+0x1c46f70, "This map uses objects that are not installed"). The struct is then copied into halo3's
  variant memory (Rust; halo3's own BLF / bitstream variant decoder is not used on this path).
- **`objects`** (borrowed palette: code <= 639, name <= 32 bytes UTF-8, resolved by name), **`sky`** (resolved by name),
  **`gravity`** 10-300, **`atmosphere`**, **`world`**, **`boundaries`**: scalar or name-resolved -- no index into native arrays.
- **`packed_map` / `packed_game`**: checked against the base map / mode and handled as Rust structs; no halo3 decode seen.
- Residue: unused palette datum entries and the struct's trailing fields (0xD34C..0xE0A0) are not resolved by Rust; whether
  halo3 reads them at map load is not traced (section 8, lead 4).
