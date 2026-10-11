# Project Reclaimer dedicated server — RCON / wire-protocol review

Scope: static review of `project-reclaimer-dedicated-v0.9.13.exe` and
`reclaimer-rcon-v0.9.13.exe` (SHA-256 verified against the release
`SHA256SUMS.txt`). This is review of binaries we run on our own machine,
for hardening our own server. No live server was attacked.

## Headline: the toolchain changes which bugs are even possible

Both binaries are **Rust** (rustc 1.x, confirmed by `rustc/<hash>` paths,
`cargo` registry paths, and panic machinery). That matters for the two bug
classes asked about:

- **Format-string bugs: not possible by construction.** Rust's `format!`,
  `println!`, `tracing`, etc. require the format string to be a
  compile-time literal. Attacker-controlled text only ever lands in a `{}`
  argument, which is type-checked and not re-interpreted. There is no
  `printf(user_input)` equivalent. I looked; there is no C `printf` family
  on a user-data path in either binary.
- **Classic stack/heap buffer overflows: require `unsafe`.** Safe Rust
  bounds-checks every slice and array access. An out-of-bounds read/write
  needs an `unsafe` block with a bug in it, or a vulnerable C dependency via
  FFI. The server's only meaningful C FFI is into `halo3.dll` itself (the
  game engine) — that is the genuinely unsafe surface, see below.

So the productive question for a Rust network service is not "is there a
`strcpy`" but **"what can a remote, possibly unauthenticated, peer do to
cause a panic (DoS), an over-allocation (DoS), or to reach the unsafe engine
FFI with crafted data."**

## RCON attack surface, in context

The server exposes RCON over **two transports** (strings confirm both):
- **Source RCON** — Valve's TCP protocol: 4-byte little-endian length
  prefix, then id/type/body with null terminators. This is the classic
  surface.
- **WebSocket** — over the bundled `h2`/HTTP stack (axum/hyper/tungstenite
  crates are linked).

Observed, relevant facts:

1. **Auth gate exists.** Strings include a "gave a wrong password" path and
   `RCON ... off (set a password under [rcon] to turn it on)`. RCON is
   disabled unless a password is configured; an empty password on a
   `[[server]]` block explicitly makes *that* server open (documented
   footgun, not a bug).
2. **RCON is plaintext — this is the real, confirmed weakness.** The
   binary's own help text repeats it: *"RCON is not encrypted … anyone who
   can reach its ports may try the password."* The password and all command
   traffic cross the network in clear. Mitigation is deployment-only: bind
   to `127.0.0.1` + SSH tunnel / VPN / HTTPS proxy, or firewall to known
   admin IPs. There is no rate-limit string suggesting brute-force
   protection, so an exposed port invites password guessing. **Treat the
   RCON port as loopback-only.**
3. **Bounded parsers elsewhere.** The variant/playlist/Megalo parsers carry
   explicit limits ("Playlist exceeds 32 MiB", "exceeds Reach variable
   capacity", "exceeds MAX_PVQ_N", "size too large: the integer type can
   fit", "exceeds available bytes") — i.e. length fields are validated
   against remaining buffer before use. That is the right pattern and argues
   against naive over-allocation in those paths.

## What I could NOT confirm statically (and the honest limits)

Strings show the *shape* of the code, not the control flow from
`socket.read()` to each parser. Specifically unconfirmed:

- **Pre-auth reachability.** Whether the Source RCON length-prefix read and
  body allocation happen *before* the password check. If a 4-byte length is
  used to size a buffer before auth, a remote unauthenticated peer could
  drive allocation/DoS. Classic Source RCON implementations read the length
  first — so this is the single most important thing to verify.
- **Panic blast radius.** 16 distinct panic strings and many
  `unwrap()/expect()` exist (most in deps/std, not necessarily reachable).
  If the RCON handler runs as a per-connection tokio task and a panic there
  only drops that connection, severity is low. If a panic can poison a mutex
  shared with the game loop or abort the process (`panic = "abort"` or a
  panic across the `halo3.dll` FFI boundary — which is **undefined
  behavior** in Rust), severity is high.
- **The engine FFI.** Data forwarded from RCON/network into `halo3.dll`
  (chat text, player names, Megalo script) crosses into unsafe C++. Any
  memory-safety bug there is a *halo3.dll* bug, not Reclaimer's, but
  Reclaimer chooses what it forwards and how it's bounded.

## Recommended next step: dynamic, in-context

These unknowns are answerable precisely once the server runs — which is the
other half of this project. Concretely, against a **local** instance:

1. Fuzz the Source RCON TCP port with malformed frames: negative/huge length
   prefixes, truncated bodies, oversized bodies, missing terminators,
   commands sent before `SERVERDATA_AUTH`, auth with id mismatches. Watch
   for process abort vs. per-connection drop (the pre-auth reachability and
   blast-radius questions answer themselves here).
2. Run the server under a sanitizer-ish harness — since the sensitive part
   is the `halo3.dll` FFI, our own PE loader (which controls every call in
   and out of the DLL) is actually the ideal instrumentation point: we can
   log/validate every buffer crossing the boundary.
3. Build a minimal Source RCON client to replay captured admin commands and
   confirm the auth state machine ordering.

Nothing here is a filed vulnerability yet. The one actionable hardening
item today is deployment: **never expose the RCON port; loopback + tunnel
only.** The rest is a dynamic pass once the server is up.
