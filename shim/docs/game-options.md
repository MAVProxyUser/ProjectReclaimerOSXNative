# Project Reclaimer game options (v0.9.18)

Options that a dedicated-server playlist game can set, as `"options": { "<key>": <value> }` (see the
playlist in `C:\ReclaimerTestServer\playlists\betrayal.playlist.json`). Every key and value below was checked
against the client's own validator (`dedicated --config <toml> check`), which names an unknown key ("Did you
mean ...?"), lists the allowed choices for a wrong value and prints numeric ranges. Choice values are strings
(`"On"`, `"Assault rifles"`, ...; case-insensitive). Numbers are JSON numbers (a quoted number is rejected).

Helper: `re-tools`-style probe in the session scratchpad (`optcheck.py <game type> <value> <key,key,...>`) builds
one playlist game per key and parses the validator's messages.

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
(the `key` + label pairs in `src/game_options.rs`'s strings), not yet run through the validator. Keys that several
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
read from halo3's own copy of the variant (shim/docs/halo3-variant-traits.md), using these key names.
