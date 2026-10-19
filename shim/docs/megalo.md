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

## Still to do

- The runtime: arithmetic overflow / division by zero, API calls with run-time values of the wrong object kind
  (`set_biped` with a non-biped, `place_at_me` with odd arguments, `rand(0)`, negative timers), deep `for each` loops and
  per-tick budgets -- on a local dedicated server with a scripted preset, once no one is playing.
- The static RE of the full API table and the VM / adapter (in progress).
