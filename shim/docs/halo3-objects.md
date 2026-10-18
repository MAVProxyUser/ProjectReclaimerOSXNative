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

| +0x13A | region permutations block reference {s16 size, s16 offset}: count = (size - (+0x4 bit 11 ? 0x20 : 0)) / 10; per region 8 bytes of state, then one permutation byte per region at offset + count*8 (indexes the model's Collision Regions[r].Permutations) | object_set_permutation (halo3+0x3457d8, +0x345d38) |

## Tags in memory
`object_set_permutation` also shows the engine's own tag lookup: tag table = `*(u64*)(halo3+0xA49018)`, 8-byte
entries, u32 address at +4; tag pointer = `*(u64*)(halo3+0x1FCF4C8)` + address*4. Object definition +0x40 = model
tag datum; model (hlmt) +0xA0 Collision Regions[0x14] { +4 collision region, +8 Permutations[8] { +5 collision
permutation } }.

## Units (biped / vehicle / giant)
| Offset | Field | Source |
|---|---|---|
| +0x174 | team index | `unit_get_team_index` |
| +0x24E | seat index (s16, -1 = not in a vehicle) | `unit_in_vehicle` (halo3+0x3a36f4) |
| +0x262 | current weapon slot (s8) | `unit_get_primary_weapon` |
| +0x268 | weapon object handles [4] | halo3+0x356388 |
| +0x294 | grenade counts [4] (s8: frag, plasma, spike, firebomb) | `unit_get_total_grenade_count` |
| +0x360 | active camouflage amount (float; > 0 = camo active) | `player_active_camouflage_on` |
| +0x1A0, +0x1AC, +0x1C4, +0x1D0 | aiming / looking vectors (unit vectors; all equal the camera forward for the local player in 100% of samples over a 15 degree pitch sweep) | recorded live (PELOAD_HB_SELFDUMP) |
| +0x384 | crouch amount (float: 1.0 crouched, 0 standing) | recorded live: 1.0 exactly while crouch was held |

Verified live with a scripted self-grenade (2026-10-16): grenade counts 2 -> 1 -> 0 as thrown; shield 1.0 -> 0 and
health 1.0 -> 0.82 from the blast; after respawning, a new unit (handle changes in the player record) with full
health/shield and 2 grenades.

## Players
Data array at TLS block +0x10, element 0x2F8 bytes (Slayer, 2 slots in a 2-player game).
| Offset | Field | Source |
|---|---|---|
| +0x28 | unit handle (changes on respawn) | `player_active_camouflage_on`; recorded |
| +0x4C | position (3 floats) | recorded |
| +0x58 | nickname (UTF-16) | recorded ("Shiz") |
| +0x294 | increments on respawn (lives used / spawn count) | recorded |
Kills / deaths / score: not in the player record (no field changed on a suicide except the respawn ones); the
scoreboard numbers are presumably in the game engine's statistics -- not located yet.

## Projectiles
| Offset | Field | Source |
|---|---|---|
| +0x1b0 | distance travelled | bullet datums at deletion |
| +0x1dc | speed | same |
| +0x1e8 | start position (where it was fired from) | same |
| +0xC0 | owner unit handle (the shooter) | bullet datum = the firing player's unit (player record +0x28) |

Most bullets (e.g. the assault rifle's) are created and deleted inside one engine tick, so they never appear in
the object table between frames. `object_delete` (halo3+0x342ed8, handle in ecx) sees them: the loader hooks its
entry (`hbdel_install`) and records projectile datums; at deletion +0x1c is the impact point.

## Still to confirm
Kills / deaths / score (game engine statistics), objective state (flags, territories, hills; needs an objective
game variant to observe).

## Vehicle weak spots: the model's damage info (2026-10-17, backlog 31)

Static, from the .map (re-tools/h3dmg.py <map> prints it; the overlay's hbg_weak in shim/src/hitbox.c uses the same rule).
Layouts: Assembly Halo3MCC hlmt.xml / coll.xml.

- object def (vehi/bipd) +0x34 model tagref -> hlmt. hlmt +0x7C Materials [0x14] (+0 name sid, +6 s16 damage section),
  +0x88 New Damage Info [0xA4], +0x94 Targets [0x20] (aim/lock-on markers: +0xC section, +0x18 flags bit 2 headshot,
  bit 3 vulnerable), +0xA0 Collision Regions.
- New Damage Info: +0xC body vitality, +0x20 shield vitality, +0x68 Damage Sections [0x2C] (+0 name, +4 flags: bit 0
  absorbs body damage, 7 headshotable, 8 ignores shields; +8 share of body vitality; +0xC Instant Responses [0x88]:
  +8 flags (bit 0 kills, 7 destroys, 16 kills except solo, 23 only on special death), +0xC threshold, +0x40 region sid,
  +0x44 new state 0-4, +0x78 ejected seat), +0x8C Damage Seats [0x20] (+0 seat label, +4 direct damage scale = share of a
  hit on the rider that reaches the rider, +0x14 region-specific [0x2C]: +0 region, +8/+0x14/+0x20 direct scale at
  minor/medium/major damage).
- A hit's section: collision surface (coll region/permutation/bsp +0x40 surfaces [0xC], material s16 at +4) -> coll +0x14
  materials (name sid) -> the hlmt material with the same name -> its section. Section damage also counts against the body
  unless the section absorbs it; parts (fenders, wings, rudders) only change damage states.
- String ids in these builds: 19-bit index, namespace at bit 19; namespace-0 indices past namespace 0's count continue
  after all namespaced strings (re-tools/h3bsp.sid_name; h3map.H3Map.sid gets the second part wrong).

What the stock vehicles have (Sandbox, every MP vehicle): Wraith back hatch / rear engine section = 10% (75 of 750): its
response KILLS the Wraith. Wraith cockpit (15%, 113) and Scorpion driver hatch (15%, 113): at "major" the driver's direct
damage scale goes 0 -> 1. Other vehicles have no lethal section; damage anywhere drains the same body pool. Riders:
Mongoose / Chopper driver 1.0, Warthog / Ghost / Mauler / gunners 0.9 (Ghost 1.0 once its hull is damaged), Banshee /
Hornet pilot 0.2-0.5 by hull state, boarding seats 1.0. The Ghost has a 5% (20) gas_tank section that kills it, but no
collision material feeds it.

![Wraith / Scorpion / Ghost collision models, weak spots colored](img/vehicle-weak-spots.png)
(top and side views from the .map's collision models; yellow = lethal section, orange = opens the driver, blue = rest)
