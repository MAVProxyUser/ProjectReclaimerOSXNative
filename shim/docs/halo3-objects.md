# halo3.dll (MCC) runtime object layout

Found 2026-10-16 from the evaluate functions of halo3's own HaloScript getters (`re-tools/hsfuncs.py` lists a
script function's table entry: return type, name, parse and evaluate functions, parameter types) and from
datums observed live. Offsets are from the object datum (object table entry +0x10).

## Finding objects
- Static-TLS index of halo3: `halo3+0xa39f9c`; thread-local block = `gs:[0x58][index]`.
- Object table: data array "object" at TLS block +0x38 (header: +0x20 max, +0x24 element size 0x18,
  +0x2c 'd@t@', +0x3c high-water, +0x48 data). Entry (0x18 B): +0x00 salt (0 = free), +0x03 type,
  +0x10 datum pointer. Handle = salt << 16 | index.
- Players: data array at TLS block +0x10 (`player_active_camouflage_on` iterates it); player datum +0x28 = the
  player's unit handle.
- Types: 0 biped, 1 vehicle, 2 weapon, 3 equipment, 5 projectile, 6 scenery, 10 crate, 11 creature, 12 giant,
  13 effect scenery.

## Every object
| Offset | Field | Source |
|---|---|---|
| +0x00 | definition tag (datum index) | |
| +0x10 | parent object handle (the vehicle when seated, the holder for a held weapon) | `unit_in_vehicle` |
| +0x1c | bounding-sphere centre; +0x28 radius | observed |
| +0x50 | origin (feet for bipeds) | observed |
| +0x5c / +0x68 | forward / up vectors | `object_set_velocity` |
| +0x74 / +0x80 | velocity / angular velocity | object_set_velocities (halo3+0x341110) |
| +0xF4 | health (body vitality, 1.0 = full) | `unit_get_health`, `object_get_health` |
| +0xF8 | shield vitality (1.0 = full, more with overshield) | `unit_get_shield` |
| +0x110 | flags; bit 2 set -> health and shield read as 0 (dead) | same getters |
| +0x136 | node matrices block reference {u16 size, u16 offset}; real_matrix4x3 (scale, fwd, left, up, pos) per node | observed |

## Units (biped / vehicle / giant)
| Offset | Field | Source |
|---|---|---|
| +0x174 | team index | `unit_get_team_index` |
| +0x24E | seat index (s16, -1 = not in a vehicle) | `unit_in_vehicle` (halo3+0x3a36f4) |
| +0x262 | current weapon slot (s8) | `unit_get_primary_weapon` |
| +0x268 | weapon object handles [4] | halo3+0x356388 |
| +0x294 | grenade counts [4] (s8: frag, plasma, spike, firebomb) | `unit_get_total_grenade_count` |
| +0x360 | active camouflage amount (float; > 0 = camo active) | `player_active_camouflage_on` |

## Projectiles
| Offset | Field | Source |
|---|---|---|
| +0x1b0 | distance travelled | bullet datums at deletion |
| +0x1dc | speed | same |
| +0x1e8 | start position (where it was fired from) | same |

Most bullets (e.g. the assault rifle's) are created and deleted inside one engine tick, so they never appear in
the object table between frames. `object_delete` (halo3+0x342ed8, handle in ecx) sees them: the loader hooks its
entry (`hbdel_install`) and records projectile datums; at deletion +0x1c is the impact point.

## Still to confirm
Aim direction, crouch / movement state, player names and scores, objective state (flags, territories).
