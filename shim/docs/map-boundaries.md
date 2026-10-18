# Map boundaries (Halo 3 MCC): what keeps players in, and how to switch it off

Static RE only, 2026-10-17. Sources: the .map files, using the Assembly Halo3MCC plugins for layouts, and
`halo3.dll` (base 0x180000000; addresses are `halo3+RVA`), read with capstone through `re-tools/hdis.py`.
Nothing was read from a running process.

- **CONFIRMED** = read from the code, with the evidence given.
- **GUESSED** = the name or meaning is inferred.
- TLS block `tls = gs:[0x58][*(u32*)(halo3+0xa39f9c)]`. Objects are at `tls+0x38` and players at `tls+0x10`, as in teleport.md.
- The scenario tag pointer is `*(halo3+0xa46a08)`. The globals (matg) pointer is `*(halo3+0xa46968)`.
- Tag addresses are 32-bit and expand as `*(halo3+0x1fcf4c8) + v*4`.

## 1. What a Halo 3 map uses to keep players in

| Mechanism | Where it lives | Effect on a player | Valhalla (riverworld) |
|---|---|---|---|
| **Kill trigger volume** | scnr +0x258 trigger volumes, listed in scnr +0x484 "Scenario Kill Triggers" (s16 each; a volume's +0x40 holds its index in that list) | Instant death: the matg falling-damage "Distance Damage" effect is applied (section 2a) | `kill_happy_place`: x -22.0..123.7, y -52.0..42.9, z -10.4..-4.8. This is the lake/river below the waterfall, north of the base |
| **Safe zone** | Trigger volumes listed in scnr +0x490 "Scenario Safe Zone Triggers" | **Being outside every safe zone kills you**, through the same code path as a kill volume | `safe_zone`: x 38.1..106.3, y -167.8..-31.9, z -1.9..26.2. **This box is Valhalla's real boundary**: it wraps the valley up to z 26 |
| **Soft ceiling, type 1 "soft kill"** | Scenario-wide: scnr +0x234 (flags, name, type). Geometry: structure_design (sddt, scnr BSP entry +0x10) +0x10, triangles | Touching it for 3 s kills you (`unit_kill`). The timer restarts if contact lapses for more than 6 s (2b) | `th`: 2645 triangles draped over all the out-of-bounds terrain, x -38..206, y -285..90, z -9..93. It ignores vehicles (flag 2), but a player in a vehicle is still tested through the vehicle |
| **Soft ceiling, type 0 "acceleration"** | Same | Pushes you back by changing the velocity of the biped, or of its vehicle | `loki`: 506 triangles, a lid at about z 13 over the valley (x 39.5..110.7, y -165..-38, z -4.2..20.5) |
| **Soft ceiling, type 2 "slip surface"** | Same | Slippery: sets biped +0x547 bit 1, so you can't stand there | `slidey`: 2117 triangles on cliff tops and boulders |
| **Invisible collision ("player clip")** | Collision BSP surface flag 2, both in the structure BSP and in instanced-geometry definitions | Solid for bipeds and vehicles; bullets, line-of-sight and other vector tests pass through it (2c) | 1058 surfaces (206 of them near-vertical walls): 484 in the structure BSP (flags 3 and 67) plus `base_int_thing00..29` and `waterfall_collision`. The walls run along the west base side and the east cliff |
| Structure BSP world bounds | sbsp +0x40 (x, y, z ranges) | Nothing directly. Outside the BSP clusters an object is only disconnected from the map (2d) | x -40.9..206.5, y -285.4..92.5, z -15.5..1892.3 |
| Engine coordinate limit | `object_set_position` | A teleport or spawn past \|c\| > 32768 fails, and the host deletes the object (2d) | Far away (the map spans about ±300) |

Trigger volume layout (0x44 bytes):

| Offset | Field |
|---|---|
| +0 | name sid |
| +0xC | forward |
| +0x18 | up |
| +0x24 | position, which is a **corner** of the box |
| +0x30 | extents |
| +0x3C | half the diagonal (checked) |
| +0x40 | s16 kill-trigger index |

The box is `pos + a*fwd + b*left + c*up` with a, b, c in [0, ext], where `left = up x fwd`.

### Tool: `re-tools/h3bounds.py`

```bash
python3 re-tools/h3bsp.py   "$MAPS/riverworld.map" out/riverworld.png                 # base picture + grid json
python3 re-tools/h3bounds.py "$MAPS/riverworld.map" out/rw_bounds.png --grid out/riverworld.json \
        [--composite out/rw_comp.png] [--no-triangles]
# without --grid it runs h3bsp.py itself first (writes <out-stem>_base.png/.json) and uses that grid
```

- **out.png** is a transparent RGBA overlay on exactly the h3bsp pixel grid (`px = (x-x0)*ppu`, `py = (y1-y)*ppu`), so the stats page can stack it on the map picture. Colours:

  | Colour | Shows |
  |---|---|
  | Red fill / edges | Soft kill |
  | Orange | Acceleration |
  | Yellow | Slip |
  | Cyan | Invisible collision: a fill for sloped/flat surfaces, 2 px lines for walls |
  | Red box + hatch | Kill volumes |
  | Green dashed | Safe zones |
  | Grey | Other trigger volumes |
  | White | BSP world bounds |

  A legend is included.
- **out.json** has these keys:
  - `grid`, `engine_limit`, `world_bounds`
  - `kill_volumes` and `safe_zones`: aabb, center, z range
  - `trigger_volumes`: all of them, with axes, extents, footprint polygon and 8 corners
  - `soft_ceilings`: type, flags, ignores, bbox, triangles
  - `invisible_surfaces`: world-space polygons with source, flags, material and a vertical flag
- It takes about 0.5 s per map.
- Checked by eye on Valhalla: the safe zone wraps the valley, the soft-kill drape covers everything around it, the acceleration lid sits over the valley, and the invisible walls line the base side and the east cliff.

Survey of all 27 MP maps:

| Finding | Maps |
|---|---|
| **No kill volume** | chillout, fortress, ghosttown, isolation, midship, s3d_turf, shrine (Sandtrap), warehouse (Foundry) |
| Exactly one safe zone | Every map except warehouse and zanzibar, which have two |
| Soft kill ignores vehicles (flag 0x2) | Every map with a soft kill; most are named `th` |
| Kill volumes above ground | Sandbox `kill` (z 35..37), Chill `kill_zone1/2` (z 42..56) |
| `playerkill`-named volumes | None anywhere |

More map details:
- **Guardian**:
  - `kill_drop` is under the whole map (z -0.9..13.6).
  - Acceleration ceiling `noouts`.
  - 792 invisible surfaces.
- **Sandtrap**:
  - Acceleration `behemoth` ignores everything except huge vehicles (the Elephant fence).
  - Soft kill `th`.
- **Construct**:
  - `kill_bottom`, plus tilted 1-unit boxes `kill_strut0/1`.

## 2. halo3.dll: where each one is enforced, and how to switch it off

All of this runs in **`biped_update` = `halo3+0x37b6c8`**. That is the slot at the biped object-type definition
`halo3+0x8b2650` +0x88, so it is a pointer at `halo3+0x8b26d8`; the type table is `halo3+0x815160`. CONFIRMED.
`creature_update` `halo3+0x3a4fb0` is the same slot of the creature type `0x8b0fd0`.

These run on the simulation (main) thread, inside the per-object update of each game tick, under the game-state lock.
The kill decisions are made by the **host only**:
- Kill volumes: the damage branch returns when `*(u8*)(gg+0x11) == 4` (distributed client).
- Soft kill: it only sets the unit's kill request, which is host-authoritative.

So the kill/safe-zone patches are needed **in the host process** (listen host or dedicated worker). For the
acceleration and slip soft ceilings, clients also simulate them on their own predicted biped. Patch the joining
clients too, or a remote player gets pulled back by its own prediction until the host's position wins. That last
point is GUESSED from how prediction works; see teleport.md section 3.

### 2a. Kill volumes and safe zones: CONFIRMED

| Address | What | Evidence |
|---|---|---|
| `0x1b8e00 (u32 object, u8* out_in_safe_zone)` | Test one object | One **kill** volume per call, round-robin: `k = (u16(object) + *(u32*)(*(tls+0xc8)+0xc)) % kill_count`, where `+0xc` is the game tick (GUESSED name). Skipped if bit k of the disabled bitvector `*(tls+0x418)` is set; otherwise `0x16f644(tv, object)`. Then, if `out != NULL`: `*out = 1`. On ticks where `(u16(object) + tick) % safe_count == 0`, `*out = 0` and every safe zone is tested; inside any one gives `*out = 1`. Returns 1 if inside the kill volume |
| `0x16f644 (s16 trigger_volume, u32 object)` | trigger_volume_test_object | Tests the object's bounding-sphere centre (datum +0x1c) with `0x16f55c`. The HS functions `volume_test_object(s)` use it too |
| `0x37b80a..0x37b838` (biped_update) | Caller | It skips the whole test when the object is at rest (datum +0xa8 bit 9). Otherwise it calls `0x1b8e00(unit, &safe)` and goes to the **kill path when `in_kill || safe == 0`** |
| `0x37b83e..0x37b95f` | Kill path | Sets `biped +0x460 \|= 0x10`; returns on clients (gg+0x11 == 4). Then `0x36eba0(&dmg, matg->falling_damage[0] +0x5C "Distance Damage")`, `dmg.flags \|= 4`, `0x36fe00(&dmg, unit, -1, -1, -1, 0)` (object_cause_damage, GUESSED name) |
| `0x3a50c3..0x3a50d0` (creature_update) | Creatures | `0x1b8e00(creature, NULL)`; when inside it sets `creature +0x110 \|= 0x40` (the unit_kill request) |
| `0x1e2390` / `0x1e22fc` | HS `kill_volume_disable` / `kill_volume_enable` | Set or clear bit `tv[+0x40]` in `*(tls+0x418)`. The bitvector is 0x80 bytes (1024 bits); `0x1b8d9c` zeroes it at map load |

Vehicles are not tested themselves (no caller passes a vehicle). A seated player still is, through biped_update.
The test uses the biped's position, so a driver in a kill box dies. GUESSED: that this applies while seated (no
early-out seen except "at rest").

**Patches** (PELOAD_PATCH is re-applied when halo3.dll is mapped: peload.c patch_install). Check the original bytes first.
The Reclaimer client refuses an entry patch of `0x1b8e00` ("Tested live" below): use the call-site rows.

| Goal | Patch | Original bytes | Notes |
|---|---|---|---|
| **No kill-volume or safe-zone deaths, for any object** (recommended) | `halo3+1b8e00:31C04885D27403C60201C3` | `48 8B C4 48 89 58 08 48 89 68 10` | The entry becomes `xor eax,eax; test rdx,rdx; je ret; mov byte [rdx],1; ret`, meaning "in no kill volume, inside a safe zone". Only biped_update and creature_update call it. HS `volume_test_*` scripts are unaffected |
| Same, bipeds only | `halo3+37b816:E95501000090` | `0F 85 54 01 00 00` | `jne 0x37b970` (the at-rest skip) becomes `jmp`, so bipeds never run the test, exactly like an at-rest biped |
| Kill volumes only (keep safe zones) | `halo3+37b82f:9090` | `75 0D` | Drops `jne kill` after the kill-volume result |
| Safe zones only (keep kill volumes) | `halo3+37b838:E92701000090` | `0F 85 26 01 00 00` | Always skip when not in a kill volume |
| Data-only alternative (kill volumes only) | Write 0xFF to the 0x80 bytes at `*(tls+0x418)` on the sim thread | | This is what `kill_volume_disable` does for every volume. Redo it after every map load. It does not cover safe zones |

### 2b. Soft ceilings: soft kill, acceleration, slip. CONFIRMED (the type to flag-bit mapping is GUESSED)

| Address | What |
|---|---|
| `0x37b72a..0x37b76b` (biped_update) | Only if `unit+0x178` (owning player) != -1 and `!0x36d520(...)`: `0x1bac00(unit, &player[+0x4c])`. The result goes to the frame byte `[rbp+0xc0]` ("pushed back") |
| `0x1bac00 (unit, pos*)` | Player soft-ceiling response. It works on the unit's ultimate parent (the vehicle when seated). Skipped if `unit+0x110` bit 2 is set. It queries `0x1bb1d0`, then: |
| `0x1baf0f` | Acceleration: if there is a hit, `0x1ba2e8` gives the penetration and the reflected velocity, `0x3411a4` sets it (object_set_velocities), and `parent+0xa8` bit 19 is set |
| `0x1bb0e0..0x1bb15f` | **Soft kill** (result flag 2). `unit+0x404` = contact start tick. If -1 or more than 6 s ago (`0x847254` = 6.0), start = now. Else, once at least 3 s (`0x847180` = 3.0) have passed, call **`unit_kill` `0x34d520`** (sets `+0x110 \|= 0x40`; the same function as HS `unit_kill`) |
| `0x1bb166..0x1bb185` | Slip (result flag 4): when biped `+0x4de == 1`, set `+0x547 \|= 2` |
| `0x1bb1d0` | Query: MOPP query (`0x449590`) over the active BSP's structure_design soft-ceiling triangles. Enabled ceilings come from `0x1bb844(name)`. Ignore flags: biped bit 0, vehicle bit 1, huge-vehicle bit 3. Soft kill applies to object types in mask 0x1003 (biped, vehicle, giant) |
| `0x1bb784` / `0x1bb844` | `soft_ceiling_enable` (HS eval `0x1e2ac4`) / is-enabled. Bit i of `*(tls+0x518)` = scnr soft ceiling i enabled |
| `soft_ceilings_disable` | The HS global exists, but its storage pointer is NULL in this release build (entry `halo3+0x8a7770`), so it does nothing |

Other users of the soft-ceiling query, not traced:
- `0x1b9474`, called from `0x213718` / `0x213904` (GUESSED: havok vehicles / huge vehicles).
- `0x1b9af4`, called from `0x1fe800` (GUESSED: camera).
- `0x1b9d6c`, called from `0x117a24`.

The player path above covers walking players and players driving vehicles.

| Goal | Patch | Original bytes |
|---|---|---|
| **No soft ceilings at all for players** (acceleration lid, soft kill, slip) | `halo3+37b734:EB` | `74` (`je 0x37b777` becomes `jmp`; this is the engine's own non-player path) |
| Soft kill only (keep the acceleration lid and slip) | `halo3+1bb0ee:EB` | `74` (`je 0x1bb161` becomes `jmp`; flag-2 handling skipped) |
| Acceleration push-back only | `halo3+1baf0f:E9AB01000090` | `0F 8E AA 01 00 00` |
| Data-only alternative | Clear the bits in `*(tls+0x518)` (one per scnr soft ceiling) after map load, which is what `soft_ceiling_enable <name> false` does | |

**Explore-everything recipe (host):**

```
PELOAD_PATCH="halo3+1b8e00:31C04885D27403C60201C3,halo3+37b734:EB"
```

To also cover joining clients' prediction, run their clients with the soft-ceiling patch (`halo3+37b734:EB`).
Live result ("Tested live" below): the client checks the ENTRY bytes of some halo3 functions ("Native function contract
mismatch at 0x1801b8e00", the worker exits) -- so this recipe's first patch is refused; the call-site patches
`37b816` / `37b734` inside biped_update are accepted. It also checks halo3's ws2 imports (CLAUDE.md). The full list of
checked entries is not mapped: patch inside function bodies. A server with anti-cheat on refuses some clients; the
reason was not traced.

### 2c. Invisible collision (player clip)

**CONFIRMED.** The surface "invisible" bit (flag 2) is read in only two places in halo3.dll. These are the leaf
surface checks of `collision_bsp_test_vector` `0x2559ac`:
- small BSP: `0x256424`; the check is at `0x2566b7`: `if (surf.flags & 2) && (test_flags & 8) skip`;
  `& 8 / & 0x10` skips breakable surfaces.
- large BSP: `0x2567e0`, flags at surface +0xE, check at `0x256a54`.

So bit 3 of the collision-test flags dword = "ignore invisible surfaces" and bit 4 = "ignore breakable surfaces"
(CONFIRMED there). This is how bullets, line of sight and other line tests pass through player clip.

The scan covered `test byte [..+0xa/0xe], 2` and byte loads followed by `test reg, 2` across the whole .text. **No
other code looks at the bit.** The sphere/feature tests and the Havok side (bipeds use Havok shape phantoms, from the
strings `character_physics` and `hkSimpleShapePhantom`; GUESSED) never filter it. That is why bipeds and vehicles
collide with player clip.

**Consequence: there is no one-branch patch in the vector tests that lets bipeds through player clip.** Bipeds and
vehicles collide through Havok, which builds its own shapes from the same collision BSP:

**The Havok side (FOUND 2026-10-18, CONFIRMED live).** Found dynamically with `PELOAD_BSPWATCH=ev` (peload.c: a breakpoint on
the small-BSP flag read `0x2566b7` captures the structure BSP, then its edges/vertices pages are made unreadable and every
reading instruction plus its caller is recorded while a biped walks; FIXES.md section 20). Guardian's structure BSP (8,856
surfaces, 156 invisible, 7,141 vertices, matching the .map file) is read by the vector tests and by one builder:
- `0x21b4d8` builds a Havok convex polygon shape for one surface (hkReferencedObject layout: vtable `0x7fbc58`, reference
  count +0xa; surface index +0x20; vertex count byte +0x2e; vertices from +0x40, 16 bytes each = x, y, z minus the
  vertex's sink, w = index | 0x3f000000; at most 8). It walks the surface's edge ring through the collision-reference
  accessors `0x2055c4` (vertex point), `0x205660` (vertex sink), `0x2056b4` (edge vertex), `0x205710` / `0x205770`
  (next edge), `0x2054d0` (plane). Its surface reference = `{small bsp*, large bsp*}` at [rbp-0x48], index [rbp-0x38]; the
  tag base is `*(halo3+0x1fcf4c8)`.
- Its callers are the getChildShape slots of two shape-container vtables: `0x21c0f8` (vtable slot `0x7fbbb0`; the key
  is the surface index, keys <= 0x1fff also go through a breakable-surface lookup `0x21c9a8`) and `0x21cf6c` (slot
  `0x7fbb38`, two calls: instanced geometry, with a transform).

**Off switch (2026-10-18):** a patch at the builder's exit (`0x21b759`, 15 bytes) checks the surface's flags and, for an
invisible one, moves every vertex to z = -20000: the MOPP still returns the key, the narrowphase finds nothing near.
Stats page button **Invisible walls on/off** (`POST /control?do=invwalls&off=0|1`; host with PELOAD_ALLOW_TELEPORT=1, or a
client of our host, forwarded: the host and that client both patch, the client only when the host accepted);
`PELOAD_INVWALLS_OFF=1` at launch; `PELOAD_INVWALLS_LOG=1` counts hidden polygons. Live test (direct Guardian, a thin
player-clip wall at y = -19.35, floor on both sides, teleported 1.8 units away and walked at it 8 s): walls on, the biped
stops at y = -19.16 (north side) / -19.54 (south side); switched off at run time, it walks through to -21.24 and falls
down the drop the wall guards. Not affected: the vector tests (bullets already passed through), object collision models.
Many invisible walls guard drops or the map's edge: with them off, the kill volumes (2a) are what's left.

Earlier options considered (superseded): serving a modified collision resource from the .map (invisible surfaces share
vertices and edges with real geometry, and it would differ from what clients have), or living with it.

### 2d. World limits an explorer meets next

| What | Where | Status |
|---|---|---|
| Position limit \|x\|,\|y\|,\|z\| <= 32768 | `0x340d7c`, compares `0x340dfc..0x340e36` against `0x8477c4` = -32768.0 (and +32768) | CONFIRMED. Only for explicit set_position (teleport/spawn), where a failure means `object_delete` on the host (teleport.md). Normal movement does not go through it |
| Outside every BSP cluster | `0x34b520` (object location from the centre, then the origin) reports "outside" (`location[0] == 0xFF`); `0x3430d4` then leaves the object disconnected from the map | CONFIRMED that there is no kill on this path. Not traced: what a disconnected biped loses (rendering/PVS, lighting, maybe AI/sound) |
| Falling damage | matg +0x458 falling damage, element 0: "Maximum Falling Distance" +0x58 and "Distance Damage" +0x5C, the same effect the kill volume applies | GUESSED: long falls off cliffs still kill. Not traced |
| Havok world extent | | Not traced. GUESSED: bodies leaving the Havok broadphase get removed; Valhalla's geometry box (about 250 x 380) is far inside any sane broadphase |
| Map edges | The BSP itself: the terrain ends in skirts (world bounds above) | Data |

## 3. Confidence

- **Map data, high.** The layouts come from the Assembly plugins and are checked against the data:
  - The trigger-volume corner convention is checked by the half-diagonal field and by the safe zone wrapping the play area.
  - The soft-ceiling names and types match between the scnr and the sddt.
  - The invisible counts match `h3bsp --invisible`.
- **Kill volumes and safe zones, high.** Every branch was read, and the patch sites are plain jcc/entry overwrites with the bytes checked against the file. Not run live.
- **Soft ceilings, high for the control flow; medium for the result-flag meanings** (2 = soft kill, 4 = slip).
- **Player clip, high for the negative result** (only the vector tests filter the bit). The Havok filter location is unknown.


## Tested live 2026-10-17 (Valhalla, our own hosted game)

Host = `shim/target.sh` (it hosts a hidden Valhalla game), moved with the new host-only teleport-to-a-point control
`POST /control?do=tpto&x=..&y=..&z=..[&slot=N]` (PELOAD_ALLOW_TELEPORT=1; the point must be inside the map structure,
|coord| < 30000; slot = a players-table slot, default = the host's own player). Ground heights off the map's collision
BSP: `re-tools/h3bsp.py` `top_z_at` (the east cliff tops, x 110-122 / y -60..-140, are z 16-29).

| host patches | teleport to (114, -100, 22.6): east cliff top, outside the safe zone (x > 106.3) |
|---|---|
| none | dead within 1 s (safe-zone kill) |
| `PELOAD_PATCH=halo3+1b8e00:31C04885D27403C60201C3,...` | **refused by the Reclaimer client**: "Native function contract mismatch at 0x1801b8e00" -- it checks that function's entry bytes; the worker exits |
| `PELOAD_PATCH=halo3+37b816:E95501000090,halo3+37b734:EB` (call-site variants inside biped_update) | **alive** 15+ s, standing on the cliff (slides slowly down its slope) |

So for exploring, host the game with
```
PELOAD_ALLOW_TELEPORT=1 PELOAD_PATCH=halo3+37b816:E95501000090,halo3+37b734:EB
```
(bipeds skip the kill-volume / safe-zone test and the player soft-ceiling response), then teleport with `tpto`.
Joining clients' own prediction still runs the soft ceilings (push-back / slip) unless they run the `37b734` patch too.
Invisible walls are unchanged (section 2c).

### Exploring from the stats page (2026-10-17)

`shim/tools/maptop.sh` (FORCE=1 to redo) now also writes `<map>.z.bin`: the top surface height every world unit
(roofs and cliff tops included, no z cut; format in the map's .json `zgrid`). On the live stats page of a game you
host with `PELOAD_ALLOW_TELEPORT=1`, tick **click to teleport** under the map and click a spot: the page looks up the
ground there and sends `tpto` to 0.6 above it. With the boundary patches above you stay alive outside the play area
(tested: Valhalla east cliff, (110, -80) -> standing at z 19.1).

### One-click from the stats page (2026-10-17)

In a game you host with `PELOAD_ALLOW_TELEPORT=1` (no launch patch needed) the overlay controls show:
- **Boundaries on / off**: writes or restores the call-site patches at run time (kill/safe zones, soft ceilings and,
  since 2026-10-17, the slip-surface ground patch halo3+219c1f:EB46 from the next section), from the simulation thread at the
  frame safe point (between ticks), all or nothing and only over the expected bytes (POST `/control?do=bounds&off=0|1`).
- **Valhalla: east cliff** (on riverworld): boundaries off, then teleports you to (114, -100, 22.6)
  (POST `/control?do=explore&spot=valhalla_cliff`). **Valhalla: north ridge** (`spot=valhalla_north`, (44, -24, 10)):
  past the north end (y > -31.9) with no soft ceiling over it. Spots: `g_st_spots` in shim/src/stats.c; the page shows
  the current map's.

Tested: explore -> alive on the cliff; boundaries back on while standing there -> dead within a second (as with the
launch-time patch). A dead or respawning player can't be teleported ("no body at the camera"): click again once you spawn.

## Soft ceilings as ground (east cliff)

Static RE, 2026-10-17. Explains the live result above: on the east cliff top, (114, -100), with
`37b816` + `37b734` patched, the player can look but cannot walk and slowly slides. At (44, -24) it walks normally.

**Answer: it is not the soft ceiling.** The cliff-top terrain is itself a **slip surface**: its collision BSP surface
flag is 0x40. A biped never counts a slip contact as ground on a multiplayer map, and Havok gives slip contacts
zero friction. The `th` drape only happens to lie 0.4 above the same terrain.
- Soft-ceiling triangles are never Havok collision. With `37b734` patched, they do nothing to a walking biped.
- The live data agrees. At (110, -80), the biped stood at z 19.1. `th` is at 19.25 there, so the biped was below the drape, not resting on it.

New tool: `python3 re-tools/h3surf.py <map> x,y [...]` lists every collision surface (flags, material, slope) and
soft ceiling above and below a point. `--slip` summarises the slip-flagged surfaces.

| Point (riverworld) | Collision surface under the feet | `th` soft kill | Live result |
|---|---|---|---|
| (114, -100) | z 22.21, **flags 0x40 slip**, material 29 `riverworld_ground_cliff` (hard_terrain_stone), slope 13.7 deg | z 22.60 | Can't walk, slides |
| (110, -80) | z 18.86, **flags 0x40 slip**, same material, slope 13.1 deg | z 19.25 | Stood at z 19.1 |
| (44, -24) | z 9.50, flags 0 (material 30 `riverworld_cliff_blend`), slope 18.4 deg | none | Walks |

`--slip` on Valhalla finds 2252 of the 70238 collision triangles flagged slip:
- 1786 are the `riverworld_ground_cliff` terrain (x 42.8..123.2, y -177.7..-33.0, z 7.3..36.3): every cliff top around the valley.
- 438 are invisible + slip player clip (flags 0x43).
- 28 are on a forerunner panel.

The other (type 2) soft ceiling, `slidey` (x <= 107.5), is not at the test point.

### Where the slip flag acts (CONFIRMED unless marked)

| Address | What |
|---|---|
| `0x205524` | Collision-surface flags getter: small BSP surface `+0xA` (12-byte stride) or large `+0xE` (16-byte stride). `0x2054d0` is the material getter |
| `0x21b4d8` | Builds the Havok shape for one BSP surface. The shape type is 0x1E, the vtable `0x7fbc58`, and the convex radius 0.0164 (= 0.05 m). It stores the material at shape `+0x30` (`0x21b745`) and the **flags at +0x32** (`0x21b752`) |
| `0x21d9c0` | Havok contact listener. When the shape type is 0x1E, it takes the flags from shape `+0x32` (`0x21db5f`). It then calls `0x15ec74` -> `0x15edbc`, which appends a 0x44-byte contact record with the **flags at +0x40** (`0x15eee4`), the normal at +0x24, and the object at +0x14 (-1 = BSP) |
| `0x15e1e0` (`0x15e2b1`) | Havok contact material for **every** object: a slip contact gets **friction 0** |
| `0x2199c4` (`0x219c11..0x219c7d`) | Biped **ground-support test**, run each tick in ground mode. It loops over the body's contacts (iterator `0x221354` / `0x221478` / `0x221514` / `0x221380`). If `contact+0x40 & 0x40` **or** `phys+0x63 & 2`, and `0x1b8bd8()` > normal.z, it raises the ground threshold `[rsp+0x70]` to `normal.z + 0.001` (`0x846968`). In the selection loop, a contact is ground only if `normal.z > threshold` (`0x21a03f`) |
| `0x1b8bd8` | The slip threshold. Returns **1.0 when `game_globals+0x10 == 2`** (multiplayer; the name is GUESSED), otherwise `*(tls+0x130)+0x1C`. So on MP maps, a slip contact is ground only if it is perfectly flat |
| `0x218ffc` (`0x2195d4`) | Ground physics, character mode 1. It calls `0x2199c4`; when that fails (no ground), it sets `[rsi+4] \|= 1`. At its end, `0x219546` clears `phys+0x63` bit 1, so the soft-ceiling slip lasts one tick |
| Call chain | Biped physics update `0x3bae08` -> `0x1dcb74` (mode dispatch: mode 1 -> `0x218ffc`, mode 3 -> `0x2452a8`) -> `0x218ffc` -> `0x2199c4`. The character-physics block is `biped+0x4DC`: mode at +2 (= biped +0x4DE), flags at +0x6B (= biped +0x547, `phys+0x63` above relative to the ground data at +8) |

So on a slip surface the biped has no ground (airborne-like: input does not move it) and its contact has no
friction, so it slides down the 13-14 deg slope slowly. This is the map's own anti-breakout design, independent of
the soft-kill drape.

### Every soft-ceiling user (none is physical)

The soft-ceiling triangles (sddt +0x10, 0x44 each) are indexed only in `0x1bb1d0`, `0x1b9474`, `0x1b9af4` and
`0x1b9d6c`. This was checked with an `imul ..., 0x44` scan over all of .text. Every one of them reads the design BSP through `0x9bc28`.
The other users of `0x9bc28` (`0x9bfe0`, `0x16b038`, `0x16b608`) are water queries (sddt +0x1C / +0x28 / +0x34). No
Havok shape is built from the design BSP.

| User | Called from | Effect |
|---|---|---|
| `0x1bb1d0` query, via `0x1bac00` response | `biped_update` `0x37b766` (players). `vehicle_update` `0x36bcd8` (type 1 slot +0x88; `0x36bd7b`, only for a parentless vehicle whose tag `+0x2BC` bit 11 is set (huge vehicle), with prev-pos vehicle+0x78C). `0x117798` (`0x11789c`) | Acceleration (velocity reflection), the soft-kill timer, and the slip flag `biped+0x547 \|= 2`. That flag feeds the **same** `0x219c21` test above. It also sets/clears `parent+0xA8` bit 19 every call |
| `0x1b9474` | `0x213718` / `0x213904` | Sphere query over **acceleration (type 0)** ceilings only. Not traced (GUESSED: AI / huge-vehicle avoidance) |
| `0x1b9af4` | `0x1fe800` | Camera. GUESSED from the global `debug_structure_soft_ceilings_camera` |
| `0x1b9d6c` | `0x117a24` <- `0x117798` | Same object list as the `0x11789c` call (tls+0x20, +0xE4B8). GUESSED: multiplayer item/object out-of-bounds handling |

With `37b734` patched, nothing soft-ceiling related touches a player biped.

### Patch: make slip surfaces walkable

`PELOAD_PATCH` / run-time write, at call sites inside function bodies (no entry bytes are touched):

| Goal | Patch | Original bytes | Notes |
|---|---|---|---|
| **Bipeds ignore slip for grounding (BSP + soft-ceiling slip)** (recommended) | `halo3+219c1f:EB46` | `75 06` | `jne 0x219c27` becomes `jmp 0x219c67`: the slip loop body is skipped for every contact, so the threshold stays the tag value |
| Bipeds ignore BSP slip only (keep the soft-ceiling slip flag) | `halo3+219c1f:9090` | `75 06` | Falls through to `test [rbx+0x63],2`. With `37b734` patched, this is the same as the row above |
| Optional: normal friction on slip contacts | `halo3+15e2b6:EB06` | `74 06` | `je` becomes `jmp`: the slip override (friction 0) is skipped. This affects **every** Havok object on slip surfaces (vehicles, crates, weapons). Use it only if a standing biped still creeps |

Exploring recipe (host):
```
PELOAD_ALLOW_TELEPORT=1 PELOAD_PATCH=halo3+37b816:E95501000090,halo3+37b734:EB,halo3+219c1f:EB46
```

- **Context:** simulation (main) thread, inside the per-object physics update of each game tick, under the
  game-state lock: the same place as biped_update. A run-time write is safe at the frame safe point, as the stats page's
  "Boundaries on/off" does for the other two patches. Check `75 06` first.
- **Who is affected:** every biped in ground mode, players and AI alike.
  - GUESSED: creatures too, via `0x3e14e4` -> `0x1dcb74`.
  - Slip-flagged surfaces become normal ground, subject to the biped's usual max-slope.
  - On other maps, ledges or rails that designers made unstandable become standable too.
  - Not affected: vehicles (their physics does not use `0x2199c4`), projectiles, the camera, and soft-ceiling acceleration / soft kill (those are `37b734`).
- **Clients:** each machine runs this test for the bipeds it simulates. The host does all bipeds; a joining client
  does its own predicted biped. Patch only the host, and a remote player's prediction still slides while the host
  walks it, so the client keeps getting corrected (GUESSED rubber-banding). Patch the clients too (same
  bytes). The host's own player needs only the host patch.
- **Entry checks:** the Reclaimer client refused `1b8e00`'s *entry* bytes. `0x219c1f` is 0x25B bytes into
  `0x2199c4`, like the accepted `37b734` / `37b816`. It is the third patch of the stats page's "Boundaries on/off"
  switch (hitbox.c g_hb_bnd, since 2026-10-17); a walking test on the cliff with it is not recorded yet.

### Confidence

| Claim | Confidence |
|---|---|
| The cliff top is slip-flagged terrain, and the soft-ceiling geometry is not physical | High (map data + the full list of soft-ceiling users) |
| The chain BSP flags -> shape `+0x32` -> contact `+0x40` -> `0x219c1b` / `0x15e2b1` | High (every hop read in the code) |
| `0x1b8bd8` == 1.0 on MP maps | Medium. The gg+0x10 == 2 "multiplayer" meaning is GUESSED |
| `219c1f:EB46` alone gives normal walking | Medium. The zero-friction contact remains (GUESSED harmless while grounded; add `15e2b6:EB06` if not) |

## In-game zones overlay (2026-10-17/18)

Stats page button **Kill/safe zones** (POST `/control?do=zones`; launch `PELOAD_HB_ZONES=1`; the F10 key is retired,
`PELOAD_HB_FKEYS=1` brings it back). shim/src/hitbox.c `hbz_*`, read from the .map being played (hb_current_map:
halo3/maps or a Workshop folder, so Workshop maps too):
- Kill volumes (in the scnr +0x484 list or with a kill-trigger index at +0x40) and safe zones (+0x490) as the oriented
  boxes of section 1; soft ceilings from every structure BSP's sddt (+0x10, triangles 0x44 with vertices at
  +0x20/+0x2C/+0x38). It logs `[zones] <map>: N kill volumes, N safe zones, N soft-ceiling triangles (N outline edges)`.
- Drawn on the GPU (`hbz_gpu`, in the present-queue blit): translucent surfaces plus brighter edges (a ceiling's
  outline = the edges only one of its triangles has: Valhalla 5268 triangles -> 1699 edges), one static buffer per map
  and kind mask.
- Per-pixel occlusion: the fragment shader projects each pixel with the camera of the visible-only depth readback
  (hb_vis_tick: the camera draw's D32F_S8 depth blitted to 480x270, read back 2 frames late; in GL-helper mode as an
  async readback, one ticket in flight) and discards it where the scene is nearer (point moved 0.3 toward the camera
  so surfaces on the ground stay). Since 2026-10-18 a pixel outside that depth frame's view (the strip turning into
  view, or behind its camera) is discarded too: before, zones showed through walls at the screen edges while turning.
- Each kind is a chip on the page (POST `/control?do=zonetype&t=<bit>`; `PELOAD_HB_ZONES_SHOW=<mask>`, default 31):
  1 kill zone (red), 2 safe zone (green: outside every one kills), 4 acceleration ceiling (orange), 8 soft-kill
  ceiling (pink), 16 slip surface (yellow).
- `PELOAD_HB_ZONES_XRAY=1` = the old CPU line drawing through walls (`hbz_draw` / hb_seg_clip in the overlay's GL_LINES batch).
- Not drawn: invisible collision (player clip) -- only the map pictures (h3bounds.py, cyan) show it.
