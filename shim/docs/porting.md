# Porting the loader to a new Project Reclaimer client release

Servers refuse an older client within hours of a release, so every release has to be checked and, when something
moved, ported the same day. This is the whole procedure: what the loader changes in the client, the scripts that find
it in a new build, how to do it by hand when they can't, and how to tell a right answer from a wrong one. Nothing here
needs anything outside this repository, its scripts and the release downloads.

Contents: 1 what is patched and hooked · 2 the scripted port (commands) · 3 worked example: 0.9.22 -> 0.9.23 ·
4 by hand · 5 errata: false positives and false negatives · 6 coverage of every release · 7 bookkeeping · 8 tool
reference · 9 how sigport works (and its limits)

## 1. What the loader changes in the client exe

The client exe is a Rust program; the loader changes it **in memory** at load, never on disk. Every place is found by a
**byte signature** when the exe is loaded (no fixed address survives a release: patch-addresses.md section 4). Two kinds:

| point | kind | where in the sources | what it does | without it |
|---|---|---|---|---|
| ui-reentrancy | patch (2 NOPs) | peload.c `g_sigpatches` | the menu's once-a-second cache cleanup panics ("RefCell already borrowed") on our first-call path | the client's menu never draws |
| xaudio1..4 | patch (NOP 4 `jne`) | `g_sigpatches` | the client checks halo3's four XAudio2 callback slots against the retail DLL before installing its own audio hooks | "XAudio2 import slots differ from the supported build; refused" |
| watchdog | patch (`je` -> `jmp`) | `g_sigpatches` | the worker's engine hang watchdog ("still running after N seconds") | only RECLAIMER_DIRECT=1 runs (the menu path never arms it) |
| set_muted | call | stats.c `g_sig_set_muted` | the stats page's mute buttons call the client's own set_muted | no mute from the page |
| voice_recv | entry hook (19 bytes) | `g_sig_voice_recv` | talking indicator, forensic voice archive | none of those |
| push_history | entry hook (19 bytes) | `g_sig_push_history` | chat / [SERVER] / notice lines in the event feed and forensic archive | no chat in the feed |
| moderation | data (found by code, no signature) | stats.c `st_addr_install` | player addresses on a host's stats page | none shown |
| max_players | call + data | `g_sig_max_players` | the joined server's player limit ("slots") | the session's limit instead |
| cast_vote / ballot | call + data | `g_sig_cast_vote`, `g_sig_ballot` | PELOAD_AUTOVOTE_MAP (vote for a map card) | no map auto-vote |

**Signature rules** (`sig_parse` / `sig_find` in peload.c): hex bytes separated by spaces, `??` = any byte; the first byte
must be literal; a signature counts only when it matches **exactly once** in the exe's executable sections (overlapping
matches counted). A `g_sigpatches` row is `{name, slot, on-by-default, signature, n, {offsets}, {original bytes},
{replacement bytes}}`: the loader checks the original bytes at each offset before writing. A slot may have several rows
(variants: the shapes different builds compile the code to); the loader tries **every** row and applies each that matches
with the expected bytes, so a build should match exactly one per slot. The stats.c lists are tried in order and the
first signature that matches once **and passes the point's meaning check** wins (section 5 lists the checks).

## 2. The scripted port

```bash
bash shim/tools/client-update.sh                 # the new release into client/ (checks its SHA256SUMS.txt)
bash shim/tools/fetch-releases.sh                # the same + every older release into the corpus (data only, never run)
bash shim/build.sh
bash shim/tools/client-survey.sh                 # the loader's own [sigpatch] / [stats] lines for old and new, import diff
python3 re-tools/patchscan.py --diff client/project-reclaimer-client-v<old>.exe client/project-reclaimer-client-v<new>.exe
```

`patchscan --diff` prints one row per point. "moved, same signature" or "another variant" = nothing to do.
**BROKEN** = that point's signatures don't find it in the new build. For each BROKEN row:

```bash
python3 re-tools/sigport.py --point <row> --old client/...v<old>.exe --new client/...v<new>.exe --corpus
#   reads the place in <old> (from the current signatures), finds it in <new>, prints a new signature, its checks and
#   the C to paste; "status: ok" + "meaning_check: ok" = trustworthy. Then the same with --apply writes it into
#   shim/src (a g_sigpatches row after that slot's last one, or a line in the stats.c list), tagged "v<new>: ...".
bash shim/build.sh && python3 re-tools/patchscan.py --diff client/...v<old>.exe client/...v<new>.exe   # all rows found now
```

Then the live check (it starts our local dedicated server on the new exe if it isn't running, and ONE hidden,
input-less client; it refuses while the user's game runs):

```bash
bash shim/tools/port-verify.sh                   # PASS / FAIL / SKIP per point; exit 0 = no FAIL
```

It also catches what no signature can: 0.9.23's first live run drew one draw a frame in the menu (a new client feature
deadlocked on a loader COM bug, section 3). Finish with the bookkeeping (section 7).

If sigport answers "port by hand" (the code changed too much, or two places look alike), go to section 4. If
port-verify FAILs a point whose signature PASSed, the signature found the wrong place (section 5) or something else
changed (read the client log it names).

## 3. Worked example: 0.9.22 -> 0.9.23 (2026-10-24)

0.9.23 SHA-256 `ceef542ac17980d178e281ebf510b4aaaf28ef650a288f1d68e8250731ad7e70` (46,547,456 bytes; 0.9.22 was
`517ee179...`, 46,391,296). What the loader of the day before (commit abce1d1) found:

```
$ python3 re-tools/patchscan.py --rev abce1d1 --diff client/project-reclaimer-client-v0.9.22.exe client/project-reclaimer-client-v0.9.23.exe
```

| point | v0.9.22 | v0.9.23 | how |
|---|---|---|---|
| ui-reentrancy | 72960f | 72b06e | moved, same signature |
| xaudio1 | 6015e0 v1 | 600670 v1 | moved, same signature |
| xaudio2 | 60160f v1 | 60069f v1 | moved, same signature |
| xaudio3 | 601646 v1 | 6006d6 v1 | moved, same signature |
| xaudio4 | 60167b v1 | 60070b v1 | moved, same signature |
| watchdog | 8713e4 v1 | MISS:none | **BROKEN**: port it (re-tools/sigport.py) |
| set_muted | 744580 | 7456b0 | moved, same signature |
| voice_recv | 757cf0 | 75a950 | moved, same signature |
| push_history | ce5bc0 | cf5c20 | moved, same signature |
| moderation | 2ae5d18 +170 | 2b0b0e8 +170 | moved, same signature |
| max_players | 737c80 +233 | 7396e0 +233 | moved, same signature |
| cast_vote | a03ce0 3f8 | a0f920 3f8 | moved, same signature |

(`v1` = the second row of that slot; `+170` = players Vec at lock+0x170; `+233` = max players at Arc+0x233; `3f8` = the
ballot cache's size.) Every address moved; only the watchdog needs work. What changed there (re-tools/hdis.py on both):

```
0.9.22  exe+0x8713da  81 BD ?? ?? ?? ?? 00 CA 9A 3B   cmp dword [rbp+0xba0], 1000000000   ; Option<Instant> deadline: None?
        exe+0x8713e4  74 ??                           je   (no deadline)                   <- the patch: je -> jmp (EB)
        exe+0x8713e6  E8 ?? ?? ?? ??                  call Instant::now
        exe+0x8713eb  90 48 39 D8 75 ??               nop ; cmp rax,rbx ; jne               ; deadline seconds in rbx (v0.9.20 row)
0.9.23  exe+0x87acdb  81 BD ?? ?? ?? ?? 00 CA 9A 3B   cmp dword [rbp+0xbc8], 1000000000
        exe+0x87ace5  74 ??                           je
        exe+0x87ace7  C6 85 ?? ?? ?? ?? 01            mov byte [rbp+0xc04], 1              ; NEW: a flag set before the clock read
        exe+0x87acee  E8 ?? ?? ?? ??                  call Instant::now
        exe+0x87acf3  90 48 8B 8D ?? ?? ?? ?? 48 39 C8 nop ; mov rcx,[rbp+X] ; cmp rax,rcx  ; the deadline reloaded from the frame again
```

The new `mov byte` breaks every older row (`74 ?? E8 ...`), and the comparison went back to the frame form. sigport, as it would have run that morning:

```
$ python3 re-tools/sigport.py --rev abce1d1 --point watchdog --old client/...v0.9.22.exe --new client/...v0.9.23.exe --corpus
status: ok
name: watchdog
site: 0x87ace5
sig: 81 BD ?? ?? ?? ?? 00 CA 9A 3B 74 ?? C6 85 ?? ?? ?? ?? 01
start: 0x87acdb
offs: [10]
orig: ['74']
repl: ['EB']
score: 1.0
runner_up: 0.333
runner_func: 0x1395db0
evidence: 43.26
func: 0x879500
meaning_check: ok
corpus:
    v0.9.23 0x87ace5 new

paste into shim/src/peload.c g_sigpatches (after the last watchdog row):
  {"watchdog","watchdog",0,"81 BD ?? ?? ?? ?? 00 CA 9A 3B 74 ?? C6 85 ?? ?? ?? ?? 01",
     1,{0xa},{"74"},{"EB"}},
(or rerun with --apply)
```

How to read it: `score` 1.0 = the old window's code is all there (instruction shapes); `runner_up` 0.333 = the next-best
function aligns far worse, so the answer isn't a coin toss; `evidence` = the anchors pointing at that function (its "still
running after" text and constants); `meaning_check: ok` = patchscan's checks pass with only this signature; `corpus` =
it matches no other release (`new`), so it can't double-apply anywhere.

The same place as the hand-made row committed for 0.9.23 (which is longer: it also pins the rest of the loop, like the
older rows). Either works; `--apply` writes sigport's.

**What the signatures could not show**: port-verify's first live run had the menu at 1 draw a frame. 0.9.23 added a
constant-buffer upload filter (it patches the D3D11 context's Map/Unmap and attaches a "resource watcher" COM object to
each buffer with SetPrivateDataInterface, then releases its own reference). Our SetPrivateDataInterface didn't AddRef, so
that Release freed the watcher, whose cleanup took a lock the filter already held: the render thread deadlocked. Fixed in
the loader (FIXES.md section 10: private-data interfaces are referenced like Windows does); with it the menu draws ~620 a
frame and loads in ~20 s. That is why the live check is part of every port.

## 4. By hand

Use this when sigport says "port by hand", for a point sigport doesn't know, or to check its answer yourself.

**4.1 Find the place in the OLD build.** `python3 re-tools/patchscan.py client/<old>.exe` prints every point's RVA (patches:
the byte changed; calls / hooks: the function entry). Look at it: `python3 re-tools/hdis.py client/<old>.exe <rva-0x40>-<rva+0x40>`.

**4.2 Find it in the NEW build by what the code means**, not by its bytes (patch-addresses.md section 6 rule 7, most
reliable first):

| point | anchor that survives rebuilds | tool |
|---|---|---|
| xaudio1..4 | the halo3 constants 0x60c416 / 0x60c422 / 0x60c40a / 0x60c385 (expected callbacks) in the compare; one site each | `python3 re-tools/sigscan.py new.exe "16 C4 60 00"` |
| watchdog | the function loading "still running after"; inside it the `cmp dword [rbp+X],1000000000 ; je ; ... call` deadline test | `python3 re-tools/refs.py new.exe strref "still running after"` |
| ui-reentrancy | `mov byte [rbx+0x68],1 ; cmp qword [rbx],0 ; jne ; mov qword [rbx],-1 ; cmp dword [rbx+0x10],1000000000` (the RefCell cleanup) | sigscan with the bytes |
| set_muted | called by the mute UI ("scoreboard-mute", "Mute voice", "You cannot mute yourself"); takes a lock with `cl` that exactly one other function (is_muted) takes with `dl` | refs.py strref, then `re-tools/fcalls.py` on that function |
| voice_recv | the only non-UI caller of is_muted; called from the code that loads "Network receive" | `re-tools/callers.py new.exe <is_muted>` |
| push_history | its 35-byte prologue never changed; called 4-5 times; caps a deque at 100 entries | sigscan with the prologue |
| max_players | called from the code loading "Player limit check poisoned"; its first call returns an Arc clone (`call ; test rax,rax`) | refs.py strref + fcalls |
| cast_vote | called from the vote screen ("Most votes wins ..."); builds Message::Vote (`movabs rax,0x80000000000000xx ; mov [rbp-0x58],rax`) | refs.py strref + fcalls |
| moderation | the first `lock cmpxchg byte [rip+d],cl` before the code that formats "You are banned from this server. Reason: " | automatic (st_addr_install) |

`python3 re-tools/fanchors.py <exe> <function rva>` prints what identifies a function (its texts, its callers and their
texts, its callees): run it on the old build to choose anchors.

**4.3 Write a signature for the new build.** Start at the patched instruction (patches; a few instructions before it so
the window has some unique code) or at the function entry (calls, hooks) and add instructions until `sigscan.py` reports
exactly one hit. Wildcard (`??`): rel8 / rel32 branch and call targets, RIP-relative disp32, rbp / rsp stack displacements
of 32 bits, the frame size of `sub rsp,imm32` if it moves between builds. Keep literal: halo3 constants, the `00 CA 9A 3B`
Duration niche, field offsets that identify the code (ui-reentrancy's `C6 43 68 01`; without it the sibling cleanup on
+0x58 matches too). The first byte must be literal. An entry hook's first 19 bytes must be literal, end on an
instruction boundary, and hold no RIP-relative operand or relative branch (they are copied and run elsewhere).

**4.4 Test it without rebuilding** (patches): `PELOAD_PATCH=exe+<rva>:<hex bytes>` writes bytes at load, e.g. the 0.9.23
watchdog `PELOAD_PATCH=exe+87ace5:EB`, an xaudio NOP `exe+600670:909090909090` (a `jne rel32` is 6 bytes, `jne rel8` 2:
NOP exactly the instruction you found). `PELOAD_HOOK` / `PELOAD_BP=exe+<rva>` show whether code runs. A run:
`QR_HIDE=1 bash shim/tools/quiet-run.sh "PELOAD_PATCH=exe+87ace5:EB PELOAD_LOCAL_RUNTIME=__unset__" 90` (direct match).

**4.5 Make it permanent.** Paste a row after the slot's last row in `g_sigpatches` (peload.c), or a line before the `0`
of the point's `g_sig_<point>` list (stats.c), with a comment naming the build and what changed. `bash shim/build.sh`,
`patchscan --diff`, `port-verify.sh`.

**4.6 Patching the exe file itself** is never needed: the loader patches memory, so the file stays the release's (its
SHA-256 still matches the release, client-update.sh and play.sh's known-build list still recognise it, and every
signature row still finds the ORIGINAL bytes it checks). If you want a patched file anyway (an experiment on Windows):
the file offset of an RVA = RVA - VirtualAddress + PointerToRawData of the section holding it;
`python3 re-tools/pelib.py <exe> <rva>` prints the section table and the offset (0.9.23's watchdog `je` exe+0x87ace5 is
at file offset 0x87a0e5), then write the replacement bytes there (check the original bytes first, as the loader does).

## 5. Errata: false positives and false negatives

A **false negative** = the point is in the build but nothing finds it. A **false positive** = something is found, at the
wrong place. Both happened in the release history; the tools check for each.

**How each tool reports them**

| signal | where | means |
|---|---|---|
| `MISS:none` | patchscan | the point exists (its first release is before this one) but no signature matches: port it |
| `miss (known: <rva>)` | patchscan | the same, documented in patch-addresses.md section 3 (old builds no one runs): the anchor-found place |
| `orig!=` | patchscan; loader "unexpected bytes ... NOT applied" | a signature matches once but the bytes to replace differ: a false positive caught |
| `TWO variants` | patchscan | two rows of one slot match this build: the loader would patch both places (a too-loose new row) |
| `noisy` (a `*` after the RVA) | patchscan | works; another row matches elsewhere with other bytes, so the loader logs a harmless NOT applied |
| `no is_muted pair`, `clone shape`, `no Vote message`, `ballot size`, `prologue has ?? bytes` | patchscan; loader `[stats]` lines | a unique match that fails the point's meaning check: a false positive caught |
| `AMBIG` | patchscan; loader "signature ambiguous" | matches more than once: lengthen the signature |
| `CONFLICT <rva>` | sigport --corpus | the new signature matches an older release somewhere other than where the existing rows find the point: it is too loose |
| "two candidates align alike" | sigport | identical code in two places (Rust copies generic functions): sigport won't guess |
| `FAIL ... (patchscan and loader agree)` missing | port-verify stage 2 | the C (loader) and the Python (patchscan) implementations found different places: one of them is wrong |
| `FAIL` in stage 3 | port-verify | the point is found but doesn't work (or something else broke) |

**The meaning checks** (the same in stats.c and patchscan.py):

- set_muted: its first `lock cmpxchg byte [rip+d],cl` (within 0x40 bytes) locks a global that some `mov dl,1 ; xor
  eax,eax ; lock cmpxchg [rip+d],dl` also locks (is_muted). History: in 0.8.16 .. 0.9.1 the current set_muted shape also
  matched a Forge `Mutex<Vec>` setter (0x414520 / 0x951490 / 0x1b4f00), whose lock only `cl` code takes -- caught.
- voice_recv / push_history: the hit's first 19 bytes equal the pattern's (the entry hook compares and relocates them).
- max_players: the getter's first `call` followed by `test rax,rax` reaches a clone function with the Arc-clone
  prologue; a `movzx esi, byte [rax+d32]` within 0x100 bytes gives the offset (0x18 .. 0x1000).
- cast_vote: a Message::Vote build (`movabs rax,0x80000000000000xx ; mov [rbp-0x58],rax`) within 0x120 bytes.
- ballot: the cache .. receiver distance (the cache's size) 0x100 .. 0x1000 bytes.
- moderation: at least 3 `imul r64,[rip+L],0x68` read the players Vec (the real one: 6-7; in 0.8.9 / 0.8.11, before
  the table existed, one stray site matched -- caught).
- patches: the original bytes at each offset.

**Confirming a place yourself.** Disassemble it (`hdis.py`), compare with the shape in patch-addresses.md section 5, and
check the anchor: the watchdog's function loads "still running after"; an xaudio compare holds its halo3 constant; a
function point is called by the code its anchor names (4.2). Two independent ways agreeing (signature + anchor) is the
standard the history tables use ("CONFIRMED").

**Live symptoms of a wrong or missing point** (port-verify checks the first five):

| point | wrong / missing shows as |
|---|---|
| ui-reentrancy | the menu stays black or the client panics "RefCell already borrowed" |
| xaudio | the client log: "XAudio2 import slots differ from the supported build; refused" |
| join | "Joining server failed: ..." -- "Public join timed out" right after a killed client is the server still holding it (~45 s): retry |
| max_players | the stats snapshot's `slots.src` is "session" or "host" instead of "server" in a joined game |
| moderation | a host's stats page lists no addresses (`session[].ip`) |
| push_history | no chat lines in /feed.json (port-verify sends one with the server's RCON `say`); a wrong prologue crashes at the first chat line |
| voice_recv | no talking indicator / voice archive; a wrong prologue crashes at the first voice packet |
| set_muted | the page's mute does nothing; a wrong function corrupts memory (why the pairing check exists) |
| cast_vote | PELOAD_AUTOVOTE_MAP never confirms its vote; a wrong function crashes on the vote |
| watchdog | RECLAIMER_DIRECT=1 matches end after 15 / 45 / 240 s ("still running after"): PV_DIRECT=1 port-verify.sh checks it |

**Known non-failures** in port-verify: "Bink import slots differ from the supported build; refused" (every build since at
least 0.9.20: Workshop mods' Bink movies; not a port issue), the server browser's "query ... timed out" lines (other
servers on the internet).

## 6. Coverage of every release

`python3 re-tools/patchscan.py --md` over the corpus (every client release v0.8.1 .. v0.9.23, 39 builds) exits 0 with the
current sources: every point is found in every release from the first one that has it, except the old builds that
patch-addresses.md section 3 documents (shown as "miss (known: rva)"; nobody can run them: servers refuse old clients):

| point | found by today's signatures | documented misses (anchor-found places in patch-addresses.md section 3) |
|---|---|---|
| ui-reentrancy | v0.9.2 .. v0.9.23 (the code doesn't exist before) | -- |
| xaudio1..4 | v0.9.2 .. v0.9.23 | v0.8.1 .. v0.9.1 (ReadProcessMemory-based check, four encodings) |
| watchdog | v0.8.1 .. v0.8.12, v0.9.2 .. v0.9.23 | v0.8.13 .. v0.9.1 (merged tail) |
| set_muted | v0.9.2 .. v0.9.23 | v0.8.1 .. v0.9.1 (older prologue; the Forge lookalike rejected) |
| voice_recv | v0.9.15 .. v0.9.23 | v0.8.1 .. v0.9.13 (the frame grew release by release) |
| push_history | v0.9.11 .. v0.9.23 | v0.8.1 .. v0.9.10 (new arguments, a moved field) |
| moderation | v0.9.2 .. v0.9.23 (players at lock+0x78, +0xa8 from v0.9.16, +0x170 from v0.9.22) | -- |
| max_players | v0.9.2 .. v0.9.23 (Arc+0x17c .. +0x233) | -- |
| cast_vote | v0.9.15 .. v0.9.23 (ballot 0x2c0 .. 0x3f8 bytes) | -- |

The full per-release table is in patch-addresses.md section 3 (v0.9.23 included).

**sigport's history replay** (`python3 re-tools/sigport.py --selftest`): for every release where a point's signature
changed or never matched, it ports the point from the nearest release the loader supports, as if the target were
unknown, and compares with the right place (the loader's checked signature, or the anchor-found place of section 3):

160 ports, **84 right, 0 wrong, 76 "port by hand"** (2026-10-24, the sources of this commit; ~10 minutes):

| point | the ports | right | "port by hand" | wrong |
|---|---|---|---|---|
| watchdog | v0.8.12 -> v0.8.13 .. v0.9.1 (the merged tail), v0.9.18 -> v0.9.19 (deadline in rbx), v0.9.22 -> v0.9.23 | 8 of 8 | -- | 0 |
| xaudio3, xaudio4 | v0.9.2 -> each of v0.8.1 .. v0.9.1 (backwards: the ReadProcessMemory-era check) | 18 of 18 each | -- | 0 |
| xaudio1, xaudio2 | the same | 2 of 18 each (v0.8.15, v0.9.1) | 16 each | 0 |
| xaudio1..4 | v0.9.18 -> v0.9.19 (the check became a table-driven routine; its constants moved to data) | -- | 4 | 0 |
| set_muted | v0.9.2 -> each of v0.8.1 .. v0.9.1 (the older prologue; the Forge lookalike in 0.8.16 .. 0.9.1) | 18 of 18 | -- | 0 |
| voice_recv | v0.9.15 -> each of v0.8.1 .. v0.9.13 | 8 (v0.9.6 .. v0.9.13) | 22 (v0.8.1 .. v0.9.5) | 0 |
| push_history | v0.9.11 -> each of v0.8.1 .. v0.9.10 | 9 (v0.9.2 .. v0.9.10) | 18 (v0.8.1 .. v0.9.1) | 0 |
| max_players | v0.9.18 -> v0.9.19 (the getter also pushes rdi) | 1 of 1 | -- | 0 |

The real porting steps of the last months are in it: v0.9.19's watchdog and max_players (both right) and its xaudio rewrite
("port by hand": the v0.9.20 rows were written by hand), v0.9.23's watchdog (right). The refusals are the cases where the
code changed shape (a different implementation, a new frame layout); there sigport says so instead of guessing, and
section 4 applies. Before the caller-count factor (section 9) it gave 5 wrong answers (push_history in 0.8.x, a sibling
function); a run that reports any WRONG exits 1 and is a bug in sigport to fix before trusting it.

## 7. Bookkeeping (every port)

1. `shim/play.sh` `KNOWN`: add `<sha256> <version>` (else every launch warns "not a client build this loader was tested with").
2. README.md: the client version and SHA-256 in the requirements.
3. patch-addresses.md section 3: a `### Client exe, v<new>` table (the `patchscan --diff` output + a "how it is found
   now" column for anything that changed) and the new import / subcommand findings from client-survey.
4. FIXES.md: an entry for every loader fix (section 16 for signatures, the right section for the rest).
5. CLAUDE.md: a short note if the port changed how something works.
6. Commit (no user names, account names or player IDs in anything committed) and push.

## 8. Tool reference

| tool | does | runs the client? |
|---|---|---|
| shim/tools/client-update.sh [tag] | the newest (or a given) release into client/, SHA-256 checked | no |
| shim/tools/fetch-releases.sh [tag] | every release exe (+ SHA256SUMS.txt) into ~/Library/Application Support/ReclaimerMac/releases/<tag>/ | no |
| shim/tools/client-survey.sh [old new] | the loader's `--version` run on both (its [sigpatch] / [stats] lines), the import-table diff, the `dedicated` subcommand | `--version` / `--help` only |
| re-tools/patchscan.py | every point in any number of builds, with the loader's checks (`--diff A B`, `--md`, `--json`, `--rev <commit>`) | no |
| re-tools/sigport.py | ports one point to a new build (`--corpus`, `--apply`, `--selftest`, `--rev`) | no |
| shim/tools/port-verify.sh [exe] | static + loader + live checks, PASS / FAIL / SKIP (`PV_LIVE=0`, `PV_DIRECT=1`, `PV_RESTART_SERVER=1`, `PV_SECS`) | yes: one hidden client + the local server |
| re-tools/hdis.py | disassembly of RVA ranges (imports named) | no |
| re-tools/sigscan.py | every hit of a signature (unlike the loader: not only unique ones) | no |
| re-tools/refs.py / ripref.py / callers.py / fcalls.py / fanchors.py | string references, references into a window, callers, callees and strings of a function, what identifies a function | no |
| re-tools/peimps.py | a PE's imports | no |
| re-tools/imulscan.py | `imul r64,[rip+d],imm8` sites by global (the moderation table's Vec) | no |
| re-tools/pelib.py [exe rva..] | the shared PE reader the Python tools use (mapped like the loader maps it); as a command: sections + file offsets | no |
| shim/tools/winshot.sh | one frame of the game window from a short recording (UI checks) | uses a running one |

Old releases are data: never start one (`port-verify.sh` refuses corpus copies). The corpus lives outside the repo.

## 9. How sigport works, and its limits

1. **The old place**: the point's current signatures in the old build, accepted only when patchscan's meaning check
   passes there (a unique hit can be the wrong function).
2. **Candidates in the new build**, by evidence: the point's own anchors (the watchdog's "still running after"; for
   set_muted, functions whose `cl` lock some `dl` code shares); the distinctive constants (4- and 8-byte) and texts of the
   old window and function, each weighted by its rarity (1 / the number of functions holding it: 0x3b9aca00 is also Rust's
   `Option<Duration>::None` in hundreds of functions, a halo3 constant is in one); the old function's callers' texts,
   found in the new build, whose callees are candidates (a function with no texts is known by who calls it); for function
   points, functions starting with the same bytes (push_history's prologue never changed).
3. **Alignment**: instructions as shapes (mnemonic + operand kinds: register sizes, memory base class, distinctive
   constants; register names and displacements ignored), aligned with difflib. A patch / site: the old window + 8
   instructions each side, scored on the matching stretch of the candidate only (so a long function can't collect
   scattered matches). A function: the whole body, scored as 2 x matches / (old + new length), times 0.5 + 0.5 x the
   similarity of the two functions' direct-caller counts ((min + 1) / (max + 1)): before 0.9.2 push_history had a
   sibling with the same prologue and deque cap that aligned better (0.64 vs 0.50) but had 1 caller instead of 4-5.
4. **Confidence**: the best score must be at least 0.6 and beat every other candidate by 0.1; else "port by hand" (two
   identical copies of a function are never guessed between).
5. **Synthesis**: from the mapped instruction (patches: the same number of instructions before it as the old signature
   had), grow one instruction at a time until unique, wildcarding rel branch / call targets, RIP-relative and 32-bit
   frame displacements; at least 12 bytes (hooks: 19, literal, ending on an instruction boundary). A patch whose
   instruction changed length is re-derived (a `jne` NOP takes the real instruction length; `je` -> `jmp` for rel8 or
   rel32).
6. **Checks**: patchscan's meaning check with only the new signature; `--corpus`: every release it matches and whether
   that agrees with the existing rows.

Limits: sigport knows the points in section 1. A new kind of point needs its meaning check in patchscan.py (and the C)
first. Data found by code (moderation) is not ported by signature: if `st_addr_install` stops finding it, port its code
search by hand (FIXES.md section 16 describes it). The loader's halo3.dll patches are a different problem (MCC
updates, not Reclaimer releases): patch-addresses.md section 9.
