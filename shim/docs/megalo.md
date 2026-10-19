# Megalo in the Reclaimer client: what it is, and the lab (backlog 72)

The client (v0.9.20) carries its own game-mode scripting, in three layers, all Rust:

1. **`script` version 1** in a custom-game preset: a JSON op-list per event (`initial`, `on_spawn`, `on_kill`, `on_death`),
   ops such as `weapon`, `score`, `set` with `when` conditions (Gun Game, Growth, One in the Chamber).
2. **`script` version 2 `logic`**: JSON structures -- `options` (value / min / max), `megalo_traits`, `deployment` (classes,
   costs: UNSC vs Covenant), `globals`, `functions`, `team_initial`, event op-lists (`on_round_start`, `on_round_end`,
   `on_join`, `on_leave`, `on_tick`), `hud_layout`, `scripted_round_end`, `seed`.
3. **`logic.megalo`**: Megalo SOURCE in the Halo: Reach dialect ReachVariantTool writes (`alias`, `on init: do ... end`,
   `for each player do`, `function`, `global.number[0]`, `current_player.biped`, `place_at_me`, ...), compiled by the client
   (`custom_modes/megalo/syntax.rs`) and run by its VM (`.../megalo/vm.rs`) through an adapter onto halo3
   (`.../logic/megalo_adapter.rs`). A compiled preset stores the SOURCE, so loading one compiles it again -- a hand-edited
   preset cannot hand the VM bytecode the compiler would refuse.

Reclaimer extensions beyond Reach's Megalo (seen in `megalo-check` capability lists): `player.set_biped`,
`player.get_infection_death_count`, `player.deploy_class`, `player.get_deployment_class / _points`,
`player.set_deployment_points`, `player.set_ui_menu`, `player.set_vehicle_use`, `player.set_turret_use`,
`player.set_max_health / _shields`, `game.get_deployment_cost / _limit / _occupied`, `event.widget click`,
`created_label:<name>`, `spawn:<object>`, `option:N`, `traits:N`. The full API table: section "API" below (static RE).

## The lab: the client's own tools, no MCC, no game

The client has three subcommands that reach the compiler without MCC; under the loader each takes ~0.07 s:

```bash
python3 shim/tools/megalo-lab/megalo_fuzz.py corpus     # the example scripts / presets inside the client ->
                                                       # ~/Library/Application Support/ReclaimerMac/megalo-corpus (not in git)
python3 shim/tools/megalo-lab/megalo_fuzz.py run megalo-check <file.megalo>        # parse + type-check + capabilities
python3 shim/tools/megalo-lab/megalo_fuzz.py run mode-compile --template <preset.json> --output <out.json> <file.megalo>
python3 shim/tools/megalo-lab/megalo_fuzz.py run mode-check <preset.json>          # validate a preset (all three layers)
python3 shim/tools/megalo-lab/megalo_fuzz.py fuzz --n 5000 --jobs 4               # mutate Megalo sources
python3 shim/tools/megalo-lab/megalo_fuzz.py json --n 5000 --jobs 4               # mutate presets
```

Verdicts: OK, ERR (refused with a message), PANIC (Rust panic = the client aborts), CRASH (a fault the loader saw),
HANG (timeout). Kept inputs + outputs go to `--out`; `errors.txt` there lists every distinct refusal (the checker's rules).

## Results so far (2026-10-19)

- 1,500 mutated Megalo sources (`megalo-check` / `mode-compile`) and 1,500 mutated presets (`mode-check`): no panic, no
  crash, no hang. 137 + 178 distinct refusals.
- What the compiler enforces (from the refusals): expression nesting and token-count limits, a source size limit (KiB),
  integer literals within signed 32-bit, Reach's variable capacity per kind (`Global.Number[N] exceeds Reach variable
  capacity`), indices only integer constants or aliases ("An index must be an integer constant or alias", "cannot be
  negative") -- no index computed at run time --, typed assignments and comparisons ("Cannot assign Variable(Number) to
  Variable(Player)", "Cannot compare different reference types"), argument counts per function, "Unsupported Megalo
  API: Variable(Object).set_rate", unknown properties, and the adapter's capability list ("Halo 3 Megalo adapter does not
  implement: ...").
- Presets: serde with typed fields and unknown fields refused ("expected struct Program", "expected u32"): no JSON-level
  type confusion.

- 32 targeted cases (the static RE's list, below) through `mode-compile` (lexer, parser, types, the adapter gate): no
  panic. Limits hold exactly: nesting 32, 4096 statements / block, 32768 tokens, 64 KiB source, signed 32-bit literals,
  labels 1..32 CHARACTERS (multibyte UTF-8 safe), timer rates constant; hidden events and client-local variables refused.

## API (static RE of client v0.9.20; exe RVAs)

Pipeline: file reader 0x180d180 (<= 0x20000 bytes, UTF-8) -> lexer 0x17e1a70 -> parser 0x17e5d00 / 0x17eede0 -> typing
0x17ed510 -> resolvers 0x17d2480 (identifiers / variables) and 0x17d3cc0 (receiver x method -> arity, argument types) ->
capabilities 0x17e3cf0.. -> **Halo 3 adapter gate 0x17fc460** (every preset load; `megalo-check` skips it) -> validator
0x17df100 -> VM 0x17ca8e0 (tick) / 0x17cca80 (statements) / 0x17cffb0 (expressions) -> adapter 0x16899b0 / 0x1698610 /
0x1687950 / 0x1686a70 -> world-command queue 0x16897c0 (<= 256 a tick) -> native applier 0x68d860 (mode_runtime/world.rs).

Three gates: the compile-time resolver ("Unsupported Megalo API", arity, argument types), the adapter gate ("Halo 3 Megalo
adapter does not implement: ..."), and the run-time adapter ("Unsupported native operation") -- any error stops the mode
("Custom game mode stopped: ..."). "Hidden" = known to the compiler, refused by the adapter gate.

- **Language**: `alias`, `alias x = allocate <scope>.<kind>`, `enum`, `declare <var> [with network priority low|high|local]`,
  `team_count N` (2..8), `function f()` (no arguments), `on <event>: do`, `for each player [randomly] / team / object [with
  label]`, `if / altif / alt`, `do`, assignments `= += -= *= /= %= &= |= ^= ~= <<= >>=`, `and or not`, 32-bit literals
  (`0x`, `0b`), `%` rates. Limits: expression and block nesting 32, 4096 statements, 16 arguments, constant indices only, two
  indexed levels.
- **Events**: `init`, `widget click`, `button press`. Hidden: `local`, `local init`, `host migration`, `double host
  migration`, `object death`, `pregame` (client-local variables: "require the client Megalo runtime, which is not implemented").
- **Variables** (global / player / object / team / temporaries): number 12/8/8/8/10, object 16/4/4/6/8, player 8/4/4/4/3, team
  8/4/2/4/6, timer 8/4/4/4/0. Identifiers: `current_player / object / team`, `game`, `team[N]`, `script_widget[N]`,
  `script_traits[N]`, `script_option[N]` (read-only), `button.<name>`, `clicked_widget`, `pressed_button`; hidden (object
  death only): `killer_player`, `killed_object`, `killer_object`, `death_event_damage_type`.
- **Properties**: biped, score, team, health, shields, max_health, max_shields, spawn_sequence (hidden: rating,
  plasma_grenades, frag_grenades, money). Writable: object health / shields / max_*, player score / team.
- **game**: rand, abs, end_round, set_ai_team, get_game_type, get_territory_count; Reclaimer: get_deployment_cost / _team /
  _limit / _occupied / _class_count. **timer**: reset, is_zero, set_rate (constant %).
- **widget** (Reclaimer): reset, set_text / _for, set_value_text / _for (format codes `%n`, `%%` only, up to 3 values),
  set_visibility, set_position, set_size, set_anchor, set_alignment, set_font_size (8..72), set_color / background / border
  (0..255), set_clickable, set_meter_for, set_meter_params.
- **object**: delete, detach, kill, move_to, set_velocity, get_speed, set_scale (10..300%), attach_to (offsets -128..127,
  depth <= 32), copy_rotation_from, place_at_me (constant type / label / flags / variant, offsets -128..127), get_carrier,
  get_distance_to, shape_contains, has_forge_label, is_of_type, set_hidden, set_invincibility, set_turret_use,
  set_vehicle_use, set_garbage_collection_disabled. Hidden: push_upward, set_shape_visibility, set_waypoint_priority /
  _visibility / _icon.
- **player**: set_biped, apply_traits, refill_ammo, give_equipment, get_kill_count, get_death_count,
  get_infection_death_count, get_respawn_time, is_not_respawning, is_button_held, set_camera_mode / get_camera_mode,
  set_ui_menu / is_ui_menu_open, set_max_health / _shields, set_turret_use, set_vehicle_use, deploy_class,
  get_deployment_class, get_deployment_points, set_deployment_points (0..100000), set_primary_respawn_object. Hidden:
  is_spartan, is_elite, is_monitor, get_weapon, get_vehicle, get_fireteam, set_fireteam, get_spree_count, killer_type_is,
  get_death_damage_type / _mod, get_crosshair_target, get_armor_ability, get_scoreboard_pos, force_into_vehicle,
  set_co_op_spawning, set_loadout_palette, set_respawn_vehicle, set_vehicle_spawning_enabled.
- **team**: set_name, set_color, end_round, get_native_score, get_territory_count, has_alliance_status, has_any_players.
- **Constant tables**: 10 buttons, 11 equipment, 13 vehicles, 27 weapons, 34 character bodies (fixed tag paths), 17 traits
  (each 0..max, <= 16 trait sets). Every other Reach call (debug_print, sounds, incidents, film markers, waypoint text /
  timer / range, ...) is unknown. **No call takes a file path, a network target, an address or an engine global**; the only
  debug hooks are environment variables (`RECLAIMER_SCRIPT_TRACE` logs world operations, `RECLAIMER_CUSTOM_GAME`).
- **JSON op-lists** (logic.rs, separate): 31 ops (score, team, end_round, health, for_each_object, with_object, set_object,
  spawn_object, delete_object, move_object, object_scale, teleport, traits, hud, if, for_each_player, for_each_team,
  with_player, call, set_global, set_team, timer_set, timer_rate, random, set, scale, damage, lethal, ammo, add_ammo,
  weapon); expressions in reverse Polish, 1..64 tokens, numbers within +-1e6; executor depth 16, budget 0x4000.

## VM (static RE)

Tree-walking interpreter (no bytecode); values are a tagged enum (number = i32, timer = f64, entity = kind + slot + 64-bit
identity, error); variables in a map keyed by (owner, kind, index) -- no array indexing, owners re-validated on every
access, dead entities' variables dropped each tick. Budget 65,536 steps per invocation ("Megalo execution budget exhausted"),
depth 64, tick delta finite and 0..10 s. Integers wrap; shifts masked to 0..31; / and % by 0 are errors; INT_MIN / -1 and
% -1 special-cased; `rand(N<=0)` handled. Timers: / 0 an error, non-finite = "timer arithmetic overflow", negatives clamp.
Native effects: 0x58-byte world commands (14 kinds), each re-verified with halo3's own handle check, under the game-state
lock; positions within +-100000 (beyond the engine's 32768, where the host deletes units), velocity finite, <= 256 live
created objects, <= 960 scale overrides.

**Where it runs**: only on the host (a local game, a host, a dedicated worker); a joined client only forwards button presses.
A joining client DOES receive the server's preset (join ticket exe+0x82a440 -> 0x625390) and compiles / validates it -- a
compiler panic there would crash the client (none found); a malicious server cannot run script on your client. Clients
render the HUD state the server computes (HudChange / HudWidget / WidgetStyle) -- not yet checked for validation.

## Still to do

- Runtime cases on a local dedicated server with a scripted preset (once no one is playing): `set_biped` to a new body and
  deleting the old one in the same tick (the Flood script does that), attach chains 32/33 and cycles, 257 world commands,
  256+ place_at_me, move_to +-99999 (beyond 32768), deleting objects inside `for each object`, the budget edge (~65k steps a
  tick), arithmetic edges, op-list validator vs run-time evaluator mismatches (~38 panic sites in 0x17c7910 / 0x17c38f0:
  v2 tokens in v1 scripts, `team.8`, `self.16`, unknown `call`, `random` min > max, `timer_set` slot 8), server HUD -> client
  (4 KB widget text, many `%n`), the logic `seed` (xorshift32 with no zero guard: a fixed / zero seed makes `rand` predictable).
