# Where the menu render stands (2026-10-11, overnight session)

## TL;DR

Halo 3's **real MCC engine runs on Apple Silicon with no Wine**, and the native
client worker now boots the **entire** startup path:

- loads `halo3.dll`, runs its self-patches (private-match hooks), passes the
  `--native-udp` anti-tamper gate,
- initializes audio (XAudio2 shimmed), loads the **main-menu map** (`mainmenu`),
  menu artwork ("loading rings"), text packages, connects controllers,
- creates an **OpenGL 4.1 Core** context (CGL), compiles all egui/glow shaders,
  and builds a **COMPLETE** menu framebuffer (after the MSAA sample-count fix),
- reaches the **menu-setup stage**.

It does **not** yet draw a frame. At menu setup the engine hits **racy Rust
panics / C++ exceptions** (`BTreeMap` navigate, `Option::unwrap` on `None`,
uncaught C++ throws). Because the shim has **no SEH / C++ exception unwinding**,
these can't be caught by Reclaimer's `catch_unwind` — they `std::terminate` the
thread (SIGILL, `0x7ff8…` trap) before any `glDrawElements` is issued. Confirmed
by instrumentation: `glDrawElements` is **never called**.

## The single remaining blocker: Windows SEH / C++ exception handling

On Windows x64, a Rust panic (panic=unwind) and a C++ `throw` both raise
`RaiseException(0xE06D7363, …)` and unwind via the OS: `RtlDispatchException`
walks `.pdata`/`.xdata`, calls each frame's language handler
(`__CxxFrameHandler3`), runs cleanup, and transfers to the matching `catch`
(`catch_unwind`). Our shim **decodes** the throw but does not **dispatch/unwind**
it, so the C++ runtime falls through to `std::terminate`.

Implementing this is the big remaining piece and is well-defined but large:
`RtlLookupFunctionEntry`, `RtlVirtualUnwind`, `RtlUnwindEx`, and the
`__CxxFrameHandler3` personality (the MSVC C++ EH state machine). This is the
part of `ntdll`/`msvcrt` that Wine implements.

Two consequences of not having it:
1. The engine's *normal* non-fatal throws (asset-missing, control flow) abort.
2. Reclaimer's menu-setup panics (whatever their root) can't be caught/logged
   and continued, so the worker dies.

Whether the menu-setup panics are *caused* by a concurrency bug in the shim or
are genuine logic the engine recovers from via `catch_unwind` on real Windows,
**SEH unblocks both**: with unwinding, a caught panic lets the worker continue.

## What was fixed to get here (this session)

- GLSL `#version<150` rewrite by resolved pointer in `make_gl_thunk`.
- `.text` self-patch: page-align `VirtualProtect`; `FlushInstructionCache` =
  `sys_icache_invalidate`.
- WASAPI COM (empty `IMMDeviceEnumerator`) so cpal stops panicking.
- ws2_32 resolved through one ordinal table behind `WS2_HANDLE` → the
  `--native-udp` import-integrity gate passes; then real BSD-socket ws2_32.
- `GetEnvironmentVariableW` sets `ERROR_ENVVAR_NOT_FOUND` on miss (Rust
  `env::var` was returning `Some("")` for every unset var).
- `GetModuleFileNameW/A` return the exe path (not the command line); Rust
  `current_exe()` works.
- Playlist: staged a valid empty-rotation playlist; empty-path `File::open`
  redirects to it (the empty global path *was* the playlist path).
- BCrypt via CommonCrypto (anti-cheat hashing).
- `WriteProcessMemory` keeps the target page RWX (was downgrading to r-x and
  faulting the `*lpNumberOfBytesWritten` store).
- Bind **delay imports** (dxgi/d3d11); minimal `IDXGIFactory` (EnumAdapters →
  none) + minimal **XAudio2** (IXAudio2 + voices) so the engine's probes don't
  deref NULL.
- GDI DIB surface (CreateDIBSection/…); the menu's CPU bitmap allocates.
- **MSAA sample clamp** → the menu FBO is COMPLETE (was
  `GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT 0x8cd6`).
- Missing GL funcs resolve to **NULL** (not a no-op stub) so glow detects
  capabilities correctly.
- **Thread-safe** GL thunk / stub pools: permanent RWX + mutex (the old
  RW↔RX toggle faulted code executing on another thread — a real source of
  random multi-thread crashes).
- Real **WaitOnAddress / WakeByAddress*** futex (were no-ops; Rust `std::sync`
  depends on them on Windows 8+).
- Real **NSWindow + NSOpenGLContext** path (`PELOAD_WINDOW`), autorelease-pool
  wrapped; frame-capture infra (`PELOAD_SHOT`, resolve-blit MSAA→single →BMP).

## How to reproduce / continue

```
cd ~/Desktop/HaloReclaimerMac
MCC="$HOME/Library/Application Support/ReclaimerMac/prefix/drive_c/Program Files/Steam/steamapps/common/Halo The Master Chief Collection"
( cd "$MCC" && PELOAD_WINDOW=1 RUST_BACKTRACE=full \
  arch -x86_64 ~/Desktop/HaloReclaimerMac/shim/build/peload \
  ~/Desktop/HaloReclaimerMac/analysis/project-reclaimer-v0.9.13.exe )
```

Useful env flags: `PELOAD_WINDOW` (real window), `PELOAD_SHOT` (frame capture to
`~/reclaimer_menu_latest.bmp`), `PELOAD_TRACE_GL`, `PELOAD_TRACE_FS`,
`PELOAD_TRACE_VA`, `PELOAD_WAIT=<s>` (worker sleeps so lldb can attach, writes
pid to /tmp/peload.pid).

### Next steps, in priority order
1. **Implement SEH/C++ EH** (`RtlVirtualUnwind` + `RtlUnwindEx` +
   `__CxxFrameHandler3`). This is the unblock: panics get caught, the engine
   survives menu setup and reaches the draw loop. The capture infra
   (`PELOAD_SHOT`) is already in place to grab the first frame.
2. If SEH is too big, pin the menu-setup panic source with `RUST_BACKTRACE`
   address mapping + r2 disassembly, and provide whatever data it wants.
3. For a *visible* (not just captured) window, run the AppKit runloop on the
   main thread while the engine runs elsewhere (or capture via `PELOAD_SHOT`).
