# Event feed: chat, HUD message lines, kill events (static RE)

Static RE only, 2026-10-17. Tools: capstone over every `.pdata` function of both binaries, `re-tools/refs.py`,
`re-tools/hdis.py`, `re-tools/hsfuncs.py`, `re-tools/h3map.py`. No process was run and no live memory was read.
Binaries:
- `$HOME/Library/Application Support/ReclaimerMac/prefix/drive_c/Program Files/Steam/steamapps/common/Halo The Master Chief Collection/halo3/halo3.dll`
  (base 0x180000000, addresses `halo3+RVA`)
- `client/project-reclaimer-client-v0.9.18.exe` (base 0x140000000, addresses `exe+RVA`)

**CONFIRMED** means it was read from the code, and the evidence is given. **GUESSED** means the name or meaning is
inferred. "PIC" means position-independent: the bytes can be copied into `entry_hook`'s trampoline unchanged.

## 1. Chat (Reclaimer's own text chat, not halo3)

Chat is drawn by the client's own egui panel (`src/chat_ui.rs`), not by halo3's HUD. Every line that panel shows
(chat lines, `[SERVER]` lines and local notices) is appended to its history by **one function**:

**`exe+0xb89930` TextChat::push_history(chat, line, mine, t_secs, t_nanos)** (`src/text_chat.rs`; the name is GUESSED)

| Arg | Win64 | Meaning |
|---|---|---|
| `chat` | rcx | the client's TextChat state (the `Mutex` data inside the remote-host connection `Arc`, which is the `Arc` + 0x2c0 in `exe+0x692240`). `*(u8*)(chat+0xf8) == 0` means the history is disabled and the line is dropped (checked at `b89953`) |
| `line` | rdx | `Line*`, 0x40 bytes, **moved** (the function takes ownership; copy what you need, do not free it) |
| `mine` | r8b | 1 = a chat line the local player sent (set by the caller when `line.sender == own id` and the variant is Chat) |
| `t_secs`, `t_nanos` | r9, `[rsp+0x28]` at entry | a timestamp (u64 and u32; nanos 1000000000 = none). Stored at entry +0x40 / +0x48. The UI fades lines with it. Meaning GUESSED |

Prologue, 27 bytes, all PIC. Copy 19 bytes (the eight pushes and `sub rsp`) for `entry_hook`:
```
b89930  55                      push rbp
b89931  41 57                   push r15
b89933  41 56                   push r14
b89935  41 55                   push r13
b89937  41 54                   push r12
b89939  56                      push rsi
b8993a  57                      push rdi
b8993b  53                      push rbx
b8993c  48 81 ec a8 00 00 00    sub rsp, 0xa8          <- 19 bytes end here
b89943  48 8d ac 24 80 00 00 00 lea rbp, [rsp+0x80]
```
`entry_hook(exe_base+0xb89930, pro, 19, fn)` calls `fn(rcx, rdx, r8, r9)` with the arguments untouched. The hook
must only read `line`.

**`Line` (0x40 bytes, Rust enum, niche in the first qword).** CONFIRMED from `exe+0x2da7e0` (the chat panel
drawer: [TEAM]/[NEARBY]/[SERVER] prefixes at `2db082`/`2dad72`/`2dae9f`) and from `push_history`'s drop code.
`q0 = *(i64*)line`:

| `q0` | Variant (serde names `Chat`, `Notice`, `Server`, table at `exe+0x205eaf8`) | Layout |
|---|---|---|
| >= 0 | **Chat** (5 fields, "struct variant Line::Chat with 5 elements") | +0x00 String **sender name** {cap, ptr +0x08, len +0x10}, UTF-8. +0x18 String **text** {cap, ptr +0x20, len +0x28}, UTF-8. +0x30 u64 **sender id** (the session peer id that the roster rows hold at +0x88/+0x90, the same id the voice mute list uses, see `reclaimer-voice-mute.md`). +0x38 u8 has_color, +0x39..0x3b name colour RGB. +0x3c u8 **channel: 0 all, 1 team ("[TEAM] "), 2 nearby ("[NEARBY] ")** |
| 0x8000000000000000 | **Notice** (local status line) | +0x08 u8 kind: 0 Throttled, 1 TeamUnavailable, 2 ProximityUnavailable, 3 Disabled, 4 Undelivered (string table at `exe+0x205eb60`; texts like "You are sending messages too quickly.") |
| 0x8000000000000001 | **Server** (shown as "[SERVER] " + text) | +0x08 String {cap, ptr +0x10, len +0x18}, UTF-8 |

The strings are **UTF-8 Rust Strings, not wide chars and not NUL-terminated**: use (ptr, len).

**Callers.** All five are CONFIRMED by an `e8` call scan:

| Call site | Function | What it pushes |
|---|---|---|
| `exe+0x683951` | `exe+0x682f60`, the client's handler for messages from the host (`src/text_chat.rs` line 763; jump table on the decoded message variant at `6838c7`) | **every received chat or server line**. `mine` = (variant is Chat) && (`line+0x30` == own id) |
| `exe+0xb89731` | `exe+0xb896c0` (host side: deliver a line to one peer) | lines the **host** delivers to itself, because a listen host has no network round trip |
| `exe+0x69175a` | `exe+0x6914c0` | Notice kinds 1 and 3 (send failures) |
| `exe+0x68a144` | `exe+0x6895e0` | Notice kind 4, Undelivered (no ack within about 5 s) |
| `exe+0x691db3` | `exe+0x691c80` | Server lines produced locally (`exe+0xb89c10`; GUESSED: the replies to chat commands such as /help) |

On a dedicated server the lines come from the server, through `exe+0x682f60`. The text chat runs in the process
that loads halo3.dll. That process owns the remote-host connection `exe+0x68c790`, the same one the teleport code
uses (`teleport.md`). GUESSED, so check it at runtime.

**Join/leave lines are different:** they go into halo3's HUD feed, see 2. The client's chat panel never sees them, and
`push_history` does not catch them.
