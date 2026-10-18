# Easy Anti-Cheat (EOS) in MCC: what it does, what it sends, how its driver gets loaded

Static analysis (2026-10-17) of `easyanticheat_eos_setup.exe` as shipped with Halo: The Master Chief Collection. It
was done after users asked whether the file is malware or a rootkit, and why Project Reclaimer works without it.
Nothing was executed: the binary was only read as data.
- **Tools:** an angr call graph of the whole binary (4,240 functions), capstone, and a small interpreter for its
  obfuscated strings.
- **Addresses:** virtual addresses (ImageBase 0x400000; the file is 32-bit x86).
- **Build:** its own BuildTime string says Sep 6 2024. Other builds may differ.

| | |
|---|---|
| File | `easyanticheat\easyanticheat_eos_setup.exe` (identical copy in `installers\`), 964,336 bytes |
| SHA-256 | `652761ed1fa44955ffde4c3daeb0654937f7cdef7a3a05ddf509c2e707f46e0d` |
| `easyanticheat\settings.json` | target `MCC\Binaries\Win64\MCC-Win64-Shipping.exe`, productid `65f3348d…`, sandboxid `185b7367…`, deploymentid `2aa54985…` |

Summary:
- **Telemetry is upload-only.** Every HTTP request is a POST. The response body is buffered and thrown away
  unread; only the HTTP status code is looked at. Nothing a server sends back can make the service do anything.
- **Telemetry fires in two cases only:**
  - a Windows **bug check (blue screen)** happened since the service last started (within 24 h);
  - reading the **TPM 2.0 endorsement-key certificate fails**.

  Nothing is uploaded on a normal day. Nothing is uploaded about games, other processes or files.
- **Crash data:** for a blue screen it sends the bug-check record. If the crash stack involves EAC's own driver,
  it also sends:
  - the symbolic stack backtrace (`k`) from the crash dump;
  - up to ten 16-bit EAC status codes found on the crashing stack.

  It never sends the dump file, raw memory, or the 8 KB stack dump it inspects.
- **Code with SYSTEM / kernel rights does not come from telemetry.** The kernel driver is handed to the SYSTEM
  service by a process on the same PC, through a shared memory section. The service writes it to disk, checks its
  Authenticode signature against an Epic / EAC publisher list, then loads it as a kernel driver.
- **Usable local surface.** That shared section and its events can be written by **any local user, including
  Guest and anonymous tokens**, and Everyone may start the service. Any local process can therefore make SYSTEM
  load *any* Epic/EAC-signed kernel driver (no version pin on that path). That's a bring-your-own-vulnerable-driver
  concern, not arbitrary code. There is no remote / network attack surface.

## 1. Why Reclaimer (and this Mac port) never touches it

MCC's EAC integration is in **MCC's own executable**. `MCC-Win64-Shipping.exe` imports these from
`EOSSDK-Win64-Shipping.dll`:
- `EOS_Platform_GetAntiCheatClientInterface`
- `EOS_AntiCheatClient_BeginSession` / `EndSession`
- `ReceiveMessageFromServer`
- `AddNotifyMessageToServer` / `RemoveNotifyMessageToServer`
- `AddNotifyClientIntegrityViolated` / `RemoveNotifyClientIntegrityViolated`

The `[AntiCheat] Integrity violation:` handler lives there.

The Reclaimer client never starts that exe. It loads `halo3.dll` itself. Our loader's log of every DLL loaded
during a session shows:
- `halo3.dll`
- `bink2w64.dll`
- `xaudio2_9redist.dll`
- the Waves DLLs
- `api-ms-win-*` shims

No `MCC-Win64-Shipping.exe`, no `EOSSDK-*.dll`, no `easyanticheat*`. Neither `halo3.dll` nor the client imports
anything EOS-related.

So the anti-cheat client API is **not stubbed, not NOP'd and not failing quietly**: none of that code is in the
process at all, and the integrity-violation callback is never registered.

Two unrelated "anti-cheat" names show up:
- **`halo3.dll`'s `net_config_disable_bad_*_anticheating_override` strings.** These are Halo 3's original
  Xbox-era peer-quality checks (bad connectivity / bandwidth / client).
- **Reclaimer's own "Anti-cheat: off / detect / enforce" server setting.** Reclaimer's own feature.

Neither has anything to do with EAC.

## 2. One binary, two roles

**Setup tool.** Commands: `install|uninstall <productid>`, `repair`, `verify-installation`, `verify-and-install`,
`qa-factory-reset`.
- **Install, `0x416e40`.** Creates `%ProgramFiles(x86)%\EasyAntiCheat_EOS\` and copies itself there as
  `EasyAntiCheat_EOS.exe`. Then `0x418c10` calls `CreateServiceW`:
  - service `EasyAntiCheat_EOS`, "Easy Anti-Cheat (Epic Online Services)", own-process service running as SYSTEM;
  - then `0x424a60` adds an ACE for **Everyone (S-1-1-0)**: `SERVICE_START | SERVICE_STOP | READ_CONTROL`
    (`0x20030`, SET_ACCESS). Any user can start and stop the service; they cannot reconfigure it.
- **Elevation.** When not elevated, `0x416bd0` re-runs itself with `ShellExecuteExW("runas")`, which is the
  normal UAC prompt.
- **Uninstall, `0x4177c0`.** Deletes the files. Its own exe is moved to `%TEMP%\EasyAntiCheat_Setup.exe.tmp`.

**Service** (`ServiceMain` = `0x420870`, registered as `EasyAntiCheat_EOS`):
- **Start argument = the game's process id.** `0x41c5b0` then runs once:
  - starts the TPM check thread (`0x41bfd0`);
  - runs the blue-screen report (`0x41c7a0`);
  - opens the game process (SYNCHRONIZE only);
  - creates the shared section and three events (section 6).
- **Main loop.** Waits on: stop, game-process-exit and `EventModule`.
  - `EventModule` -> `0x41dca0`, the driver load (section 5).
  - Then waits on `EventGame` -> `0x41da10`, which watches the game PID written into the section until it exits.
  - Its process "checks" only compare process image paths with the game's image path (`K32EnumProcesses` +
    `K32GetProcessImageFileNameW`), to see whether the game is still running.
  - The service stops when the game exits.
- **Two start arguments = self-update** (`0x420380`, section 5).

## 3. Telemetry, traced end to end

### 3.1 The HTTP client (every network operation in the binary)
libcurl 7.69.1 is statically linked: `0x401647` refuses any other version (`0x074501`). TLS is Schannel.

The service wraps it in one class, vtable `0x4b461c`:

| slot | function | does |
|---|---|---|
| init | `0x40ab00` -> `0x401647` | `curl_easy_init` and its options; see below |
| deinit | `0x40ab20` | |
| clear headers | `0x40ab30` | |
| GET | `0x40ab80` | `CURLOPT_HTTPGET`. Present in the vtable but **never called**: the binary makes no virtual calls on this class, and its two users call POST directly |
| POST | `0x40abe0` | request headers (`CURLOPT_HTTPHEADER`), then `CURLOPT_POST`, port, `CURLOPT_POSTFIELDSIZE` + `CURLOPT_COPYPOSTFIELDS` |
| perform | `0x4019a0` | `CURLOPT_TIMEOUT_MS`, `CURLOPT_URL`, `curl_easy_perform`; on failure retries through each system proxy (`CURLOPT_PROXY` / `NOPROXY`); logs "Connect result … Response Code … Destination IP" |

Options set at init:
- `NOSIGNAL`;
- `PROTOCOLS` = HTTP|HTTPS;
- a header callback `0x403260`, which stores response headers into a map at object+0x10c;
- a write callback `0x403580`, which appends the response body to a buffer at object+0x114;
- an error buffer;
- `SSL_OPTIONS = NO_REVOKE`.

**What happens to a response.** Nothing reads the header map or the body buffer. The class has no method that
returns them; `perform` only clears them before a request; the destructor frees them. Both senders do the same
after `perform`:
1. `curl_easy_getinfo(CURLINFO_RESPONSE_CODE)`;
2. treat 200–299 as success;
3. destroy the object.

**Reachability.** On the call graph:
- `perform` is called only by GET and POST;
- POST is called only by the two senders below;
- `socket`/`connect`/`send`/`recv` and the like are reachable from the service's roots (`ServiceMain`, the TPM
  thread, the report) **only through `perform` or curl's init**. With those two blocked, no root reaches a socket
  call.

`listen`/`accept` appear only in curl's loopback socket-pair helper, used for its resolver thread. The service
opens no listening port.

### 3.2 Endpoints

| sender | URL (assembled on the stack from 16-byte pieces) | live? |
|---|---|---|
| `0x40d2b0` (called by `0x40c020`) | `https://gossip.easyanticheat.net/datarouter/api/v1/public/data/?AppVersion=2.0.0&AppEnvironment=Production&UploadType=eteventstream&AppID=` | **yes**: the one used |
| `0x40f3e0` (URL from `0x40f030`, choice 0) | `https://api.epicgames.dev/telemetry/data/datarouter/api/v1/public/data?AppVersion=1.0.0&AppEnvironment=Production&UploadType=sdkevents&AppID=EOSSDK.AntiCheatExt.Service&SessionId=` | no, see below |
| `0x40f3e0` (choice 1) | `https://datarouter.ol.epicgames.com/datarouter/api/v1/public/data?AppVersion=1.0.0&AppEnvironment=Production&UploadType=eteventstream&AppID=easyanticheat&SessionId=` | no |

Request details:
- **Headers:** `Content-Type: application/json`.
- **User-Agent:** `Easy Anti-Cheat/1.0.0` (gossip) or `Easy Anti-Cheat EOS/1.0.0`.
- **Timeouts:** connect 2.5 s; a low-speed abort under 30 B/s for 2 s.

**The two Epic endpoints are dead code in this build.**
- Their caller `0x40dd70` returns at once when its ProductId string (`0x4c0c58`) is empty, and nothing in the
  binary ever writes that string.
- The same holds for the AppID (`0x4c0c28`) and SessionId (`0x4c0c40`) strings appended to the URLs, and for
  SandboxId / DeploymentId (`0x4c0c88` / `0x4c0c70`).
- **Evidence:** a scan of every section finds only their readers and their exit-time destructors. Every request
  therefore goes to gossip.easyanticheat.net with an empty `AppID=`.

### 3.3 What triggers a send: exactly two events
The event-name table `0x40cbd0` lists 19 names, from `AntiCheat.Error.IntegrityViolation` to
`…ServiceUpdateFailed` and `…DriverInitResult`. It has exactly two callers, each passing a constant:

| caller | code | event | when |
|---|---|---|---|
| `0x41c7a0` | 2 | `AntiCheat.Error.BugcheckDetailed` | at service start (always under WOW64, i.e. 64-bit Windows), once per blue screen found in the event log |
| `0x41bfd0` | 18 | `AntiCheat.Warning.EKCertRetrievalFailed` | at service start, only if the TPM is 2.0 and `TpmCoreProvisioning!TpmRetrieveEkCertificate` fails |

No other event in the table is produced by this binary.

Payload (`0x40c020`): `{"Events":[{"EventName":…,"BuildTime":…,"OS":"Windows <maj>.<min> <32/64-bit> (Build <n>)", <attributes>}]}`.
The OS version comes from the PEB.

**EKCertRetrievalFailed** carries:
- `N1` = the failure HRESULT;
- `N2`, `N3` = two numbers from `Tbsi_GetDeviceInfo`.

The certificate itself is never sent. On success nothing is sent.

### 3.4 BugcheckDetailed: exactly what is read from crash reports

1. **The event-log query.** `0x4088e0` -> `0x412860` / `0x412f90` load `wevtapi.dll` and run one query on the
   System log:
   - provider `Microsoft-Windows-WER-SystemErrorReporting`, source `BugCheck`, EventID `1001`;
   - `TimeCreated` within the time since the service last started (from its registry bookkeeping), capped at 24 h.

   That is the "the computer has rebooted from a bugcheck" record. No application crashes and no other logs.
2. **Attributes per record** (built in `0x41c7a0`; two-letter keys):
   - `D1`: the record's time (ISO 8601, `0x411900`);
   - `D2`: a time from the service's own state;
   - `N1`–`N3`, `S1`–`S5`: fields of the bug-check record and of the service's own registry bookkeeping under
     `HKLM\SOFTWARE\EasyAntiCheat_EOS`: `ErrRpt_LastInitTime`, `ErrRpt_LastInitInfo1/2`,
     `ErrRpt_LastInitProductId`, `ErrRpt_LastProductId`.

     Numbers are printed as `%u` / `0x%X` / `0x%llX`. Which slot holds which value was not mapped one by one.
   - **Read but not sent:** `0x405df0` also reads `ErrRpt_GameCrash*` (game PID, exit status, Info01–13), but no
     event this build sends carries them.
3. **The matching crash dump.**
   - **Location:** from `HKLM\SYSTEM\CurrentControlSet\Control\CrashControl`: `CrashDumpEnabled`, `DumpFile`,
     `MinidumpDir`.
     - Settings 1, 2 and 7 (complete / kernel / automatic): `MEMORY.DMP`, if its file time fits (`0x407920`).
     - Setting 3 (small dumps): `MinidumpDir\*.dmp` (`0x4083c0`).
   - **Opening it.** `0x4079c0` loads `dbgeng.dll` and calls `DebugCreate` for `IDebugClient4`, `IDebugControl4`
     and `IDebugSymbols3`. It opens the dump and checks it is the right type, then runs these debugger commands.
     They are hidden in the binary as xorshift/LCG-encrypted strings and decrypted just before use.

     | step | command | use |
     |---|---|---|
     | 1 | `.bugcheck` | the code must equal the event-log record's, or the dump is skipped |
     | 2 | `.for( r $t0 = 0; @$t0 < 512; r $t0 = @$t0 + 1 ) { ln poi( rsp + 8 * @$t0 ) }` | resolves the 512 stack slots of the crashing thread to nearest symbols; the text output is searched case-insensitively for `easyanticheat` |
     | 3, only if found | `k` | the stack backtrace (module!function+offset per frame); its first, header line is removed and the rest becomes **`S7`** |
     | 4, only if found | `dp rsp L1024` | 8 KB of raw stack as text, **not sent**: `0x4074d0` keeps only values of the form ``00000000`0eacXXXX`` (EAC's own status codes) and sends up to ten `XXXX` joined by `-` as **`S6`** |

   - **Never read or sent:** the dump file itself, raw memory and the 8 KB stack dump.
   - **When `S6`/`S7` are sent:** only when EAC's driver is on the crashing stack. They describe the kernel stack
     at the time of the crash: module and function names, and EAC codes.
   - **Symbols:** there is no symbol-server URL in the binary; `ln` and `k` use whatever symbol path dbgeng picks up.

## 4. Correction of an earlier statement

An earlier answer said Epic "can deliver new signed code that runs as SYSTEM or in the kernel", which read as if it
came through telemetry. **That was wrong about telemetry.** The telemetry code receives nothing (section 3.1).

Code with elevated rights reaches this service only:
- as the kernel driver handed over by a local process (section 5);
- or as a new service binary named in the service's own start arguments (section 5.2).

Both must pass the same Authenticode + publisher check.

## 5. How a kernel driver gets loaded (`EventModule` -> `0x41dca0`)

1. **Hand-over.** A process on this PC writes the driver blob into the shared section
   `Global\EasyAntiCheat_EOSBin` (section 6) and signals `Global\EasyAntiCheat_EOSEventModule`. In normal use that
   is the game side: the EOS SDK / bootstrapper.
2. **Size check and de-obfuscation.** `0x41dca0` checks the size and de-obfuscates the blob (around `0x41dda1`).
   "Invalid binary size." if the check fails.
3. **Write to disk.** Written as `%ProgramFiles(x86)%\EasyAntiCheat_EOS\EasyAntiCheat_EOS.sys` (`0x41e08e` /
   `0x41e0e7`).
4. **Reopen for validation.** Reopened `GENERIC_READ` + `FILE_SHARE_READ` (`0x41e155`), so nobody can change it
   while it is checked and loaded.
5. **Signature check.** `0x41bcd0` runs it on a worker thread with a 5 s timeout ("Code signing certificate
   validation timed out"):
   - `0x41be10`: `WinVerifyTrust` via `wintrust.dll` (`0x4121e0`, must report a trusted chain);
   - the signer's name (`0x412350`);
   - **publisher allowlist `0x412550`:** `EasyAntiCheat Oy`, `Epic Games Inc.`, `Epic Games, Inc`,
     `Epic Games, Inc.`, `Epic Games Inc` ("Unknown driver certificate publisher: '%s'.").
6. **Load.** `0x41e3d0`:
   1. `OpenSCManagerW`;
   2. `CreateServiceW("EasyAntiCheat_EOSSys", SERVICE_KERNEL_DRIVER, demand start, <that path>)`;
   3. `StartServiceW`;
   4. `DeleteService`, so the driver entry does not persist.
7. **Reply.** A status record is written back into the section and `Global\EasyAntiCheat_EOSEventDriver` is set.

**No version or hash pinning was found on this path.** Any driver that passes step 5 is loaded, including an older
signed one.

**Where the game side gets the driver bytes is not traced here.**
- MCC's launcher `mcclauncher.exe` carries the EOS module CDN template
  `https://modules-cdn.eac-prod.on.epicgames.com/modules/{{productid}}/{{deploymentid}}/{{system}}`.
- Whether the driver comes from there or is shipped with the game was not checked.

### 5.2 Service self-update (`0x420380`, service started with two arguments)
1. The service validates the file named by its second argument with the same check (4.5 s timeout).
2. It requires the product name `Easy Anti-Cheat Service (EOS)` and compares the two binaries' file versions.
   The exact rule wasn't decoded; it looks like "newer only".
3. It replaces its own exe: `MoveFileExW(…, MOVEFILE_REPLACE_EXISTING)`, keeping a `.bak` / `.tmp` copy.

Nothing is downloaded by the service itself.

## 6. `Global\EasyAntiCheat_EOSBin` and its events: who uses them, and is it usable?

**Created in `0x41c5b0`.**
- A 64 MB page-file section. Header: version 1, size, two version-like words, and the game PID at +0x10.
- Plus `…EventModule`, `…EventDriver` and `…EventGame`.
- All of them use the SDDL `D:(A;OICI;GA;;;BG)(A;OICI;GA;;;AN)(A;OICI;GRGWGX;;;AU)(A;OICI;GA;;;BA)`:

| trustee | rights |
|---|---|
| Built-in Guests | generic all |
| Anonymous logon | generic all |
| Authenticated Users | read / write / execute |
| Administrators | generic all |

This SDDL is used for these four objects only. The service and its registry key use other descriptors.

**Users.**
- The service itself (section 2).
- The game-side EAC component that hands over the driver and the game PID.

**What any local process can do while the service runs.** That includes a Guest account; Everyone may also start
the service, with any PID as its argument.
- **Make SYSTEM load an Epic/EAC-signed kernel driver of its choice.** Steps 2–6 of section 5. With no version pin,
  that includes an *older* signed EAC driver. If such a driver had an exploitable bug, this is a local privilege
  escalation path ("bring your own vulnerable driver"). It is limited by:
  - whether such a driver exists (not checked);
  - Microsoft's vulnerable-driver blocklist and HVCI.
- **It cannot load unsigned or third-party code this way.** The trust check plus the publisher allowlist stand in
  the way, and the share-read reopen closes the swap-after-check race.
- **It can disrupt the anti-cheat.** Garbage blobs, fake events, stopping the service. That's a denial of service
  against the game's protection, not against Windows.
- **Nothing reachable from the network.** No listening sockets; telemetry responses are discarded.

## 7. Not established
- The exact meaning of each BugcheckDetailed slot `N1`–`N3` / `S1`–`S5`.
- The exact version rule of the self-update.
- The internals of the de-obfuscation in step 2.
- Where the game side obtains the driver.
- What the driver itself does. It is not in this file; this file only loads it.

The method has its limits. A static call graph follows direct calls and the vtables / callbacks named above. An
indirect call it cannot see could in principle add a path. None was found around the HTTP class: its only
constructor users are the two senders, and no code calls through its vtable.
