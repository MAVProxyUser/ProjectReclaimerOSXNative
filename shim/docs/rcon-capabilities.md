# Reclaimer dedicated server — capability inventory & RCON reach

Static review of `project-reclaimer-dedicated-v0.9.13.exe` and
`reclaimer-rcon-v0.9.13.exe` (hashes verified). No binary was executed or
modified for this review. Static evidence (strings, imports, exports) shows
what code *exists* and roughly how it's gated; it does **not** prove runtime
reachability. Confirming the reachable paths needs the dynamic pass (fuzzing
a local server), which the shim will enable.

## Direct answers

**Can RCON commands execute code?**
No RCON verb does. The RCON command set is game moderation only (see below) —
there is **no raw engine-console passthrough** verb and no `exec`/`eval`.
Code execution exists in the process but behind config + subsystems, not RCON:
- `CreateProcessW` → launches **SteamCMD** for Workshop mod downloads. The
  executable path comes from config (`[workshop] steamcmd = '...'`), not the
  network. "Reclaimer never accepts Steam passwords or Steam Guard codes."
- `LoadLibraryExW` → loads the fixed MCC/`halo3.dll` engine DLLs. Targets are
  not attacker-supplied; Workshop content is map packages, never DLLs.

**Upload / download arbitrary files or DLLs?**
- **Download (mods):** Workshop items via SteamCMD, from Steam, confined to
  `content/{Maps,Images,Game Modes,Mods}`. Validated: rejects filesystem
  links/symlinks, "unsupported directory or link", invalid file names, items
  exceeding install limits, items that aren't an MCC mod package or contain no
  map files. Max 64 items. **Not** arbitrary-path, **not** DLLs.
- **Download (self-update):** from the GitHub releases API, **SHA-256 verified**
  against the release's published checksums; refuses on mismatch. Update URL
  overridable only via the `RECLAIMER_UPDATE_URL` env var (not network/RCON).
  Gated by `auto_update` config; "the executable's folder must be writable."
- **Upload (server→client):** "mod sharing" (`share_mods`/`share_rate`) sends
  validated map packages to joining clients. Not arbitrary files.
- No arbitrary file read/write primitive is exposed to RCON. The only
  "cannot write" RCON string is a socket-write error, not filesystem.

**Port-mapping (UPnP) poke?**
`igd-next` crate; the server speaks UPnP to the **local router** via SSDP
(`239.255.255.250:1900`): `AddPortMapping` / `DeletePortMapping` /
`GetExternalIPAddress`, for **its own** listen + master ports, at startup and
on lease renewal. It is a UPnP **client**, not a server — there is no inbound
UPnP control surface, and it is gated by `upnp` config, not RCON. (Classic
LAN-UPnP abuse doesn't apply: nothing lets a peer request mappings here.)

**Hidden commands / "knock-knock" sequences?**
The closest thing is an **experimental native-engine worker**, gated behind a
chained set of CLI flags + env vars, all labeled EXPERIMENTAL, none
network/RCON-reachable:
- flags: `--startup`, `--skip-render-warmup`, `--shell-callbacks`,
  `--native-udp`, `--guardian-slayer`, `--allow-empty-match`,
  `--probe-admission`, `--real-client`, `--client-window`
- the chain IS a sequence: "Native experiments require --startup", "Native UDP
  requires shell callbacks and render warmup bypass", "Real-client experiment
  requires empty match", "Standalone client window requires Guardian", etc.
- env: `RECLAIMER_NO_CONSOLE`, `RECLAIMER_PARENT_CONTROL`, `RECLAIMER_VOIP`,
  `RECLAIMER_TEXT_CHAT`, `RECLAIMER_PLAYLIST_NO_DEBUG_HEAP`, `RECLAIMER_DISCOVERY_DIR`
- **`--shell-callbacks`** is the eyebrow-raiser: it "supplies mapped callbacks"
  (engine RVAs) to the worker — "Unmapped shell callback; stopping the
  experimental worker." A developer harness for driving the native engine, run
  under a supervisor in a private temp dir. Powerful, but local-operator only.

No evidence of a *network* backdoor, magic auth bypass, or hardcoded
credential. Integrity goes the other way: hardcoded SHA-256 pins reject an
"unrecognized Halo 3 binary" and unrecognized Guardian map metadata
(`6d8434a3...`). Reclaimer even ships a bounded (256 MiB) static PE-analysis
"engine probe" that inspects `halo3.dll` (exports, imports, networking
imports) — part of anti-cheat/integrity, not an attack surface.

## RCON command set (moderation only)

From the client's own arg surface and the server's help/response strings:
`players`, `kick`, `ban` / `unban` (+ ban list), `mute` / `unmute`, `chat`/`say`,
`vote` (+ `vote image`), `end` (end game) / start new round, `team` + `team
shuffle`, `map` / `variant` / `playlist` selection, and `--watch` (stream
console + live events). All operate on the running match. None sets config,
installs Workshop items, writes files, or changes ports.

## Flawed-logic / risk notes (static, to verify dynamically)

1. **Plaintext RCON (confirmed).** Password + traffic in clear; no rate-limit
   string seen. Loopback + tunnel only. This is the real exposure.
2. **Open-server footgun (confirmed).** An empty `rcon_password` on a
   `[[server]]` block makes that server's console **open** (no auth). A
   per-server override of a global password — easy to set by accident.
3. **Pre-auth parsing reachability (unconfirmed).** Whether the Source RCON
   length-prefix read / body allocation happens before the password check —
   the top dynamic-fuzz target.
4. **`RECLAIMER_UPDATE_URL` + writable exe dir (low, local).** Anyone who can
   set the server's env and write its folder can steer self-update — but the
   SHA-256 check still gates what installs. Not remote.
5. **Panic blast radius (unconfirmed).** Whether an RCON/parser panic drops one
   connection or aborts the process / poisons a lock shared with the game loop,
   or crosses the `halo3.dll` FFI (UB). Answerable only at runtime.

Nothing here is a filed remote vulnerability. The actionable items today are
deployment hygiene (#1, #2). The rest resolves in the dynamic pass.
