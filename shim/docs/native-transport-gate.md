# The `--native-udp` native-transport integrity gate

Status: **current blocker** (2026-10-11). The native client worker initializes
graphics, the menu, and audio, then refuses with:

```
Error: Native transport instruction/import mismatch; refused
```

and the worker exits with code 1. This document records what the check is, the
evidence gathered, and the options.

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

## Separate remaining work for a *visible* screen

Even past the gate, nothing is shown yet:

- `CreateWindowExW` is a stub returning a fake HWND — no Cocoa window exists.
- The GL context is an **offscreen CGL context**; `SwapBuffers`→`CGLFlushDrawable`
  has no on-screen drawable.

To actually display: create an `NSWindow` + `NSOpenGLView` (or a CAOpenGLLayer),
bind the existing CGL context to its drawable, and present there. The engine
already renders egui/glow frames into the context; the bytes are being produced.
A cheaper interim proof is to `glReadPixels` one rendered frame and write a PNG.
