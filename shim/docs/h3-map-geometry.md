# Halo 3 (MCC) level geometry from a .map file -> top-down picture

Tool: `re-tools/h3bsp.py` (read-only, static file work, numpy + Pillow; builds on `re-tools/h3map.py`).

```bash
python3 re-tools/h3bsp.py "$MAPS/riverworld.map" out.png [--size 2048 | --px-per-unit N] [--bounds=x0,y0,x1,y1]
#   --crop play|geometry|all   --zcut auto|off|<z>   --no-cull   --no-instances   --invisible   --unmaterialed
```
`$MAPS` = `$HOME/Library/Application Support/ReclaimerMac/prefix/drive_c/Program Files/Steam/steamapps/common/Halo The Master Chief Collection/halo3/maps`.
About 1 to 4 s per map.

## Output contract (out.png + out.json)
- PNG is RGBA. Alpha 0 means there is no geometry. Orthographic, looking down -Z. World +X points right, world +Y points up.
- `px = (x - x0) * px_per_unit`, `py = (y1 - y) * px_per_unit`, origin at the top-left corner of pixel (0,0).
  `x1 = x0 + w/px_per_unit` and `y0 = y1 - h/px_per_unit` hold exactly.
- These are the same world units as object positions in memory. Checks:
  - Valhalla: the user's sample position (80.6, -88.0, 2.3) has ground z 1.86 under it in the BSP.
  - Every map: all scenario spawn points land on floors in the image.
- Brightness encodes height. `zmin` is darkest and `zmax` is lightest; heights outside that range clamp. The range comes from the floors within 6 units of spawn points and items. Within it, brightness is 35 % linear and 65 % histogram-equalised. Slopes get NW hillshade. Steps above 0.3 units get a dark outline.
- Extra keys in the JSON:
  - `z_in_image`: the real z extremes in the image.
  - `play_box`: the spawn and item bounds.
  - `geometry_box`: the bounds of all drawn triangles.
  - `zcut` and `roofed_points`: the result of the indoor heuristic below.
- Bounds for the verified maps (default crop):

  | Map | Default crop (x, y) | Play box (x, y) | Geometry (x, y) |
  |---|---|---|---|
  | Valhalla (riverworld) | 20.3..131.3, -188.1..-17.9 | 48.7..102.8, -159.7..-46.2 | -38..207, -285..90 (incl. skyline) |
  | Guardian | -17.7..19.5, -28.0..6.2 | | |
  | Construct | -26.2..26.6, -42.2..20.9 | | |

## Format path (header v13 "Update 14" builds; layouts = Assembly Halo3MCC + TagTool gen3/MCC)
1. **scnr** +0x18 "Structure BSPs" (0x6C each, tagref at +0) gives the **sbsp** tags. Every MP map has exactly one; campaign maps have 3 to 16.
2. **sbsp meta**:
   - +0x38C: datum of its `structure_bsp_tag_resources` resource (index = datum & 0xFFFF).
   - +0x1C0: instanced geometry *instances*, 0x78 each:
     - +0x00 scale; +0x04 forward, +0x10 left, +0x1C up, +0x28 position.
     - +0x34 definition index (s16), +0x36 flags (bit 1 = render only, bit 3 = collidable).
     - +0x40 world bounding sphere; +0x54 name sid.
     - Transform: `p' = scale*(x*fwd + y*left + z*up) + pos`. This matches the stored world sphere centre exactly; the column convention is off by up to 64 units.
   - Other fields: +0x28 collision materials (0x18 each, shader tagref +0, global material +0x10), +0x40 world bounds.
   - +0x254 is a `render_geometry_api_resource_definition` (decorator geometry). +0x36C, the main render geometry resource, is empty: the textured level meshes live elsewhere (lightmap/Lbsp). This tool uses collision only.
3. **zone** tag (cache_file_resource_gestalt):
   - +0x04: resource types (0x1C each, name sid +0x18). Order: bitmap_texture_interop, render_geometry_api, constant_buffer, sound, model_animation, structure_bsp_tag_resources, structure_bsp_cache_file_tag_resources.
   - +0x64: resource table, 0x40 per entry:
     - +0x12 type (s8)
     - +0x14 / +0x18: definition offset / size in the info buffer
     - +0x22 segment index (s16)
     - +0x24 base definition address
     - +0x28 resource fixups, 8 bytes each: u32 offset into the definition, u32 address
   - +0x148 / +0x154: info buffer size / 32-bit tag pointer.
4. **Definition**: copy `info[off:off+size]`, then write every fixup's address at its offset. Resource addresses are `(type << 29) | offset`:
   - type 1 = the definition bytes
   - type 2 = primary segment data
   - type 3 = secondary segment data

   A resource tag block is `count, address, pad` (12 bytes). The struct starts at `base & 0x1FFFFFFF`.
5. **play** tag (cache_file_resource_layout_table):
   - +0x00 codecs: one GUID, b42ada7e..., which is raw deflate.
   - +0x0C external cache files (0x108 each): english / mainmenu / shared.map.
   - +0x18 raw pages, 0x58 each:
     - +0x3 codec (s8, -1 = stored)
     - +0x4 shared file index (s16, -1 = this file)
     - +0x8 offset, relative to the resource section
     - +0xC compressed size
     - +0x10 uncompressed size
   - +0x3C raw segments, 0x10 each: s16 primary page, s16 secondary page, s32 primary offset, s32 secondary offset, s16 x2 size index (-1 here; the size table is empty).
6. **Page data**:
   - The resource section's file offset is `header sections[1].addr + masks[1]` (h3map's tables at 0x4DC / 0x4CC).
   - Decompress with `zlib.decompress(raw, -15)`.
   - On MP maps every sbsp resource sits in one page. Valhalla: page 1420, 45 MB inflating to 92 MB, about 0.1 s.
7. **structure_bsp_tag_resources** (0x30). The collision blocks hold their data in page data (address type 2).
   - Blocks:
     - +0x00 collision bsps (0x60 each)
     - +0x0C large collision bsps (0x60 each, int32 indices; no H3 MCC map uses them)
     - +0x18 instanced geometry definitions (0xC4 each; +0x04 sphere offset, +0x10 radius, +0x14 inline collision bsp)
     - +0x24 havok data
   - **collision bsp** = 8 blocks: 3d nodes(8), planes(16), leaves(8), 2d refs(4), 2d nodes(16), +0x3C surfaces(0xC), +0x48 edges(0xC), +0x54 vertices(0x10).
     - surface: u16 plane (bit 15 = negated), u16 first edge, s16 material, s16 breakable set, s16 breakable, u8 flags, u8 best-plane vertex.
     - surface flags: 1 two-sided, 2 invisible, 4 climbable, 8 breakable, 0x40 slip.
     - edge: u16 start vertex, end vertex, forward edge, reverse edge, left surface, right surface.
     - vertex: float3 + u16 first edge + s16 sink.
   - Polygon of surface s: start at its first edge e. If `edge.left == s`, take `start` and go to `forward`; otherwise take `end` and go to `reverse`; stop when back at the first edge. The ring is counter-clockwise around the plane normal; this held for every surface checked.

## Rendering choices (all switchable)
- What gets drawn:
  - The structure collision BSP plus every placed instance, except instances flagged render-only.
  - Surfaces flagged *invisible* (player clip) are dropped.
  - Surfaces with *no collision material* are dropped: sky and ceiling blockers, e.g. Valhalla's z=1892 lid and its backdrop plane.
- **Back-face cull**: the tool keeps the highest *upward-facing* (or two-sided) surface per pixel. Without the cull, ceilings and undersides win.
- **`--zcut auto`**:
  - It counts the spawn points and items that have geometry more than 1.5 units overhead ("roofed").
  - If dropping every triangle whose lowest point is above (highest spawn/item z + 2.5) un-roofs at least 15 % of them, the cut is applied.
  - Applied: Construct, Guardian (tree canopy), The Pit, Foundry, Epitaph, Last Resort, ...
  - Not applied: Valhalla, Sandtrap, Standoff, ...
- **Crop `play`**: the box around the scenario's spawn points and game-mode markers, plus weapon, vehicle and equipment placements, plus `max(16, 25 %)` margin, clipped to the geometry.
  - Scenery counts only if it comes from `objects\multi\`.
  - Vehicles, weapons and equipment count only if they come from `objects\{multi,weapons,vehicles,equipment}\`. This keeps Sandbox's and Sandtrap's out-of-bounds "defender" turrets out of the box.

## Limits
- It is one top layer:
  - Stacked floors show only the upper one (Construct is still 58 % roofed after the cut).
  - Snowbound's bunkers sit under terrain at play height.
  - Blackout and Heretic interiors stay roofed (the cut does not help there).
- Objects are not drawn: scenery, crates, machines/doors/lifts, Forge items. Their collision lives in their own `coll` tags. Water is not collision, so river beds show.
- Not implemented:
  - Resource pages stored in `shared.map` (never needed for sbsp).
  - Real render geometry (vertex/index buffers).
- `h3map.H3Map.sid` assumes a 19-bit index. These builds use a 16-bit index and an 8-bit namespace; namespace-0 overflow continues after `ns_start[-1]`. `h3bsp.sid_name` has the correct decoding. The h3map `locations` names are wrong because of this.
