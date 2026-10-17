# Reclaimer client: the `workshop-download` helper and its stdout protocol (static RE)

Client `client/project-reclaimer-client-v0.9.18.exe` (ImageBase 0x140000000; addresses are exe RVAs). Found with
`re-tools/refs.py`, `re-tools/hdis.py`, `re-tools/decomp.py` and rip-relative/call xref scans; nothing was run.
CONFIRMED = read in the disassembly. GUESSED = inferred, check it at runtime before relying on it.

Purpose: replace the helper (which needs a running Steam client) with DepotDownloader, and write to the parent's
pipe exactly what a successful helper writes.

## Summary

- The helper prints **one compact JSON object per line** (`\n`-terminated, flushed after each line) on stdout. The
  type is a serde, externally tagged enum named `Message`:

  ```rust
  enum Message {
      Progress { done: u64, total: u64 },      // bytes, from ISteamUGC::GetItemDownloadInfo
      Ready    { root: PathBuf, subscribed: bool },
      Error(String),
  }
  ```

- It prints zero or more `Progress` lines, then exactly one `Ready` or one `Error`, then exits.
- The parent reads the pipe line by line, ignores lines that are not a valid `Message`, and stops at the first
  `Ready` or `Error`. Exit code and stderr are never looked at.
- After `Ready`, the parent uses the Steam folder **in place** ("Using Steam Workshop files directly.") only when
  `subscribed` is true **and** `canonicalize(root) == canonicalize(<mcc-path>\..\..\workshop\content\976730\<id>)`.
  Otherwise it copies the files into its download folder (Mods\Downloaded; the copy wiring is GUESSED).
- Under the loader today the parent cannot receive any of this: see section 6 (inherited stdio in play.sh,
  ReadFileEx on pipes, TerminateProcess).

Literal lines for a successful `--subscribe` of item 2933266434 with the default MCC path:

```
{"Progress":{"done":0,"total":1712345678}}
{"Progress":{"done":52428800,"total":1712345678}}
{"Progress":{"done":1712345678,"total":1712345678}}
{"Ready":{"root":"C:\\Program Files\\Steam\\steamapps\\workshop\\content\\976730\\2933266434","subscribed":true}}
```

A failure is a single line, for example `{"Error":"Steam Workshop download timed out"}`.

Our replacement, shim/tools/workshop-helper.sh (DepotDownloader), also makes the stats page's top-down pictures of the
item's maps once it has printed `Ready`: `maptop.sh <the item's .map files>` in its own session, off the client's pipe,
under native arm64 python (the script itself may run translated), log `$TMPDIR/reclaimer_workshop_<id>_maptop.log`.
`PELOAD_NO_MAPTOP=1` skips it. (Before, a downloaded map got its picture the first time it was played.)

## 1. The command line and how the parent spawns it (CONFIRMED)

Spawner **exe+0x803290** (`fn(out, self, item_id: u64, update: bool, cancel: &AtomicBool, progress_data,
progress_fn)`):

| Step | Where |
|---|---|
| program = `std::env::current_exe()` (the client itself) | 0x8032e9 |
| args: `workshop-download`, `--mcc-path`, `<self.mcc_path>` | 0x8034a5 / 0x8034bf / 0x8034f6 |
| `--update` if the `update` argument is true | 0x803501 |
| `--subscribe` if `self+0x20` (the "Prefer Steam Workshop" setting, GUESSED name) | 0x803522 |
| the item id, decimal (inline u64 itoa) | 0x80353c..0x803695 |
| env `SteamAppId=976730`, env `SteamGameId=976730` | 0x8036ed, 0x80374f |
| stdin = piped, stdout = piped, stderr = **Null** | 0x803787..0x8037ec (Command+0x58/+0x68/+0x78 = stdin/stdout/stderr, checked against the STD_INPUT/OUTPUT/ERROR ids in Command::spawn at 0x1d5b294..0x1d5b34f; values 3 = MakePipe, 2 = Null) |
| creation flags `CREATE_NO_WINDOW` (0x08000000) | 0x8037f6 |
| `Command::spawn` | 0x803822 |

Rust quotes the path, so the real command line is
`"<client exe>" workshop-download --mcc-path "C:\Program Files\Steam\steamapps\common\Halo The Master Chief Collection" [--update] [--subscribe] <id>`.

Who calls it:
- A trait object, vtable **exe+0x1f601a8** (`drop`, size 0x28, align 8, method0, method1):
  - method0 **exe+0x98860** = download: validates the Workshop source with 0x1765760 (item id must be 1..u64::MAX-1,
    package name at most 200 bytes, else "Invalid Workshop source"), then calls 0x803290 with `update = false`.
  - method1 **exe+0x98820** = update: calls 0x803290 with `update = true`.
  - The join dialog's mod-download routine **exe+0x6d5b90** calls method0 at 0x6d78f4 after setting the status
    "Downloading from Steam Workshop…".
- exe+0x963a40 (a dedicated/host Workshop provisioning routine that also knows SteamCMD) calls 0x803290 directly
  with `update = false` (GUESSED role).
- A second spawner, **exe+0x8096a0**, starts `workshop-download --mcc-path <path>` with **no item and no flags**, all
  three stdio handles Null. It runs at most once a minute, only when `<cache>\workshop-transport\` has entries, and
  waits up to 5 s for the process (WaitForSingleObject(0) + 100 ms sleeps, GetExitCodeProcess). In that mode the
  helper only deletes stale transport markers (section 4) and prints nothing. A replacement can just exit 0.

## 2. The helper side (CONFIRMED)

The helper body is case 0 of the subcommand switch in **exe+0xac2f80** (entry block 0xac303f; jump table
0x235dfa4). Its argument struct: `+0x08` Option tag of the item (1 = Some), `+0x10` item id, `+0x18..+0x28`
mcc_path PathBuf, `+0x38` update flag, `+0x39` subscribe flag. The flow, in order:

1. With an item: validate the source (0x1765760). Any error from here on goes to the common error exit (step 12).
2. `std::env::var("SteamAppId")` must be exactly `976730` (0xac30c0, compare at 0xac960d), else
   **"Workshop helper requires the MCC app context"**. `SteamGameId` is not checked by the helper.
3. `canonicalize(mcc_path.join("MCC/Binaries/Win64/steam_api64.dll"))` (0xace052) else "MCC's Steam runtime was not
   found"; `LoadLibraryExW(path, 0, 0x900)` (0xad08c3) else "Could not load MCC's Steam runtime".
4. GetProcAddress: SteamAPI_Init, _Shutdown, _ManualDispatch_Init, _GetHSteamPipe, _ManualDispatch_RunFrame,
   _GetNextCallback, _FreeLastCallback; with `--subscribe` also ISteamUGC_SubscribeItem and
   ManualDispatch_GetAPICallResult; then SteamAPI_SteamUGC_v021..v016, ISteamUGC_GetItemState, _DownloadItem,
   _GetItemDownloadInfo, _GetItemInstallInfo, SteamAPI_SteamUser_v023..v020, ISteamUser_BLoggedOn. A missing
   export -> "Steam runtime lacks {name}"; no UGC accessor -> "Steam UGC is unavailable".
5. `SteamAPI_Init()` false -> **"Start Steam and sign in with an account that owns MCC to use Workshop"**
   (0xad439f -> 0xad55fe). Then ManualDispatch_Init, `SteamUGC()`.
6. No item: transport-marker cleanup (section 4), exit with no output.
7. `state = GetItemState(id)`; `marker = <cache>\workshop-transport\<id>` (0x1764950; `<cache>` =
   `RECLAIMER_CACHE_DIR` or a folder under `%LOCALAPPDATA%`, 0x1764e20).
8. `--subscribe` and not yet subscribed (`state & 1` clear): `SubscribeItem(id)` (0xad5c22; call handle 0 -> "Steam
   refused the Workshop subscription"), then pump callbacks every 100 ms for up to **60 s** waiting for
   SteamAPICallCompleted_t (703) -> RemoteStorageSubscribePublishedFileResult_t (1313); `m_eResult != 1` or another
   item -> "Steam Workshop subscription failed ({EResult})"; 60 s -> "Steam Workshop subscription timed out".
9. Subscribed (before or now): delete a stale `marker` if present; **subscribed = true**.
   Not subscribed: with `--update` just **subscribed = false**; otherwise create `marker`'s folder and write it with
   the text `Temporary Workshop transport; installed files are in Mods/Downloaded.\n` (0xad6b4a), subscribed =
   false.
10. `DownloadItem(id, true)` (0xad63c7). False -> if BLoggedOn() is false "Steam is offline: go online in Steam, or
    sign in again if it signed you out, to use Workshop", else "Steam refused to update the Workshop item"
    (`--update`) / "Steam refused the Workshop download".
11. Loop, every **200 ms** (0xad6405), at most **1800 s** (0xad6425, else "Steam Workshop download timed out"):
    RunFrame + drain callbacks looking for DownloadItemResult_t (3406, size 0x18) with `m_unAppID == 976730` and our
    id. Not finished yet: if `GetItemDownloadInfo(id, &done, &total)` returns true, print
    **`{"Progress":{"done":done,"total":total}}`** (0xad6548), every iteration, changed or not.
    Finished: `m_eResult != 1` -> "Steam Workshop download failed ({EResult})"; else
    `GetItemInstallInfo(id, &size, folder[0x1000], 0x1000, &ts)` false -> "Steam did not provide an installed
    Workshop folder"; folder not UTF-8 -> "Invalid Steam folder"; folder not absolute or not a directory -> "Invalid
    Steam Workshop folder"; else print **`{"Ready":{"root":folder,"subscribed":...}}`** (0xad6e8b), flush, call
    SteamAPI_Shutdown, FreeLibrary, return Ok.
12. Common error exit (0xad3e25 -> 0xac968a): SteamAPI_Shutdown if initialised, FreeLibrary, print
    **`{"Error":"<message>"}`** (0xac96f0), return Err(message). The dispatcher (exe+0xaf82ba) then eprintln!s
    `Error: <message>` to stderr (NUL here) and returns 1; Ok returns 0 (GUESSED: that value is the process exit code).

The writer, **exe+0x80ac10**, locks `io::stdout()` and writes the JSON by hand with serde_json's compact formatter:
`{`, `"Progress"`, `:`, `{`, `"done":<u64>`, `,"total":<u64>`, `}`, `}`; or `{"Ready":{"root":<string>,`
`"subscribed":true|false}}` (the PathBuf goes through `to_str`; non-UTF-8 is a serialisation error); or
`{"Error":<string>}`; then `\n` (0x80b24f) and a flush (0x80b28a). No spaces anywhere. Strings are JSON-escaped
(backslashes doubled).

## 3. The parent side (CONFIRMED unless marked)

### Reading the pipe

After spawning, 0x803290 takes `child.stdout` (no stdout -> "Workshop helper has no output"), creates a bounded
channel of 32 `Message`s (0x600-byte buffer at 0x803955) and starts a reader thread (CreateThread at 0x804023;
closure vtable 0x2062470, call_once 0xcd2b00 -> 0xc0f7b0 -> **exe+0x805310**). The reader:

- reads with `ReadFileEx` (8 KiB, OVERLAPPED zeroed) and waits in `SleepEx(INFINITE, TRUE)` until the completion
  routine has run (0x80549c..0x8054c3). This is Rust's blocking `AnonPipe::read`.
- splits on `\n`, at most 16 KiB per line (0x805421), checks UTF-8, then `serde_json::from_str::<Message>`. JSON
  whitespace (space, tab, CR, LF) around the object is fine, so `\r\n` works.
- a line that is not a valid `Message` (unknown variant, missing field, plain text) is **dropped and reading
  continues** (0x805e00..0x805e74). All four fields are required: there are "missing field" errors for `done`,
  `total`, `root`, `subscribed` (0x807504, 0x8077a2, 0x807a6f, 0x807c37). Unknown fields are allowed
  (GUESSED, serde default).
- EOF, a read error or invalid UTF-8 ends the thread, which drops the channel sender.

### The wait loop (0x804058..0x8043fc)

Every 100 ms (`recv_deadline(now + 100 ms)`, 0x804176..0x80421d) it checks, in this order:

| Condition | Result |
|---|---|
| cancel flag set (`*[rbp+0x5a0]`) | Err("Cancelled") |
| no increase of `done` for **120 s** while `done` is still 0, or for **45 s** after it moved | Err("Steam Workshop download timed out") (0x8040d4: `mov ecx,120`; 0x8040de: `mov ecx,45`; 0x8040f2 second copy of 120) |
| **1800 s** since the spawn | same error (0x80414e `cmp rax,0x708`) |
| channel disconnected (reader ended) with nothing final received | **Err("Steam Workshop is unavailable")** (0x8043ca; the only reference to that string) |
| `Progress{done,total}` | if `done` > the last value (unsigned): remember it and restart the stall timer. Always call the progress callback (0x80408d -> exe+0x6da160, which posts a progress event for the dialog). |
| `Error(s)` | Err(s) |
| `Ready{root,subscribed}` | Ok (below) |

`Ready` handling (0x8042ea..0x80457d):

```
steam_copy = subscribed
          && canonicalize(<mcc_path>.parent().parent() / "workshop/content/976730/<id>")   // 0x804332..0x804367, 0x809440
               == canonicalize(root)                                                         // 0x8044a2, Option::zip 0xe92ca0, components eq 0x805050
return Ok { child, root (as printed, not canonicalised), steam_copy }                       // 0x80453f..0x80457d
```

Both sides go through `std::fs::canonicalize` (exe+0x1d64a00 = CreateFileW + GetFinalPathNameByHandleW). If either
fails, `steam_copy` is false.

### What the join dialog does with it (exe+0x6d5b90)

- Ok -> status "Verifying Workshop files…". It resolves the server's package inside the item with exe+0x6c42b0:
  `canonicalize(root)`, then `canonicalize(root.join(package))` when the server names a sub-folder, which must stay
  inside root ("Workshop package escapes its item").
- Package missing (0x6d7454): "Workshop package missing; downloading from server…" when the byte at
  `[rbp+0x7cc]` is set (GUESSED: the server shares files), else Err("The Workshop item does not contain this
  package, and direct sharing is disabled on this server.").
- Package present and `steam_copy` (test at 0x6d754e) -> exe+0x6c4900 verifies the files in place ("Steam Workshop
  files changed during verification. Try again once the update finishes." if they change) -> status
  **"Using Steam Workshop files directly."** (0x6d7a1b).
- Package present and not `steam_copy` -> `create_dir_all([rbp+0x5a8])` (0x6d757f; GUESSED: the download folder),
  then (GUESSED wiring, from the functions and status strings) the copy path: exe+0x6c1e20 copies each file through
  a `workshop-part` temporary and checks it ("Workshop file size differs", "Workshop file digest differs", "Workshop
  file ended early"); missing or differing files come from the server ("Workshop differs; downloading remaining
  files from server…"); `reclaimer-workshop.json` is written (0x6d8ca7). This is the "Mods/Downloaded" copy the
  marker text refers to.
- GUESSED: the installed check is the per-file size + SHA-256 comparison with the server's manifest (mods.md); this
  path does not read `ModInfo.json` itself.

### The child afterwards (exe+0xdeb580)

The `Child` stays inside the Ok value and is dropped later; error, timeout and cancel paths drop it at once
(0x80444c). The drop guard closes the helper's stdin pipe, polls `WaitForSingleObject(h, 0)` 6 times and
`try_wait` 4 times with 25 ms sleeps (about 250 ms), then **`Child::kill()` = TerminateProcess** (0x1d534e0) and
waits. The exit code is never used.

## 4. Transport markers (CONFIRMED)

`<cache>\workshop-transport\<id>` exists while a non-subscribed ("temporary") download is in use; the files live in
Mods\Downloaded. The helper deletes it once the item is subscribed (step 9), and the no-item helper (spawner
0x8096a0) walks the folder and deletes the markers of items that `GetItemState` now reports as subscribed
(0xad615c..0xad6293). A DepotDownloader replacement can ignore markers.

## 5. What a replacement must print

For `workshop-download --mcc-path <M> [--update] [--subscribe] <id>`:

1. While downloading: `{"Progress":{"done":<bytes>,"total":<bytes>}}` lines. `done` must grow at least every 45 s
   (the first growth within 120 s of the spawn), and the whole run must finish within 30 min, or the parent gives
   up. The timeouts are the imm32 operands at exe+0x8040d5 / exe+0x8040f3 (120), exe+0x8040df (45) and exe+0x804150
   (1800) if they ever need a PELOAD_PATCH (untested).
2. When the files are on disk: `{"Ready":{"root":"<W>","subscribed":true}}` with
   `<W>` = `<M>` minus its last two components + `\workshop\content\976730\<id>`, e.g.
   `C:\\Program Files\\Steam\\steamapps\\workshop\\content\\976730\\2933266434` (JSON-escaped). Derive it from `<M>`
   rather than from the `Program Files (x86)` link: the shim's GetFinalPathNameByHandleW returns the path as opened
   (no symlink resolution), so both sides of the comparison must name the same path. `subscribed:false` (or a
   root that does not match) still works but makes the client copy everything into Mods\Downloaded.
3. Then exit within ~250 ms.
4. On failure: `{"Error":"<text>"}` (shown to the user), then exit.
5. No item: print nothing, exit 0.

## 6. Shim problems on this path (CONFIRMED in shim/src/peload.c, not fixed here)

- `PELOAD_WORKER_INHERIT_STDIO=1` (set by shim/play.sh and shim/target.sh) makes i_CreateProcessW skip the dup2 of
  the STARTUPINFO std handles. The real helper's JSON then goes to the loader's log instead of the parent's pipe,
  the parent reads EOF, and the dialog shows "Steam Workshop is unavailable" -- the observed behaviour. An
  intercept has to write to the `hStdOutput` handle from STARTUPINFO (0x1000+fd, a socketpair end).
- i_ReadFileEx does `pread()` at OVERLAPPED.Offset whenever an OVERLAPPED is passed. The parent's reader always
  passes one, and the pipe is a socketpair, so pread fails (ESPIPE), ReadFileEx returns FALSE and the reader thread
  ends: "Steam Workshop is unavailable" even with correct output. Stream fds need `read()` (block, call the
  completion routine with the byte count, return TRUE; at EOF complete with 0 bytes or ERROR_BROKEN_PIPE 109,
  which Rust treats as EOF). i_ReadFile already does this for overlapped stream reads.
- i_TerminateProcess ignores its handle and `_exit()`s the loader. The drop guard in section 3 calls it whenever the
  helper is still alive ~250 ms after being dropped (always on timeout/cancel), which would close the game.
  It needs `kill(pid)` for `0x8000+pid` child handles.
- i_CreateProcessW applies the child's environment block with `setenv()` in the loader itself, so `SteamAppId` /
  `SteamGameId` stay set in the client after the first helper spawn (GUESSED harmless).
