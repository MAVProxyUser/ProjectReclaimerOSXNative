# Halo 3 engine settings you can change at launch (`PELOAD_H3`)

halo3.dll keeps a table of its HaloScript globals — 24-byte entries `{name, type, storage}` (1251 of them in
the MCC build the loader runs). The retail build **compiled out the storage of ~1030** (storage address 0),
so most debug switches are gone. Of the 219 entries with storage (re-counted 2026-10-18 from the halo3.dll in the
prefix), 138 are AI enum constants (`ai_movement_*`, `ai_activity_*`, `morph_form_*`, type short), 2 are AI handles
(`ai_current_squad` / `ai_current_actor`, type 19), and **79 are settings** (36 bool, 35 real, 8 long): the tables
below. The loader can set bool / real / short / long globals at launch (other types are reported as unsupported):

```bash
PELOAD_H3="render_debug_depth_render=1,camera_fov_scale=1.2" bash shim/play.sh
```

Values: numbers (`true`/`on` = 1 for booleans). The loader writes each value when halo3.dll loads and again
every 0.5 s; names that are compiled out or unknown are reported in `/tmp/reclaimer_play.log` (`[h3]` lines).
Effects not marked "verified" are inferred from the names — try them.

**Not available: hitboxes and the other `debug_objects_*` overlays.** `debug_objects_collision_models`,
`debug_objects_physics_models`, `debug_objects_bounding_spheres`, `debug_objects_biped_autoaim_pills`,
`debug_objects_model_targets`, `collision_debug*` (55), `physics_debug`, `havok_debug_mode` and the cheats
(`cheat_deathless_player`, `cheat_infinite_ammo`, `cheat_bottomless_clip`, `cheat_super_jump`, `cheat_medusa`,
`cheat_omnipotent`, `cheat_jetpack`, …) all have no storage — their variables and drawing code are not in the
retail DLL. Compiled-out families by prefix: debug 244, ai 216, render 117, net 76, collision 55, havok 19,
cheat 12, and smaller ones. The loader draws its own hitboxes instead (the stats page's Hitboxes button, or
`PELOAD_HITBOXES` at launch; see README).


## Rendering and post-processing

| Setting | Type | Default | Notes |
|---|---|---|---|
| `allow_480p_resolutions` | bool | 0 |  |
| `decorator_res_scaled` | bool | 1 |  |
| `use_hdr_bloom_scaling` | bool | 0 |  |
| `hdr_bloom_intensity` | real | 0.35 | bloom strength. |
| `skies_delete_on_zone_set_switch_enable` | bool | 0 |  |
| `reduce_widescreen_fov_during_cinematics` | bool | 1 |  |
| `motion_blur_max_viewport_count` | long | 1 |  |
| `motion_blur_center_falloff` | real | 2 |  |
| `motion_blur_max_y` | real | 0.024 |  |
| `motion_blur_scale_x` | real | 0.045 |  |
| `motion_blur_scale_y` | real | 0.08 |  |
| `motion_blur_taps` | long | 5 | motion blur sample count. |
| `motion_blur_expected_dt` | real | 0.0333333 |  |
| `motion_blur_max_x` | real | 0.0135 |  |
| `render_alpha_to_coverage` | bool | 0 |  |
| `instance_default_fade_start_pixels` | long | 36 | instanced geometry fade start (screen pixels). |
| `instance_default_fade_end_pixels` | long | 30 | instanced geometry fade end. |
| `decal_disable` | bool | 0 | 1 = no decals (bullet holes, blood, scorch marks). |
| `render_debug_depth_render_scale_g` | real | 10 |  |
| `render_debug_depth_render_scale_b` | real | 9 |  |
| `render_debug_depth_render` | long | 0 | 1 = show the depth buffer as a red/green/blue gradient instead of the scene (scales: render_debug_depth_render_scale_r/g/b). Verified. |
| `render_debug_depth_render_scale_r` | real | 9 |  |
| `render_bounce_light_intensity` | real | 1.5 |  |
| `render_bounce_light_only` | bool | 0 | 1 = show only bounce (indirect) lighting. |
| `render_postprocess_saturation` | real | 0 | post-process colour saturation offset (0 = default). |
| `render_postprocess_red_filter` | real | 0 | post-process red channel filter. |
| `render_postprocess_green_filter` | real | 0 | post-process green channel filter. |
| `render_postprocess_blue_filter` | real | 0 | post-process blue channel filter. |
| `render_postprocess_hue` | real | 0 | post-process hue shift. |
| `render_HDR_target_stops` | real | 7 |  |
| `render_surface_LDR_mode` | long | 3 |  |
| `render_surface_HDR_mode` | long | 2 |  |
| `render_first_person_fov_scale` | real | 0.88 | scale of the first-person weapon/arms field of view. |
| `render_far_clip_distance` | real | 10240 | far clip plane (world units). |
| `texture_camera_exposure` | real | 1 |  |
| `texture_camera_illum_scale` | real | 1 |  |
| `render_near_clip_distance` | real | 0.0078125 | near clip plane. |
| `render_screen_flashes` | bool | 1 | 0 = no full-screen flashes (damage, flashbangs). |
| `texture_camera_near_plane` | real | 0.001 |  |
| `render_lightmap_shadows` | bool | 1 | 0 = no lightmap shadows. |
| `effects_corpse_nonviolent` | bool | 0 | 1 = non-violent corpse effects. |
| `camera_fov_scale` | real | 0 | multiplies the player camera field of view (0 = engine default). |

## Streaming / prediction and performance

| Setting | Type | Default | Notes |
|---|---|---|---|
| `use_memory_mirror_events` | bool | 1 |  |
| `sleep_for_vblank` | bool | 1 | engine-side vsync sleep. |
| `cinematic_prediction_enable` | bool | 1 |  |
| `enable_better_cpu_gpu_sync` | bool | 1 |  |
| `disable_render_state_cache_optimization` | bool | 0 |  |
| `enable_structure_prediction` | bool | 1 |  |
| `ignore_predefined_performance_throttles` | bool | 0 | 1 = ignore the map's performance throttle presets (more effects/objects drawn). |
| `enable_first_person_prediction` | bool | 1 |  |
| `enable_cluster_objects_prediction` | bool | 1 |  |
| `enable_tag_resource_prediction` | bool | 1 |  |
| `enable_entire_pvs_prediction` | bool | 1 |  |

## Sound

| Setting | Type | Default | Notes |
|---|---|---|---|
| `enable_sound_transmission` | bool | 1 | sound transmission through walls. |
| `debug_sound_listeners` | bool | 0 | debug flag for sound listeners (drawing may be compiled out). |
| `debug_duckers` | bool | 0 | debug flag for sound ducking. |
| `enable_pc_sound` | bool | 1 |  |
| `sound_global_room_gain` | real | 1 | global reverb/room gain. |

## Physics and world

| Setting | Type | Default | Notes |
|---|---|---|---|
| `breakable_surfaces` | bool | 1 | 0 = surfaces (glass etc.) never break. |
| `water_physics_velocity_minimum` | real | 0.5 | water physics velocity clamp. |
| `water_physics_velocity_maximum` | real | 3 | water physics velocity clamp. |
| `minimum_havok_object_acceleration` | real | 0.05 |  |
| `minimum_havok_biped_object_acceleration` | real | 0.3 |  |
| `disable_expensive_physics_rebuilding` | bool | 0 |  |
| `use_dynamic_object_obstruction` | bool | 0 |  |

## Tool / console / input

| Setting | Type | Default | Notes |
|---|---|---|---|
| `use_temp_directory_for_files` | bool | 0 |  |
| `use_tool_command_legacy` | bool | 0 |  |
| `maximum_tool_command_history` | long | 5 |  |
| `enable_console_window` | bool | 1 | engine console window flag (no console UI in MCC builds). |
| `sapien_keyboard_toggle_for_camera_movement` | bool | 0 |  |
| `enable_pc_joystick` | bool | 1 |  |
| `ai_current_squad` | AI handle (type 19) | 0 | not settable with `PELOAD_H3` (unsupported type). |
| `ai_current_actor` | AI handle (type 19) | 0 | not settable with `PELOAD_H3` (unsupported type). |
| `debug_controller_latency` | bool | 0 |  |

## Flood morph (campaign AI; no effect in multiplayer)

| Setting | Type | Default | Notes |
|---|---|---|---|
| `morph_disallowed` | bool | 0 | Flood pure forms never morph. |
| `morph_time_ranged_tank`, `morph_time_ranged_stealth`, `morph_time_tank_ranged`, `morph_time_tank_stealth`, `morph_time_stealth_ranged`, `morph_time_stealth_tank` | real | 1.5 | morph duration (s) between the forms. |
