# Project Reclaimer game options (v0.9.18)

Options that a dedicated-server playlist game can set, as `"options": { "<key>": <value> }` (see the
playlist in `C:\ReclaimerTestServer\playlists\betrayal.playlist.json`). Every key and value below was checked
against the client's own validator (`dedicated --config <toml> check`), which names an unknown key ("Did you
mean ...?"), lists the allowed choices for a wrong value and prints numeric ranges. Choice values are strings
(`"On"`, `"Assault rifles"`, ...; case-insensitive). Numbers are JSON numbers (a quoted number is rejected).

The probe used then (a scratch script, not kept) built one playlist game per key and parsed the validator's
messages. To repeat a check: write the game into a playlist in the server folder `shim/dedicated.sh` creates
(`C:\ReclaimerTestServer`), point a config's `playlist =` at it and run the client's `dedicated --config <toml>
check` (this Mac's prefix has `optprobe.toml` + `playlists/optprobe.playlist.json` for that; not in the repo).

## Rounds (all game types)

| Key | What | Values |
|---|---|---|
| `rounds.teams` | Teams enabled | Off, On |
| `rounds.team_changing` | Team changing | Disabled, Enabled, Balancing only |
| `rounds.friendly_fire` | Friendly fire | Off, On |
| `rounds.betrayal_booting` | Betrayal booting | Off, On |
| `rounds.reset_players` | Reset players each round | Off, On |
| `rounds.reset_map` | Reset map each round | Off, On |
| `rounds.minutes` | Time limit (0 = unlimited) | 0..60 |
| `rounds.limit` | Round limit | 1..10 |
| `rounds.early_victory` | Wins for early victory (0 = disabled) | 0..5 |

## Respawn

| Key | What | Values |
|---|---|---|
| `respawn.lives` | Lives per round (0 = unlimited) | 0..50 |
| `respawn.team_lives` | Team lives (0 = unlimited) | 0..100 |
| `respawn.seconds` | Respawn delay (s) | 0..240 |
| `respawn.suicide` | Suicide penalty (s) | 0..240 |
| `respawn.betrayal` | Betrayal penalty (s) | 0..240 |
| `respawn.growth` | Respawn growth (s) | 0..15 |
| `respawn.traits_duration` | Respawn traits duration (s) | 0..60 |
| `respawn.synchronize` | Synchronize with team | Off, On |
| `respawn.on_kills` | Respawn on kills | Off, On |

## Powerups and map

| Key | What | Values |
|---|---|---|
| `powerups.red_duration`, `.blue_duration`, `.yellow_duration` | Powerup duration (s) | 0..120 |
| `map.weapons` | Weapon set | Default, Assault rifles, Dual wield, Hammers, Lasers, Rockets, Shotguns, Sniper rifles, Swords, No power weapons, No snipers, No weapons, Random, ODST weapons |
| `map.vehicles` | Vehicle set | Default, None, Mongooses, Light ground, Tanks, Aircraft, No light ground, No tanks, No aircraft, All |
| `map.grenades` | Grenades on map | None, Map default |
| `map.indestructible_vehicles` | Indestructible vehicles | Off, On |

## Player traits

The same trait keys exist in several trait sets: `player.*` (everyone), `respawn_traits.*` (after spawning),
`red_powerup.*`, `blue_powerup.*`, `yellow_powerup.*`, `leader.*` (Slayer), `flag_carrier.*` (CTF),
`ball_carrier.*` (Oddball) and the other modes' role sets (table below: `hill_occupant`, `vip_team`, `vip_proximity`,
`vip`, `juggernaut`, `defender`, `attacker`, `bomb_carrier`, `bomb_arming`, `infected`, `initial_infected`,
`safe_haven`, `last_survivor`, `editor`).

| Trait | What | Values |
|---|---|---|
| `shields` | Shields | Unchanged, None, Normal, 2x, 3x, 4x |
| `recharge` | Shield recharge | Unchanged, -25%, -10%, -5%, Disabled, 50%, 90%, 100%, 110%, 200% |
| `resistance` | Damage resistance | Unchanged, 10%, 50%, 90%, 100%, 110%, 150%, 200%, 300%, 500%, 1000%, 2000%, Invulnerable |
| `headshots` | Headshot immunity | Unchanged, Immune, Vulnerable |
| `vampirism` | Vampirism | Unchanged, Off, 10%, 25%, 50%, 100% |
| `damage` | Damage dealt | Unchanged, 0%, 25%, 50%, 75%, 90%, 100%, 110%, 125%, 150%, 200%, 300%, Instant kill |
| `speed` | Movement speed | Unchanged, 25%, 50%, 75%, 90%, 100%, 110%, 125%, 150%, 200%, 300% |
| `gravity` | Player gravity | Unchanged, 50%, 75%, 100%, 150%, 200% |
| `camouflage` | Active camouflage | Unchanged, Off, Poor, Good, Invisible |
| `waypoint` | Waypoint | Unchanged, Off, Allies, Everyone |
| `aura` | Aura | Unchanged, Off, Team color, Black, White |
| `radar` | Motion tracker | Unchanged, Off, Allies only, Normal, Enhanced |
| `ammo` | Ammunition | Unchanged, Normal, Infinite, Bottomless clip |
| `grenade_regeneration` | Grenade regeneration | Unchanged, On, Off, On, with firebombs |
| `vehicles` | Vehicle use | Unchanged, None, Passenger only, Full use |
| `pickup` | Weapon pickup | Unchanged, Allowed, Disabled |
| `primary_weapon`, `secondary_weapon` | Starting weapon (player set) | Random, Unchanged, Map default, Battle rifle, Assault rifle, Plasma pistol, Spiker, SMG, Carbine, Energy sword, Magnum, Needler, Plasma rifle, Rocket launcher, Shotgun, Sniper rifle, Brute shot, Unarmed, Beam rifle, Spartan laser, No weapon, Gravity hammer, Mauler, Flamethrower, Missile pod, Sentinel beam, Fuel rod gun, Silenced SMG, Automag, Brute plasma rifle, Machine gun turret, Plasma turret, Golf club |
| `grenades` | Starting grenades (player set) | Unchanged, Map default, None |
| `character_set` | Character set | Unchanged, Vanilla characters, Random, Customization mod 1..16 |
| `character` | Character | Players, Random, Character 1..16 |
| `camera` | Camera | Unchanged, Third person, First person |

## Mode options (`mode.*`, per game type)

Game types: Slayer, Capture the Flag, Oddball, King of the Hill, Assault, Infection, Territories, Juggernaut,
VIP, Escalation Slayer (plus game variants: `dedicated content`).

**Slayer:** `mode.score` score to win (-1..500); points -10..10: `mode.kill`, `mode.assist`, `mode.death`,
`mode.suicide`, `mode.betrayal`, `mode.leader_kill`, `mode.elimination`, `mode.assassination`, `mode.headshot`,
`mode.beatdown`, `mode.sticky`, `mode.splatter`, `mode.spree`.

**Capture the Flag:** `mode.score` captures to win, `mode.reset` flag reset (s), `mode.sudden_death` (s; -1 =
unlimited), `mode.touch_return` (s; -1 = disabled), `mode.flag_at_home` (Off, On), `mode.flag_waypoint` (Never,
When not carried by enemy, Away from home, Always), `mode.flag_type` (Multi flag, One flag, Neutral flag); traits
`flag_carrier.*`.

**Oddball:** `mode.score`, `mode.carrying` carrying points, `mode.ball_kill` ball melee points,
`mode.carrier_kill` carrier kill points, `mode.ball_count`, `mode.spawn_delay`, `mode.reset` ball reset delay,
`mode.ball_effect` (Off, On), `mode.auto_pickup` (Off, On), `mode.kill`; traits `ball_carrier.*`.

**King of the Hill, VIP, Juggernaut, Territories, Assault, Infection, Forge** -- read from the client's option table
(the `key` + label pairs in `src/game_options.rs`'s strings), not yet run through the validator. The exact keys are
in "Exact keys" below, which resolves every "probably" in this table. Keys that several
modes share (`score`, `kill`, `death`, `suicide`, `betrayal`, `reset`, `sudden_death`) have no string of their own
next to these labels, so where a label has no key in front of it the key is marked "probably".

| Mode | Keys |
|---|---|
| King of the Hill | `mode.score` (probably), `mode.uncontested` uncontested bonus, `mode.inside` inside hill points, `mode.outside` outside hill points, `mode.opaque_hill` (Off, On), `mode.hill_movement` (10 seconds, 15 seconds, 30 seconds, 1 minute, 2 minutes, 3 minutes, 4 minutes, 5 minutes), `mode.hill_order` (Random, Sequence), `mode.team_scoring` (Sum of team, Minimum score, Maximum score); traits `hill_occupant.*` |
| VIP | `mode.score`, `mode.takedown` VIP takedown points, `mode.vip_kill` kill as VIP points, `mode.vip_death` VIP death points, `mode.arrival` destination arrival points, `mode.vip_suicide` VIP betrayal points, VIP influence radius (key not next to its label), `mode.single_vip`, `mode.destinations`, `mode.end_on_vip_death` (Off, On), `mode.vip_selection` (Random, Next respawn, Next kill, Unchanged), `mode.destination_movement` (On arrival, On switch), `mode.destination_order` (Random, Sequence); traits `vip_team.*`, `vip_proximity.*`, `vip.*` |
| Juggernaut | `mode.score`, Juggernaut kill points (probably `mode.takedown`), `mode.juggernaut_kill` kill as Juggernaut points, `mode.juggernaut_delay` (s), `mode.allied_against_juggernaut`, `mode.respawn_on_lone_juggernaut` (Off, On), `mode.initial_juggernaut` (Random, First kill, First death), `mode.next_juggernaut` (Killer of Juggernaut, Killed by Juggernaut, ...); traits `juggernaut.*` |
| Territories | `mode.capture` capture time (s), `mode.one_sided`, `mode.lock_on_capture` (Off, On), `mode.objective_respawn` (respawn on capture: Off, On friendly capture, On enemy capture, On any capture); traits `defender.*`, `attacker.*` |
| Assault | `mode.score` bombs to win, `mode.reset` bomb reset (s), `mode.arming`, `mode.disarming`, `mode.fuse` (s), `mode.reset_on_disarm` (Off, On), `mode.bomb_type` (Multi bomb, One bomb, Neutral bomb), `mode.bomb_waypoint` (enemy bomb waypoint: Never, When armed, ...), `mode.objective_respawn` (respawn on detonation: On friendly / enemy / any detonation), `mode.sudden_death` (probably); traits `bomb_carrier.*`, `bomb_arming.*` |
| Infection | `mode.initial_count` initial infected count (1 player .. 15 players, or 25% / 50%), `mode.safe_havens` (Off, On), `mode.haven_seconds` safe haven movement (s), `mode.respawn_on_haven_move` (Off, On), `mode.next_infected` (Winner, Loser, ...), infected kill points (probably `mode.kill`), `mode.infection` infection points, safe haven arrival points (probably `mode.arrival`), last survivor bonus (probably `mode.last_survivor`); traits `infected.*`, `initial_infected.*`, `safe_haven.*`, `last_survivor.*` |
| Forge | `mode.edit_mode` (Everyone, Host only); traits `editor.*` |

Per-team characters: `teams.<color>_character_set` and `teams.<color>_character` (red, blue, green, orange, purple,
gold, brown, pink; values as `character_set` / `character` above).

## Reading the running game's options

The live stats page (`play.sh`, http://127.0.0.1:8765/) shows the options and every trait set of the game you are in,
read from halo3's own copy of the variant (shim/docs/halo3-variant-traits.md), using these key names. It decodes every
option of the running mode with `option-map.json` (below; built into the loader with `#embed` and served as
`/option-map.json`). Checked live on Slayer (2026-10-17): all rounds / respawn / map / powerup / points values and the
powerup trait sets matched the game.

## Exact keys (from the client's option table)

Found 2026-10-17 by static RE of `client/project-reclaimer-client-v0.9.18.exe` only (capstone; nothing run, no live
memory). Machine-readable form: **`shim/docs/option-map.json`** (664 entries = 639 distinct keys; a key that sits at
a different native offset in different game types, e.g. `mode.kill`, has one entry per mapping, with its `modes`).
All keys below are exact strings from the client's tables; every "probably" in the section above is resolved.

### Where it comes from (client functions, RVAs)

- `0x172c080(out, game_type)` builds the complete option list for one game type as 0x90-byte records via the builder
  `0x17307a0(rec, group, key, label, offset, size, min, max)`, which stores key = `"<group>.<key>"`, label,
  **native offset = offset + 4** (rec+0x70), size (rec+0x78), min/max (rec+0x80/+0x84, i32), choices `&[&str]`
  (rec+0x40), an i32 list (rec+0x50, "UI value list" below), aliases `&[(&str, i32)]` (rec+0x60), bit-flag marker
  (rec+0x88) + bit (rec+0x89) and a UI-hidden byte (rec+0x8A). Sources: 25 base options written out in code
  (0x172c101..0x172cc60); trait sets (5 common sets + the game type's role sets from the list at 0x2351688/0x23516b0)
  x (camera, character_set, character, primary/secondary weapon + the 19-entry trait table at 0x22be2d0, 0x48-byte
  entries); `mode.*` bit flags (0x30-byte tables, jump table 0x2370cd4), choices (0x40-byte tables, 0x235170c /
  0x2351738) and integers (0x38-byte tables, 0x2351788 / 0x23517d8); `teams.<color>_*` (8 colors).
- Game types (client index -> halo3 engine index, table 0x2351828): 0 Capture the Flag -> 1, 1 Slayer -> 2, 2 Forge
  -> 5, 3 Oddball -> 3, 4 King of the Hill -> 4, 5 VIP -> 6, 6 Juggernaut -> 7, 7 Territories -> 8, 8 Assault -> 9,
  9 Infection -> 10, 10 Escalation Slayer -> 12 (names: 0x2351854/0x2351880).
- Playlist value -> i32 (`0x1aca690`, the playlist parser; its messages sit next to the `src/playlist.rs` string): a JSON number is taken as is; a string is compared (via
  0x1ac87d0, case-insensitive) with the choice labels -> value = **min + choice index**, then with the aliases ->
  alias value; anything else is an error listing the choices. Integer options have no choices, so they take numbers
  only; choice options also accept a number in range.
- Validate `0x1730fc0`: unknown key -> "Unknown game option for <type>"; value outside [min, max] -> error.
- Apply `0x1730a70(out, options, game_type, v)`: refuses unless `*(i32*)v` == the engine index ("Native game layout
  mismatch!"); then per option with offset != -1: bit flag -> `v[off] = (v[off] & ~(1<<bit)) | (value<<bit)` (one
  byte); otherwise the i32 value's low `size` bytes are copied to `v+off` (size 1 or 2, offset+size <= 0x4C4).
  `v` = halo3's c_game_variant (halo3-variant-traits.md), built by halo3 and filled by the host path at 0x59d580.
- Read-back `0x172bf50`: flag -> bit test; size 1/2 -> **sign-extended iff min < 0** (that is the `signed` column);
  `0x17316b0` re-reads every option after halo3's own validation and fails with "Native engine could not preserve
  <label> = <value>" if the engine changed it.
- Options whose offset is forced to -1 (`variant: null`) are client-side only and never touch the variant: every
  trait set's `camera`, `character_set`, `character`, and all `teams.*`.

Notation: `v+0x104 bit 0` = bit 0 of the byte at c_game_variant + 0x104; `u8/s8/u16/s16` = size and signedness.

### Base options (all 11 game types)

| Key | Label | Native field | Values |
|---|---|---|---|
| `rounds.teams` | Teams enabled (hidden in the UI for Capture the Flag, VIP, Juggernaut, Territories, Assault, Infection) | v+0x104 bit 0 | 0 Off / 1 On |
| `rounds.team_changing` | Team changing | v+0x12E u16 | 0 Disabled, 1 Enabled, 2 Balancing only |
| `rounds.friendly_fire` | Friendly fire | v+0x12C bit 0 | 0 Off / 1 On |
| `rounds.betrayal_booting` | Betrayal booting | v+0x12C bit 1 | 0 Off / 1 On |
| `rounds.reset_players` | Reset players each round | v+0x104 bit 1 | 0 Off / 1 On |
| `rounds.reset_map` | Reset map each round | v+0x104 bit 2 | 0 Off / 1 On |
| `rounds.minutes` | Time limit (minutes; 0 = unlimited) | v+0x105 u8 | 0..60 |
| `rounds.limit` | Round limit | v+0x106 u8 | 1..10 |
| `rounds.early_victory` | Wins for early victory (0 = disabled) | v+0x107 u8 | 0..5 |
| `respawn.lives` | Lives per round (0 = unlimited) | v+0x109 u8 | 0..50 |
| `respawn.team_lives` | Team lives (0 = unlimited) | v+0x10A u8 | 0..100 |
| `respawn.seconds` | Respawn delay (seconds) | v+0x10B u8 | 0..240 |
| `respawn.suicide` | Suicide penalty (seconds) | v+0x10C u8 | 0..240 |
| `respawn.betrayal` | Betrayal penalty (seconds) | v+0x10D u8 | 0..240 |
| `respawn.growth` | Respawn growth (seconds) | v+0x10E u8 | 0..15 |
| `respawn.traits_duration` | Respawn traits duration (seconds) | v+0x10F u8 | 0..60 |
| `respawn.synchronize` | Synchronize with team | v+0x108 bit 0 | 0 Off / 1 On |
| `respawn.on_kills` | Respawn on kills | v+0x108 bit 3 | 0 Off / 1 On |
| `powerups.red_duration` | Red powerup duration | v+0x1A8 u8 | 0..120 |
| `powerups.blue_duration` | Blue powerup duration | v+0x1A9 u8 | 0..120 |
| `powerups.yellow_duration` | Yellow powerup duration | v+0x1AA u8 | 0..120 |
| `map.weapons` | Weapon set | v+0x150 u16 | 0 Default, 1 Assault rifles, 2 Dual wield, 3 Hammers, 4 Lasers, 5 Rockets, 6 Shotguns, 7 Sniper rifles, 8 Swords, 9 No power weapons, 10 No snipers, 11 No weapons, 12 Random, 13 ODST weapons |
| `map.vehicles` | Vehicle set | v+0x152 u16 | 0 Default, 1 None, 2 Mongooses, 3 Light ground, 4 Tanks, 5 Aircraft, 6 No light ground, 7 No tanks, 8 No aircraft, 9 All |
| `map.grenades` | Grenades on map | v+0x130 bit 0 | 0 None / 1 Map default |
| `map.indestructible_vehicles` | Indestructible vehicles | v+0x130 bit 1 | 0 Off / 1 On |

### Mode options (`mode.*`)

Escalation Slayer has **no** `mode.*` options (the client's mode tables cover game types 0..9 only); Forge has only
`edit_mode`.

**Capture the Flag** -- `flag_at_home` Flag at home to score v+0x1B4 bit 0 [0 Off / 1 On]; `flag_waypoint` Home flag waypoint v+0x1B5 u8 [0 Never, 1 When not carried by enemy, 2 Away from home, 3 Always]; `flag_type` Flag type v+0x1B6 u8 [0 Multi flag, 1 One flag, 2 Neutral flag]; `objective_respawn` Respawn on capture v+0x1B7 u8 [0 Normal, 1 On friendly capture, 2 On enemy capture, 3 On any capture]; `touch_return` Touch return (seconds; -1 = disabled) v+0x1B8 s16 [-1..300]; `sudden_death` Sudden death (seconds; -1 = unlimited) v+0x1BA s16 [-1..300]; `score` Captures to win v+0x1BC u16 [0..50]; `reset` Flag reset (seconds) v+0x1BE u16 [0..300]

**Slayer** -- `team_scoring` Team scoring v+0x1AE u16 [0 Sum of team, 1 Minimum score, 2 Maximum score]; `score` Score to win v+0x1B4 s16 [-1..500]; `kill` Kill points v+0x1B6 s16 [-10..10]; `assist` Assist points v+0x1B8 s8 [-10..10]; `death` Death points v+0x1B9 s8 [-10..10]; `suicide` Suicide points v+0x1BA s8 [-10..10]; `betrayal` Betrayal points v+0x1BB s8 [-10..10]; `leader_kill` Leader kill bonus v+0x1BC s8 [-10..10]; `elimination` Elimination bonus v+0x1BD s8 [-10..10]; `assassination` Assassination bonus v+0x1BE s8 [-10..10]; `headshot` Headshot bonus v+0x1BF s8 [-10..10]; `beatdown` Beatdown bonus v+0x1C0 s8 [-10..10]; `sticky` Sticky bonus v+0x1C1 s8 [-10..10]; `splatter` Splatter bonus v+0x1C2 s8 [-10..10]; `spree` Spree bonus v+0x1C3 s8 [-10..10]

**Forge** -- `edit_mode` Allow editing v+0x1B5 u8 [0 Everyone, 1 Host only]

**Oddball** -- `team_scoring` Team scoring v+0x1AE u16 [0 Sum of team, 1 Minimum score, 2 Maximum score]; `auto_pickup` Automatic ball pickup v+0x1B4 bit 0 [0 Off / 1 On]; `ball_effect` Ball carrier effect v+0x1B4 bit 1 [0 Off / 1 On]; `score` Score to win v+0x1B8 s16 [-1..1000]; `carrying` Carrying points v+0x1BA s16 [-10..10]; `kill` Kill points v+0x1BC s8 [-10..10]; `ball_kill` Ball melee points v+0x1BD s8 [-10..10]; `carrier_kill` Carrier kill points v+0x1BE s8 [-10..10]; `ball_count` Ball count v+0x1BF u8 [1..3]; `spawn_delay` Ball spawn delay v+0x1C0 u16 [0..120]; `reset` Ball reset delay v+0x1C2 u16 [0..120]

**King of the Hill** -- `team_scoring` Team scoring v+0x1AE u16 [0 Sum of team, 1 Minimum score, 2 Maximum score, 3 Team control]; `opaque_hill` Opaque hill v+0x1B4 bit 0 [0 Off / 1 On]; `score` Score to win v+0x1B8 u16 [0..1000]; `hill_movement` Hill movement v+0x1BA u8 [0 Off, 1 10 seconds, 2 15 seconds, 3 30 seconds, 4 1 minute, 5 2 minutes, 6 3 minutes, 7 4 minutes, 8 5 minutes]; `hill_order` Hill order v+0x1BB u8 [0 Random, 1 Sequence]; `uncontested` Uncontested bonus v+0x1BC s8 [-10..10]; `kill` Kill points v+0x1BD s8 [-10..10]; `inside` Inside hill points v+0x1BE s8 [-10..10]; `outside` Outside hill points v+0x1BF s8 [-10..10]

**VIP** -- `score` Score to win v+0x1B4 u16 [0..500]; `single_vip` Single VIP v+0x1B6 bit 0 [0 Off / 1 On]; `destinations` Destination zones v+0x1B6 bit 1 [0 Off / 1 On]; `end_on_vip_death` End round on VIP death v+0x1B6 bit 2 [0 Off / 1 On]; `kill` Kill points v+0x1B8 s8 [-10..10]; `takedown` VIP takedown points v+0x1B9 s8 [-10..10]; `vip_kill` Kill as VIP points v+0x1BA s8 [-10..10]; `vip_death` VIP death points v+0x1BB s8 [-10..10]; `arrival` Destination arrival points v+0x1BC s8 [-10..10]; `suicide` Suicide points v+0x1BD s8 [-10..10]; `betrayal` Betrayal points v+0x1BE s8 [-10..10]; `vip_suicide` VIP betrayal points v+0x1BF s8 [-10..10]; `vip_selection` VIP selection v+0x1C0 u8 [0 Random, 1 Next respawn, 2 Next kill, 3 Unchanged]; `destination_movement` Destination movement v+0x1C1 u8 [0 Off, 1 10 seconds, 2 15 seconds, 3 30 seconds, 4 1 minute, 5 2 minutes, 6 3 minutes, 7 4 minutes, 8 5 minutes, 9 On arrival, 10 On switch]; `destination_order` Destination order v+0x1C2 u8 [0 Random, 1 Sequence]; `radius` VIP influence radius v+0x1C4 u16 [0..50]

**Juggernaut** -- `score` Score to win v+0x1B4 u16 [0..500]; `initial_juggernaut` Initial Juggernaut v+0x1B8 u8 [0 Random, 1 First kill, 2 First death]; `next_juggernaut` Next Juggernaut v+0x1B9 u8 [0 Killer of Juggernaut, 1 Killed by Juggernaut, 2 Unchanged, 3 Random]; `allied_against_juggernaut` Allied against Juggernaut v+0x1BA bit 0 [0 Off / 1 On]; `respawn_on_lone_juggernaut` Respawn when Juggernaut is alone v+0x1BA bit 1 [0 Off / 1 On]; `destinations` Destination zones v+0x1BA bit 2 [0 Off / 1 On]; `destination_movement` Destination movement v+0x1BB u8 [0 Off, 1 10 seconds, 2 15 seconds, 3 30 seconds, 4 1 minute, 5 2 minutes, 6 3 minutes, 7 4 minutes, 8 5 minutes, 9 On arrival, 10 On switch]; `destination_order` Destination order v+0x1BC u8 [0 Random, 1 Sequence]; `kill` Kill points v+0x1BD s8 [-10..10]; `takedown` Juggernaut kill points v+0x1BE s8 [-10..10]; `juggernaut_kill` Kill as Juggernaut points v+0x1BF s8 [-10..10]; `arrival` Destination arrival points v+0x1C0 s8 [-10..10]; `suicide` Suicide points v+0x1C1 s8 [-10..10]; `betrayal` Betrayal points v+0x1C2 s8 [-10..10]; `juggernaut_delay` Juggernaut delay (seconds) v+0x1C3 u8 [0..10]

**Territories** -- `one_sided` One-sided territories v+0x1B4 bit 0 [0 Off / 1 On]; `lock_on_capture` Lock territories on capture v+0x1B4 bit 1 [0 Off / 1 On]; `objective_respawn` Respawn on capture v+0x1B6 u16 [0 Normal, 1 On friendly capture, 2 On enemy capture, 3 On any capture]; `capture` Capture time (seconds) v+0x1B8 u16 [0..120]; `sudden_death` Sudden death (seconds; -1 = unlimited) v+0x1BA s16 [-1..300]

**Assault** -- `reset_on_disarm` Reset bomb on disarm v+0x1B4 bit 0 [0 Off / 1 On]; `objective_respawn` Respawn on detonation v+0x1B6 u16 [0 Normal, 1 On friendly detonation, 2 On enemy detonation, 3 On any detonation]; `bomb_type` Bomb type v+0x1B8 u16 [0 Multi bomb, 1 One bomb, 2 Neutral bomb]; `bomb_waypoint` Enemy bomb waypoint v+0x1BA u16 [0 Never, 1 When not carried by enemy, 2 When armed, 3 Always]; `score` Bombs to win v+0x1BC u16 [0..50]; `sudden_death` Sudden death (seconds; -1 = unlimited) v+0x1BE s16 [-1..300]; `reset` Bomb reset (seconds) v+0x1C0 u16 [0..60]; `arming` Bomb arming (seconds) v+0x1C2 u16 [0..30]; `disarming` Bomb disarming (seconds) v+0x1C4 u16 [0..30]; `fuse` Bomb fuse (seconds) v+0x1C6 u16 [0..30]

**Infection** -- `respawn_on_haven_move` Respawn on safe haven movement v+0x1B4 bit 0 [0 Off / 1 On]; `safe_havens` Safe havens v+0x1B5 u8 [0 Off, 1 Sequence, 2 Random]; `next_infected` Next infected v+0x1B6 u8 [0 Winner, 1 Loser, 2 Unchanged, 3 Random]; `initial_count` Initial infected count v+0x1B7 u8 [0 25%, 1 50%, 2 1 player, 3 2 players, 4 3 players, 5 4 players, 6 5 players, 7 6 players, 8 7 players, 9 8 players, 10 9 players, 11 10 players, 12 11 players, 13 12 players, 14 13 players, 15 14 players, 16 15 players]; `haven_seconds` Safe haven movement (seconds) v+0x1B8 u16 [0..120]; `kill` Infected kill points v+0x1BA s8 [-10..10]; `infection` Infection points v+0x1BB s8 [-10..10]; `arrival` Safe haven arrival points v+0x1BC s8 [-10..10]; `suicide` Suicide points v+0x1BD s8 [-10..10]; `betrayal` Betrayal points v+0x1BE s8 [-10..10]; `last_survivor` Last survivor bonus v+0x1BF s8 [-10..10]

### Trait sets

Every trait key exists in every set the game type has; key = `<set>.<trait>`, native field = set offset + the trait's
offset below. Sets (v+ = start of that c_player_traits, 0x1C bytes): `player` v+0x134, `respawn_traits` v+0x110,
`red_powerup` v+0x154, `blue_powerup` v+0x170, `yellow_powerup` v+0x18C (all game types); `flag_carrier` v+0x1C0
(CTF); `leader` v+0x1C4 (Slayer); `editor` v+0x1B8 (Forge); `ball_carrier` v+0x1C4 (Oddball); `hill_occupant` v+0x1C0
(KOTH); `vip_team` v+0x1C6, `vip_proximity` v+0x1E2, `vip` v+0x1FE (VIP); `juggernaut` v+0x1C4 (Juggernaut);
`defender` v+0x1BC, `attacker` v+0x1D8 (Territories); `bomb_carrier` v+0x1C8, `bomb_arming` v+0x1E4 (Assault);
`infected` v+0x1C0, `initial_infected` v+0x1DC, `safe_haven` v+0x1F8, `last_survivor` v+0x214 (Infection).
Escalation Slayer gets only the five common sets. All of these agree with halo3-variant-traits.md.

| Trait key | Label | +off in set | Type | Values (value = choice index unless noted) |
|---|---|---|---|---|
| `camera` | Camera | -- | client-side | 0 Unchanged, 1 Third person, 2 First person |
| `character_set` | Character set | -- | client-side | 0 Unchanged, 1 Vanilla characters, 2 Random, 3 Customization mod 1, 4 Customization mod 2, 5 Customization mod 3, 6 Customization mod 4, 7 Customization mod 5, 8 Customization mod 6, 9 Customization mod 7, 10 Customization mod 8, 11 Customization mod 9, 12 Customization mod 10, 13 Customization mod 11, 14 Customization mod 12, 15 Customization mod 13, 16 Customization mod 14, 17 Customization mod 15, 18 Customization mod 16 |
| `character` | Character | -- | client-side | 0 Players, 1 Random, 2 Character 1, 3 Character 2, 4 Character 3, 5 Character 4, 6 Character 5, 7 Character 6, 8 Character 7, 9 Character 8, 10 Character 9, 11 Character 10, 12 Character 11, 13 Character 12, 14 Character 13, 15 Character 14, 16 Character 15, 17 Character 16 |
| `primary_weapon` | Starting primary weapon | +0xA | s8 | -3 Random, -2 Unchanged, -1 Map default, 0 Battle rifle, 1 Assault rifle, 2 Plasma pistol, 3 Spiker, 4 SMG, 5 Carbine, 6 Energy sword, 7 Magnum, 8 Needler, 9 Plasma rifle, 10 Rocket launcher, 11 Shotgun, 12 Sniper rifle, 13 Brute shot, 14 Unarmed, 15 Beam rifle, 16 Spartan laser, 17 No weapon, 18 Gravity hammer, 19 Mauler, 20 Flamethrower, 21 Missile pod, 22 Sentinel beam, 23 Fuel rod gun, 24 Silenced SMG, 25 Automag, 26 Brute plasma rifle, 27 Machine gun turret, 28 Plasma turret, 29 Golf club. |
| `secondary_weapon` | Starting secondary weapon | +0xB | s8 | -3 Random, -2 Unchanged, -1 Map default, 0 Battle rifle, 1 Assault rifle, 2 Plasma pistol, 3 Spiker, 4 SMG, 5 Carbine, 6 Energy sword, 7 Magnum, 8 Needler, 9 Plasma rifle, 10 Rocket launcher, 11 Shotgun, 12 Sniper rifle, 13 Brute shot, 14 Unarmed, 15 Beam rifle, 16 Spartan laser, 17 No weapon, 18 Gravity hammer, 19 Mauler, 20 Flamethrower, 21 Missile pod, 22 Sentinel beam, 23 Fuel rod gun, 24 Silenced SMG, 25 Automag, 26 Brute plasma rifle, 27 Machine gun turret, 28 Plasma turret, 29 Golf club. |
| `resistance` | Damage resistance | +0x0 | u8 | 0 Unchanged, 1 10%, 2 50%, 3 90%, 4 100%, 5 110%, 6 150%, 7 200%, 8 300%, 9 500%, 10 1000%, 11 2000%, 12 Invulnerable. |
| `recharge` | Shield recharge | +0x1 | u8 | 0 Unchanged, 1 -25%, 2 -10%, 3 -5%, 4 Disabled, 5 50%, 6 90%, 7 100%, 8 110%, 9 200%. |
| `vampirism` | Vampirism | +0x2 | u8 | 0 Unchanged, 1 Off, 2 10%, 3 25%, 4 50%, 5 100%. |
| `headshots` | Headshot immunity | +0x3 | u8 | 0 Unchanged, 1 Immune, 2 Vulnerable. |
| `shields` | Shields | +0x4 | u8 | 0 Unchanged, 1 None, 2 Normal, 3 2x, 4 3x, 5 4x. |
| `grenades` | Starting grenades | +0x8 | u16 | 0 Unchanged, 1 Map default, 2 None. |
| `damage` | Damage dealt | +0xC | u8 | 0 Unchanged, 1 0%, 2 25%, 3 50%, 4 75%, 5 90%, 6 100%, 7 110%, 8 125%, 9 150%, 10 200%, 11 300%, 12 Instant kill. |
| `grenade_regeneration` | Grenade regeneration | +0xD | u8 | 0 Unchanged, 1 On, 2 Off, 3 On, with firebombs. UI value list [0, 2, 1, 3]. |
| `ammo` | Ammunition | +0xE | u8 | 0 Unchanged, 1 Normal, 2 Infinite, 3 Bottomless clip. |
| `pickup` | Weapon pickup | +0xF | u8 | 0 Unchanged, 1 Allowed, 2 Disabled. |
| `speed` | Movement speed | +0x10 | u8 | 0 Unchanged, 1 25%, 2 50%, 3 75%, 4 90%, 5 100%, 6 110%, 7 125%, 8 150%, 9 200%, 10 300%. |
| `gravity` | Player gravity | +0x11 | u8 | 0 Unchanged, 1 50%, 2 75%, 3 100%, 4 150%, 5 200%. |
| `vehicles` | Vehicle use | +0x12 | u8 | 0 Unchanged, 1 None, 2 Passenger only, 3 Full use. |
| `camouflage` | Camouflage | +0x14 | u8 | 0 Unchanged, 1 Off, 2 Poor, 3 Good, 4 Invisible. |
| `waypoint` | Waypoint | +0x15 | u8 | 0 Unchanged, 1 Off, 2 Allies, 3 Everyone. |
| `aura` | Aura | +0x16 | u8 | 0 Unchanged, 1 Off, 2 Team color, 3 Black, 4 White. |
| `forced_color` | Forced color | +0x17 | u8 | 0 Unchanged, 1 Off, 2 Red, 3 Blue, 4 Green, 5 Orange, 6 Purple, 7 Gold, 8 Brown, 9 Pink, 10 White, 11 Black, 12 Zombie. |
| `radar` | Motion tracker | +0x18 | u16 | 0 Unchanged, 1 Off, 2 Allies only, 3 Normal, 4 Enhanced. Aliases: "Moving players" = 2, "All players" = 3, "Allies" = 4. |
| `radar_range` | Motion tracker range | +0x1A | u16 | 0 Unchanged, 1 10 m, 2 15 m, 3 25 m, 4 50 m, 5 75 m, 6 100 m, 7 150 m. UI value list [0, 1, 3, 5, 7]. |

`forced_color` and `radar_range` are trait keys too (missing from the validator list above). The weapon value is the
choice index - 3 (-3 Random, -2 Unchanged, -1 Map default, matching halo3's special values).

UI-hidden flag (rec+0x8A; read by the client's option UI at 0xd02b9c, which then shows the option only when it is set;
the validator and apply ignore it, so the option is still accepted and written): `rounds.teams` in Capture the Flag,
VIP, Juggernaut, Territories, Assault and Infection (table 0x2351700); `primary_weapon`, `secondary_weapon`, `grenades`
in `red_powerup`, `blue_powerup`, `yellow_powerup`, `flag_carrier`, `ball_carrier`, `bomb_carrier`, `bomb_arming`,
`hill_occupant`, `last_survivor`, `safe_haven`; in `editor` everything except `resistance`, `recharge`, `shields`,
`camouflage`, `waypoint`, `radar`, `radar_range` (and the client-side camera/character keys).

Per-team characters: `teams.<color>_character_set` (label "<Color> team's character set", 0 Unchanged, 1 Vanilla
characters, 2 Random, 3..18 Customization mod 1..16) and `teams.<color>_character` ("<Color> team's character",
0 Players, 1 Random, 2..17 Character 1..16) for red, blue, green, orange, purple, gold, brown, pink; client-side.

### UNRESOLVED

- **Defaults**: the client's tables carry none. An option a playlist leaves out keeps the value of the variant halo3
  built for that engine before the options are applied (0x59d580 calls into halo3 first); those values are not
  pinned down here (`"default": null` in the JSON).
- **UI value list** (rec+0x50, `ui_values` in the JSON): `radar_range` [0, 1, 3, 5, 7], `grenade_regeneration`
  [0, 2, 1, 3]. Not read by the parser, validator, apply or read-back; its consumer was not found, so whether it is
  the UI's offered subset/order is unconfirmed.
- **`radar` aliases**: the client maps "Moving players" = 2, "All players" = 3, "Allies" = 4, while halo3's own
  setting table (halo3+0x90a310..0x90a370) names 1 off, 2 friends, 3 normal, 4 all, which matches the client's
  choice labels but not these aliases. Recorded exactly as the client has them.
- **Native meaning of `map.weapons` / `map.vehicles`**: the client writes the raw choice index (weapons 0 Default ..
  12 Random, 13 ODST weapons; vehicles 0 Default .. 9 All) into the i16 fields at v+0x150 / v+0x152. How halo3 maps
  these numbers (halo3-variant-traits.md has -1 = map default there) was not checked; the read-back check above only
  guarantees that the engine did not change the value (hosting fails otherwise). Seen live (Slayer, 2026-10-17): the
  running variant held -1 (0xFFFF) there, a value no client choice produces; the stats page shows any 16-bit option
  holding 0xFFFF without a matching choice as "Map default". Whether the client's choice 0 "Default" ends up as 0 or
  -1 was not checked.

## Graphics quality: Settings -> Advanced graphics (client v0.9.20; found 2026-10-18)

Where: `C:\ReclaimerProfile\settings.json` (the Mac path is `~/Library/Application Support/ReclaimerMac/prefix/drive_c/
ReclaimerProfile/settings.json`; a test profile via `RECLAIMER_PROFILE_DIR=C:\<folder>`). The client writes the block when
you press **Apply changes** (BACK discards unsaved changes) and applies it at once, in a running game too -- no restart.
Written by the client itself (Low preset):

```json
"quality": {
  "one": "low",
  "detail": { "decals": "low", "draw_distance": "low", "effects": "low", "level_of_detail": "low",
              "lighting": "low", "shadows": "low", "water": "low" }
}
```

- `one` = the one-player preset the menu shows; `detail` = the levels actually used (the menu's Shadows, Dynamic lights,
  Effects, Draw distance, Level of detail, Decals, Water). Up to 0.9.20 a file with `one` but no `detail` was refused
  ("one setting this build cannot use kept its default (quality.one)") and the client used High. **0.9.22 no longer reads
  `one`** (checked 2026-10-20, four start-ups with low/medium/high files): it prints that warning on every start, works the
  preset out from `detail` (a file with `one: "low"` and Medium detail starts as Medium) and never rewrites `one`, so the
  value left in the file can be stale. Only `detail` matters; Low / Medium / High all apply live and survive a restart.
- Levels: `auto`, `off`, `low`, `medium`, `high`, `ultra` (serde names in the client; "off" only where the menu offers it,
  e.g. Shadows). High is what Halo 3 drew before these settings, Medium the Xbox 360's own detail, Ultra goes past it.
- Other keys (defaults when absent): `split` (the split-screen preset, `auto`), `texture_filtering` (`original`, `4x`,
  `8x`, `16x`), `motion_blur` and `antialiasing` (true/false).
- What the engine gets (the client's log line "Graphics detail for 1 view(s): ..."):
  - High: 1500 shadow casters at 1.00, lights 1000+1000 at 1.00/1.00, effects 3.00, fades 8.00/3.00/3.00, models 3.00, decals 1.00, water 3.00.
  - Low: 2 shadow casters at 0.35, lights 1+3 at 0.50/0.50, effects 0.50, fades 0.25/0.50/0.50, models 0.50, decals 0.50, water 0.30.
- The log line "Graphics quality: one view High; split screen Auto; textures Original, motion blur on, antialiasing on."
  confirms what was read at start.

### How the client applies it (client 0.9.23; found 2026-10-25: re-tools/ripref.py <exe> 2b19e5c 2b19e98 lists the globals' users)

- `apply_settings(const Settings*)` (exe+0x91bb70; called by the Settings menu's Apply after saving settings.json, exe+0x5491c0,
  and once at start, exe+0x729cdb) borrows the struct and packs the levels into globals with `xchg`:
  Settings +0x1d3..+0x1d9 = one view, +0x1da..+0x1e0 = split screen, each byte one level in the order shadows, dynamic lights,
  effects, draw distance, level of detail, decals, water (0 auto, 1 off, 2 low, 3 medium, 4 high, 5 ultra; >= 5 -> 5)
  -> u64 at exe+0x2b19e90 (one view) and exe+0x2b19e60 (split); +0x1e1..+0x1e3 (texture filtering, motion blur, antialiasing)
  -> exe+0x2b19e88; then 1 -> exe+0x2b19e5c ("applied"). It logs "Graphics quality: ..." when any of them changed.
- halo3 gets its detail values from the client: the client writes `jmp [rip+0]` -> exe+0x8662d0 over a halo3 function (checked
  against its 14 prologue bytes first) that reads those globals every time halo3 asks (before any apply: the High preset) and
  logs "Graphics detail for N view(s): ..." when the result changes.
- The menu's presets come from exe+0x181a0d0 (preset in cl -> the packed 7 levels; registers only):
  Low 2,2,2,2,2,2,2 · Medium 3,3,3,3,3,4,3 (decals high) · High 4 x 7 · Ultra 5,5,5,4,5,4,4 (draw distance, decals, water high) ·
  preset 1 1,1,2,2,2,1,2 (off / low; the menu has no such preset) · 0 = all auto.
- The stats page's Quality (POST /control?do=quality&v=auto|low|medium|high|ultra) calls that preset function and swaps its result
  into the one-view global, as apply_settings does -- live in a match within frames, no menu. Not saved to settings.json and not
  shown in the client's own menu (its next Apply sets its values again). `PELOAD_QUALITY_MENU=1` = the old way, clicking through
  the client's Settings menu (it opens for a few seconds; the client saves the file).

Frame rate effect (fixed vista on Hugegrass, local server, same spot and view each run): see shim/docs/PERF-PLAN.md
"Advanced graphics settings".
