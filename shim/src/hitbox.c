// hitbox.c — hitbox / bounding-volume overlay for Halo 3 objects. Included by peload.c.
// The retail halo3.dll compiled out its own debug_objects_* overlays (see shim/docs/halo3-engine-globals.md),
// so the plan is to read the engine's object table and draw the volumes ourselves.
//
// DO NOT scan process memory to find engine structures. A first attempt walked every VM region with
// mach_vm_region + mach_vm_read_overwrite looking for halo3's 'd@t@' data-array signature; on a ~6 GB
// Rosetta process (GPU/IOSurface-mapped regions included) it caused a macOS KERNEL PANIC (2026-10-16,
// "Kernel data abort", panicked task peload). Locate structures from halo3.dll's own code/data instead
// (a global pointer found by static analysis) and read only those known addresses.
static void hitbox_start(void){ }
