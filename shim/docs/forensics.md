# Forensic archive of voice, chat and game events (+ viewer)

`shim/src/forensic.c` (included by peload.c right after stats.c) keeps an archive of what this client receives from the
server it is connected to -- voice packets, chat lines, the game's event feed and who was in the session -- so abusive or
cheating players can be documented afterwards. The viewer is `http://127.0.0.1:8765/forensics` (linked at the top of
the stats page). Written 2026-10-18 (backlog 43); tested offline only (see "Testing" and "Not verified yet").

## What is and is not captured

- **Only what is addressed to this client.** Voice: every packet the client's own `voice_recv` (exe+0x684080 in 0.9.18,
  found by signature, stats.c `st_on_voice`) is handed. The hook runs at the function's entry, BEFORE the client's own
  checks, so packets from players this client has muted, packets for channels it would not play and its own echo are
  archived too. Chat: every line the client's chat panel gets (`TextChat::push_history`, stats.c `st_on_chat`), with the
  sender's peer id. Events: every line of the stats page's event feed (stats.c `st_event`): joins, leaves, kills and
  deaths, votes, `[SERVER]` lines, chat notices, match start / end.
- Nothing is sent anywhere; nothing is read from other processes or the network beyond what the client already gets.
- **Who fans voice out is not known yet** -- whether each speaker's client sends a copy to every listener or a server
  relays them. Each voice datagram carries a sender id (datagram +0x14) and a recipient id (+0x1c) that the receive loop
  compares with its own id (reclaimer-voice-mute.md section 3), so packets are addressed per recipient either way. A
  speaker the server or the speaker's client does not send to this client (e.g. the other team's team channel) is never
  in the archive.
- Voice audio is the Opus bytes exactly as received (no decoding, no re-encoding). It is only stored with the **voice
  audio** switch on, which requires the **one-party consent** box (below). Without it the archive still keeps who talked
  when: time, sender, channel, sequence, packet length and the Opus TOC byte(s) (mode / bandwidth / frame size), but no
  audio.
- Needs the stats server (`PELOAD_STATS_HTTP`, on by default in play.sh and quiet-run.sh): its hooks feed the archive and
  its 100 ms snapshot thread supplies the roster. Only the process that loads halo3.dll archives.

## Consent

Voice on a public server is transmitted to everyone in the channel it is sent on (global, team or proximity); once the
client has joined, it is one of the recipients the packets are addressed to -- like a scanner tuned to talk groups, it
keeps what it was sent and nothing else. Recording what a participant receives is allowed in one-party-consent places
such as Ohio (Ohio Revised Code 2933.52); some places require every party's consent, and whether in-game voice counts as
a private conversation there can vary (not legal advice). The user is in Ohio (2026-10-18), so:

- with no `settings.json` yet, one-party consent and voice audio start ON (the time is recorded as the confirmation);
  unticking "One-party consent: I'm a participant in the channels sent to this client and choose to record them" on the
  viewer page turns voice audio off (only who talked when is kept); ticking it again asks for confirmation;
  `PELOAD_FORENSIC_AUDIO=0|1` overrides voice audio for one run;
- persists the confirmation with its time (`one_party_consent: true`, `consent_confirmed_unix`) in
  `forensics/settings.json` and copies it into each session's `session.json`;
- refuses `voice=1` without consent on record; withdrawing consent turns voice audio off at once;
- notes every setting change in the running session's `events.jsonl` (kind `archive`).

Text, events, the roster and voice metadata (who talked when, no audio) are archived by default; the "Archive" switch
turns all of it off.

## Settings and switches

| | |
|---|---|
| page: Archive | text + events + roster + voice metadata. Default ON. `POST /control?do=forensic&archive=0\|1` |
| page: One-party consent | `POST /control?do=forensic&consent=1` (records the time) / `consent=0` (also turns voice audio off) |
| page: Voice audio | `POST /control?do=forensic&voice=0\|1`; `voice=1` is refused without consent (`consent=1&voice=1` in one request works) |
| `PELOAD_FORENSIC=0` | disables recording entirely (the viewer still shows old sessions) |
| `PELOAD_FORENSIC_DIR=<dir>` | archive somewhere else (default `$HOME/Library/Application Support/ReclaimerMac/forensics`) |
| `PELOAD_FORENSIC_PAGE=<file>` | serve the viewer from disk; with `PELOAD_STATS_PAGE` set (play.sh does in a checkout) `forensic_page.html` next to it is served, so page edits show on a refresh |

`/control` needs POST with an `X-Reclaimer-Control` header like every stats page control; `PELOAD_STATS_READONLY=1`
turns all controls off. Settings are in `forensics/settings.json`:
`{"archive":true,"voice_audio":true,"one_party_consent":true,"consent_confirmed_unix":<first start>,"consent_confirmed":"<iso time>"}` (the defaults).

Cost: with the archive off, each hook (voice, chat, event) does one flag check (`g_fx_on`) and the snapshot thread one
more; the writer thread is not even started. With it on, a hook copies its record into an in-memory queue under one
short lock (no file I/O, no allocation in the steady state); the `shim-forensic` thread writes the queue out once a
second (sooner past 4 MB). Up to 64 MB can wait in the queue; past that new records are dropped and counted (the page
shows the count). Files are flushed every second, so a crash loses at most the last second; `session.json`'s
`ended_unix` stays null when the process dies (the viewer then uses the last record's time).

## Sessions

`forensics/<YYYYmmdd-HHMMSS>_<label>/`, local time, label = the map (the stats page's map name, e.g. `riverworld`), else
the server from a `reclaimer://join?server=<host:port>` launch (play.sh `RECLAIMER_JOIN`), else `lobby`. A new session
starts when, stable for 2 s (map loads flicker):

- the network session's **host peer id** changes (another server, or a new session on the same one); map changes
  within one server stay in its session -- every roster line carries the map (2026-10-18: keying on the map as well
  split the main menu into a session per background scene: guardian, 005_intro, ...);

the current one also closes when the client has been out of any session for 5 s. Records that arrive with no session
open (rare: a notice in the main menu) open a `<time>_menu` session. A server joined from the Server Browser has no
address in its session (the client logs none the loader taps); its host peer id identifies it across sessions.

## Files

All integers little-endian. Peer ids are the 8-byte Halo network-session peer ids the client also uses as voice ids
(reclaimer-voice-mute.md section 4), written as 16 hex digits.

### voice.bin

Header, 64 bytes:

| off | type | |
|---|---|---|
| 0 | char[8] | `"RCVOICE\0"` |
| 8 | u32 | version = 1 |
| 12 | u32 | header size = 64 |
| 16 | u64 | created, unix ns |
| 24 | u64 | this client's peer id when the file was made (0 = not known yet) |
| 32 | u32 | record header size = 40 |
| 36 | u32 | flags: bit 0 voice audio was on at creation |
| 40 | 24 bytes | 0 |

Then one record per packet, append-only: a 40-byte header followed by `stored` bytes.

| off | type | |
|---|---|---|
| 0 | u16 | sync `0xA55A` |
| 2 | u8 | channel = payload[0]: 1 global, 2 team, 3 proximity, 0 control (below); 0xFF unreadable |
| 3 | u8 | flags: 1 audio stored, 2 control packet, 4 truncated (stored < length; never in practice), 8 shorter than the 9-byte header (raw bytes stored with audio on), 16 from this client's own peer id (echo), 32 payload unreadable |
| 4 | u16 | length of the Opus data (payload length - 9) |
| 6 | u16 | stored bytes that follow (= the length with audio on; 0 without; control packets: their bytes) |
| 8 | u64 | arrival time, unix ns (CLOCK_REALTIME, taken in the hook) |
| 16 | u64 | sender peer id (voice_recv's rdx) |
| 24 | u64 | sequence = payload[1..9] as u64 LE |
| 32 | u8, u8 | the Opus TOC byte and the byte after it (packet duration even without audio) |
| 34 | 6 bytes | 0 |

A reader that meets a bad sync (a torn write) scans forward to the next record with a valid sync, a sane length and a
time within a day before / 40 days after the header's creation time (the viewer counts them as `bad_records`); an
incomplete last record is simply not read yet.

Payload facts, CONFIRMED in voice_recv (0.9.18): audio packets are accepted with a total length of 11..650 bytes
(exe+0x684138: 2..641 bytes of Opus) and channel 1..3 (exe+0x684153); the sender's own echo is dropped
(sender == voice state +0x278 -> +0x9c0); the per-sender stream drops a packet whose sequence is <= the last accepted
one, compared as an unsigned u64 (exe+0xb2ca4d, `cmp rdx,[rcx+8]; ja`), so the sequence is a little-endian, strictly
increasing counter per sender. Whether it counts packets (+1 each) is GUESSED: the viewer only reports `seq_gaps`
(missing packets) for an utterance whose smallest step is 1. Channel-0 packets (exe+0x684248): only accepted from the
peer id at voice state +0x1e8 (GUESSED: the server / voice host), exactly 10 bytes: [1..9] their own sequence, [9] a
code 0..4 or 0x80..0x8f that sets two fields of the voice state (+0xafa bit, +0xafc mask: codes 0..4 -> masks
0,7,7,4,4; GUESSED: which channels the server allows). They are archived with flag 2 and their code byte, whether or
not voice audio is on (they are not audio).

Python reader:
```python
import struct
d = open('voice.bin', 'rb').read(); assert d[:8] == b'RCVOICE\0'
hs, rs = struct.unpack_from('<I', d, 12)[0], struct.unpack_from('<I', d, 32)[0]; o = hs
while o + rs <= len(d):
    sync, ch, fl, olen, slen, t, sender, seq, toc = struct.unpack_from('<HBBHHQQQB', d, o)
    if sync != 0xA55A: break                      # (resync as described above)
    opus = d[o + rs:o + rs + slen]; o += rs + slen
    print(t / 1e9, '%016x' % sender, ch, seq, olen, 'audio' if fl & 1 else '-')
```

### roster.jsonl

One line whenever the session's member list (id, name, team, slot, host / self, muted) or the map changes, and the
first one of every session:
```json
{"t":1760760000.123,"map":"riverworld","host_peer":0,"host_id":"9e3...","self_peer":3,"self_id":"41c...",
 "members":[{"id":"1111111111111111","name":"Alice","peer":1,"index":0,"slot":0,"team":1,"team_name":"blue",
             "host":false,"self":false,"muted":false}]}
```
`members` = the halo3 network session (stats.c `st_session`: player records with a peer index, peer id, UTF-16 name),
paired with the engine's players table the way the stats page pairs them (session index == player slot and the same
name, else the name). `team` is the last team seen on the player's biped (-1 before it spawned). `muted` is this
client's own mute list. `host_id` / `self_id` are the host's and this client's peer ids (a dedicated server is a host
peer without a player).

### events.jsonl

One line per event, in arrival order: `t` (unix seconds), `seq` (the stats feed's sequence), `kind` (chat, join, leave,
kill, death, vote, server, game, log, archive), `text`, and when present `a` / `b` (killer, victim; chat: sender name),
`how` (weapon), `verb`, `ta` / `tb` (teams), `estimated`, `mine`, `betrayal`. Chat lines also carry `channel` (all,
team, nearby, server, notice) and, for players' messages, `sender_id` (Line +0x30, the sender's peer id).

### session.json

Name, label, map, server, host / self peer ids, start (unix + local ISO time), end (null while open or after a crash),
pid, and the voice-audio / consent state when the session started.

## Viewer and endpoints (GET)

| path | |
|---|---|
| `/forensics` | the page (shim/src/forensic_page.html, embedded) |
| `/forensics/sessions.json` | settings, recording state, counters, the archive directory, every session (newest first) with sizes and its session.json |
| `/forensics/<s>/index.json` | the session: `utterances`, `speakers`, `people` (everyone the roster listed), `control` packets, `events`, stats |
| `/forensics/<s>/utt/<n>.ogg` | utterance n as Ogg Opus (404 when it has no audio) |
| `/forensics/<s>/utt/<n>.json` | its packets: time, sequence, length, stored, TOC decoded (mode, bandwidth, frame ms, stereo, code, duration) |
| `/forensics/<s>/speaker/<peer id>.ogg` | all of one sender's audio in the session (`.json`: their packets) |
| `/forensics/<s>/events.jsonl`, `roster.jsonl`, `voice.bin`, `session.json` | the raw files |

`?dl=1` adds `Content-Disposition: attachment`. Ogg replies honour `Range: bytes=` (206 / 416) and are byte-identical
on every request (Safari's player asks for ranges). A session being written is flushed before it is read, and its
voice.bin is read incrementally (only the new tail) on each refresh.

**Utterance** = a run of packets from one sender on one channel whose arrival times are less than 0.6 s apart. `start`
= the first packet's arrival, `end` = the last packet's arrival + its duration, `audio_s` = the summed packet durations.
Each utterance is attributed to the roster entry in force at its start (name, team, slot), so a later rename or team
change does not rewrite history; `speakers[].names` lists every name an id had.

**Ogg files.** OpusHead version 1, channel count from the first packet's TOC stereo bit, pre-skip 0 (nothing the
client received is dropped), input rate 48000, gain 0, mapping family 0; OpusTags with the speaker, peer id, session,
channel and start time; audio pages of whole packets, about one second each, granule = samples through the page's last
packet (from each packet's TOC: config -> frame duration, code 0/1/2/3 -> frame count), CRC-32 polynomial 0x04C11DB7,
EOS on the last page. Packets are never re-encoded. A **speaker file** keeps each utterance's packets back to back and
puts the gap between two utterances in as silence, shortened to at most 1 s (`F8 FF FE`, a 20 ms CELT silence frame),
so it plays as one recording of everything that person said; the index has the real times.

The page: the session list (LIVE = being written), a summary with downloads, speakers with team colours and their
`.ogg`, a timeline with one lane per speaker (blue global, green team, orange proximity, striped = no audio kept) plus
chat / kill / event lanes, and everything in one list in time order with filters (player by peer id, channel, voice /
chat / kills / other events). Click a voice block or "play" to listen; "packets" shows the decoded packets. The live
session refreshes every 5 s.

## Testing

`bash shim/tools/forensic-oggtest.sh` (offline, no game; needs ffmpeg with libopus + python3) builds
`shim/tools/forensic-oggtest.c`, which compiles forensic.c with `FX_TEST=1` (everything except the engine glue):

1. ffmpeg encodes tones to Ogg Opus in 8 shapes (CELT 2.5/10/20 ms, stereo 40 and 60 ms = code 1/2/3 packets, 120 ms,
   SILK voip at 10-12 kbit/s); the harness re-wraps the raw packets with the writer, reads them back (our CRC agrees
   with ffmpeg's pages, packets byte-identical, granules), and ffmpeg decodes ours without a single message to the
   exact expected sample count, bit-identical to its decode of its own file (up to that file's end-trimmed last frame).
2. 50 silence frames decode to 1 s of zeros.
3. The archive end to end: consent refused / accepted / persisted, roster lines, 4 speakers' packets with a pause, a
   missing sequence number, a control packet, a malformed packet, our own echo, voice audio switched off mid-session,
   chat with sender id, a torn record followed by more packets, a map change (debounce, a second session; the test predates the host-only key) -- then every
   endpoint over a socketpair: names / teams at the time, Range requests, 404s, and every .ogg served decodes cleanly to
   the expected length; every JSON reply parses.

`forensic-oggtest serve <in.ogg> <dir> <port>` serves that test archive on 127.0.0.1:<port> to look at the page.

## Not verified yet (needs a live game)

- The hook's 3rd / 4th arguments (payload, length) in a running client: the entry hook passes rcx/rdx/r8/r9 unchanged
  and voice_recv's prologue moves r8 / r9 to rdi / r14 (the signature), but no live packet has been archived yet.
- Real voice packets: frame size, whether the sequence steps by 1, how often channel-0 packets come and from whom.
- Session boundaries on a public server across map changes, and the host peer id staying the same within one server.
- The chat sender id (Line +0x30) on received lines.
- Playback of the page's Ogg files in Safari (the test browser played them; Safari's Ogg Opus support depends on its
  version -- the page says so and offers downloads when `canPlayType` says no).
