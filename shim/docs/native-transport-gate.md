# The `--native-udp` native-transport integrity gate — PASSED

Status: **PASSED** as of 2026-10-11. The gate below is defeated. Everything
after it in this file is history (2026-10-11): the `os error 2` blocker was
fixed the same day (last section), a real NSWindow came the same evening, and
the client now runs its menu, joins online games and renders them (CLAUDE.md).

**Still true today:** the client checks halo3.dll's ws2_32 import slots on
every run (the play log's "Selected native loopback bind; shell peer
transport=true." is the passed gate). Every ws2_32 import (exe and halo3) is
bound through the one ordinal/name table that `GetProcAddress(WS2_HANDLE,
...)` also uses (peload.c
`ws2_by_ordinal` / `ws2_resolve_name`); the trace stubs of `PELOAD_TRACE_ALL`
/ `PELOAD_CALLCOUNT` wrap only the **exe's** ws2 imports, never halo3's (a
stub there would differ from GetProcAddress and trip "Native transport
instruction/import mismatch; refused").

## How the gate was passed (summary)

The gate reads halo3.dll's WS2_32 import thunks (6-byte `FF25` reads) and the
`bind` IAT slot (RVA `0x6fa778`, ordinal 2) via `ReadProcessMemory`, then
compares that slot to the function it independently resolves via
`GetModuleHandle("ws2_32")`+`GetProcAddress`. Our loader was handing out a
*different* stub for the IAT entry vs. GetProcAddress, so the slots mismatched.

Fix (commit `2bfe516`): resolve every ws2_32 function through ONE ordinal/
name-keyed table shared by `bind_imports` and `GetProcAddress`, behind a
distinct module handle `WS2_HANDLE` returned by `GetModuleHandle`/`LoadLibrary`
for "ws2_32". IAT.bind now equals GetProcAddress(ws2_32, 2). Gate prints
`Selected native loopback bind; shell peer transport=true`. Later commit
`04aabf4` made those functions real (BSD sockets) for the loopback UDP path.

## Progress after the gate

The worker now runs through: window/graphics contract init, **all menu shaders
compile**, **`Persistent menu created`**, audio init (`Menu music ready: 8 of 8
songs`, "no playback device" handled), map/variant enumeration — then fails
with `os error 2` (next section).

---

## ORIGINAL ANALYSIS: the gate (kept for reference)

The native client worker initialized graphics, the menu, and audio, then
refused with `Error: Native transport instruction/import mismatch; refused`.
This section records what the check was.

## Where it sits in the boot flow

The worker is spawned by the client as:

```
project-reclaimer probe-engine-worker "<…/halo3/halo3.dll>" \
    --guardian-slayer --client-window --startup --shell-callbacks --native-udp
```

The experiment flags form a **locked chain** (from the exe's own help strings):

- `--client-window` → "our own window + native rendering, without the MCC shell"
- "Standalone client window requires Guardian" → needs `--guardian-slayer`
- "Guardian experiment requires --native-udp and its startup prerequisites"
- "Native UDP requires shell callbacks and render warmup bypass"
- "Native experiments require --startup"

So the window path **cannot** drop `--native-udp`. The transport gate is on the
critical path to showing the window.

Immediately before the refusal the log prints:

```
Initialized standalone Rust window/native graphics contract; no MCC executable launched.
Error: Native transport instruction/import mismatch; refused
```

With `PELOAD_TRACE_ALL=1`, there are **no Winsock/socket/WSA calls and no
VirtualProtect/CoCreate calls** between window-init and the refusal — the check
reads process memory **directly** (via `ReadProcessMemory`, which we implement
as an in-process `memcpy`) and compares against hard-coded expectations.

## Related strings (one .rdata blob)

```
Cannot bind local transport on
Cannot protect native transport worker memory
Cannot finalize native transport worker patch
Native transport instruction/import mismatch; refused
Invalid native transport patch size
ws2_32.dll
Winsock module missing
bind
```

Sibling gate elsewhere, same shape:

```
XAudio2 import slots differ from the supported build; refused
```

Both validate **import slots / instructions against "the supported build"** —
i.e. Reclaimer pins the exact MCC `halo3.dll` layout (consistent with the
hard-coded SHA-256 binary pins noted in `docs/rcon-capabilities.md`).

## What halo3.dll imports from WS2_32

IAT at RVA `0x6fa6b8` (VA `0x1806fa6b8`), ~29 entries, mostly by ordinal:

```
ord 14,8,15,9,57,115,116,52, WSAIoctl, getaddrinfo, freeaddrinfo,
ord 23,151,18,20,17,19,111,16,1,13,3,22,4,2,10,21,7,11
```

Our loader fills those IAT slots with **stub pointers** (our stub pool), not
real Winsock. The gate reads the slots and/or the bytes they point at.

## Disassembly of the loopback-bind path (context)

`r2` on the exe, function at `0x1400d5960` (reached on the native-udp path).
This is the **loopback bind** the transport performs, not the gate itself, but
it shows the shape of the native transport:

```
ReadProcessMemory(GetCurrentProcess(), rdx, &buf, 16)   ; read a sockaddr
cmp [rbp+0x10], 0x10        ; 16 bytes read
cmp word [rbp], 2           ; AF_INET
… build sockaddr_in:
  word [rbp+0x20] = 2       ; AF_INET
  word [rbp+0x22] = <port>  ; from the read
  dword [rbp+0x24] = 0x0100007f   ; 127.0.0.1  (loopback forced)
call rbx                    ; rbx = ws2_32 bind (from [0x14228ae78])
… on bad socket: WSASetLastError(0x2726 = WSAENOTSOCK), return -1
```

So the transport itself is loopback-only UDP — which BSD sockets on macOS could
service. The blocker is the **pre-validation** gate, not the socket I/O.

The error string is not loaded by a direct `lea` to its VA nor a `(ptr,len)`
&str constant — it is sliced from a concatenated .rdata blob by a selector, so
the exact compare constants live in the gate function (to be pinned down).

## Options

1. **Satisfy the gate** by making halo3.dll's WS2_32 IAT / the pointed-at
   instructions match the supported Windows build. We are not Windows and do
   not ship `ws2_32.dll`, so matching real Winsock prologues is not feasible.
   (Loading a real `ws2_32.dll` guest PE is also a dead end: its bodies do real
   Windows socket syscalls.)

2. **Neutralize the gate in the client exe** (the user's own copy; interop /
   fair-use research). Locate the gate function (near `0x1400d5xxx`, the native
   experiment patch logic) and patch the refuse branch to accept. Fragile, and
   later gates (`--probe-admission`, `--real-client`) likely follow.

3. **Render without the full Guardian chain.** The window + menu are created
   *before* the gate. If the menu can present a frame without the native
   transport, the "startup screen" goal is met with far less fighting. Requires
   (a) a real visible window (see below) and (b) getting the worker to the
   present loop without the transport — e.g. by making the gate's refusal
   non-fatal or by driving the menu render directly.

## Separate remaining work for a *visible* screen (DONE 2026-10-11, `548d4ce`)

Done: a real NSWindow + GL context (later the present thread and the D3D11->GL
renderer). As written at the time, even past the gate, nothing was shown yet:

- `CreateWindowExW` is a stub returning a fake HWND — no Cocoa window exists.
- The GL context is an **offscreen CGL context**; `SwapBuffers`→`CGLFlushDrawable`
  has no on-screen drawable.

To actually display: create an `NSWindow` + `NSOpenGLView` (or a CAOpenGLLayer),
bind the existing CGL context to its drawable, and present there. The engine
already renders egui/glow frames into the context; the bytes are being produced.
A cheaper interim proof is to `glReadPixels` one rendered frame and write a PNG.

---

## FIXED (2026-10-11, `2a8f65e`): `Error: System error 2 (os error 2)`

Resolution: `PELOAD_TRACE_EMPTY=1` (the exe return addresses of a
`CreateFileW("")`) showed the empty path was cloned from an empty global
String, the **playlist path**. The loader now stages
`C:\ReclaimerContent\default.playlist.json` (an empty rotation) and redirects
empty-path opens to it (`PELOAD_NO_EMPTY_FIX` = old; `/dev/null` when the file
is missing). The same commit made `GetEnvironmentVariableW` report
ERROR_ENVVAR_NOT_FOUND for unset variables (Rust saw `Some("")` for every one)
and `GetModuleFileNameW` return the exe path. The findings as written before
the fix:

The worker exits 1 with this after audio init. Findings so far:

- It is a Rust `io::Error` whose code comes from **`GetLastError()` == 2**
  (ERROR_FILE_NOT_FOUND) — read at `exe+0x18d9183`, the tail of a
  `CreateFileW`-based `File::open` wrapper (confirmed by r2: `call CreateFileW;
  cmp rax,-1; je <read GetLastError>`). So it is a **file** open, not a socket
  error (socket errors would be WSA 10xxx via WSAGetLastError).
- Per-thread `lastfail` tracking shows the failing path is **empty** —
  `CreateFileW("")` → `File::open("")`. The empty open happens twice on the
  worker; the first is handled (menu is still created afterwards), the second
  propagates fatally.
- Not caused by: missing fonts (redirected), the ws2_32 sockets (made real),
  cross-thread `GetLastError` pollution (made `g_lasterr` `__thread`),
  `Program Files (x86)` vs `Program Files` (symlinked), missing content/
  discovery dirs (created + `RECLAIMER_CONTENT_DIR`/`RECLAIMER_DISCOVERY_DIR`
  set), or `GetFinalPathNameByHandleW` format (`fl=0`, full DOS path returned
  correctly).
- Most likely an **unset path** the engine reads and opens directly
  (`File::open(env::var(...).unwrap_or_default())` → `""`), or a path built
  from a lookup that returned empty. Hard to isolate because the error thread
  interleaves with the map-variant enumeration thread in the single combined
  log.

### Diagnostics available (env-gated)
- `PELOAD_TRACE_FS=1` — CreateFileW/NtCreateFile/GetFileAttributes/find, with
  `pid`/`disp`/`acc` tags and ENOENT markers.
- `PELOAD_TRACE_LE=1` — logs when `GetLastError` returns 2, with the caller
  module+offset and the thread's last-failed path (`CFW:`/`NT:`/`GFA:`;
  `<empty>` for empty paths).
- `PELOAD_TRACE_RPM=1`, `PELOAD_TRACE_VA=1`, `PELOAD_TRACE_ALL=1`.

### Next ideas
- Add **thread-id** to the FS/LE traces to isolate the error thread's ops from
  the enumeration thread (the single biggest blocker to pinning it).
- Or break in lldb on the `CreateFileW` wrapper (`exe+0x18d9140`) with a
  condition that the first arg (the UTF-16 path) is empty, and walk the Rust
  stack to the `File::open("")` call site.
- Then supply whatever path/env the engine expected.

### Separately, for a *visible* screen
Even past this, nothing is shown yet: `CreateWindowExW` is a stub and GL renders
to an offscreen CGL context (no `SwapBuffers`/`glClear`/`glDraw*` were ever seen
in the trace — the engine exits before its present loop). A real Cocoa `NSWindow`
bound to the CGL context, or a one-frame `glReadPixels` → PNG, is the remaining
work to actually see the startup screen.
