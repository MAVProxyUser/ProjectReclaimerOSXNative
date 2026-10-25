# The Reclaimer client's game datagrams (0.9.22 vs 0.9.23)

What the client puts in each QUIC datagram it sends to the server, and what 0.9.23's release note means by "Each game packet
sent over the internet now carries a 2-byte header instead of 43 bytes of addressing the connection already knows". Static RE
of both builds (2026-10-24), confirmed with real frames (`PELOAD_FRAMECAP`, below) and by measuring datagram sizes with the
packet log (shim/src/pktlog.c). Nothing here is needed to run the game; it explains the traffic the Packets window shows.

## The internal frame (both builds)

The client wraps everything it exchanges with the server -- the engine's game packets, voice, its own control messages -- in a
frame with a 36-byte header, built by the code that produces each kind (e.g. 0.9.23 exe+0x7480d0 for game packets, voice_recv's
exe+0x75a950 for voice) and handed to one sender (0.9.23 exe+0x8bb5a0, 0.9.22 exe+0x8b0400; arguments: the connection, a
delivery class, the frame, its length):

| offset | bytes | content |
|---|---|---|
| 0x00 | 4 | kind tag, ASCII: `MCL2` (the engine's game packets), `MCV1` (voice), `MCH1`, `MCP1`, `MCA1`, `MCT1`, `MCM1`, `MCF1`, `MCJ1`, `MCR1`, `MCE1`, `MCG1`, `MCS1`, `MCU1`, `MCI1` |
| 0x04 | 16 | a session value, the same on every frame of a connection (the receiver checks it: "Native channel frame/session mismatch") |
| 0x14 | 8 | the sender's endpoint id |
| 0x1C | 8 | the recipient's endpoint id |
| 0x24 | .. | the payload (for `MCL2`: one engine packet) |

Seen live (`PELOAD_FRAMECAP=4`, a 0.9.23 client joined to our local server): `MCA1`, `MCL2` and `MCF1` frames, every one with
the same session value and the same two ids (client -> server); payloads of 18 to 277 bytes; class 1, and class 0 on some
`MCL2` frames.

## 0.9.22: 7 + 36 = 43 bytes on every datagram

The sender (exe+0x8b0400) cut the frame into at most 16 pieces and sent each as `[class 1][message id 4][fragment index 1]
[fragment count 1]` + the piece -- the WHOLE frame, its 36-byte header included (exe+0x8b081b: an allocation of `piece + 7`;
+0x8b0840 class, +0x8b0848 the message id, big-endian, from a global counter, +0x8b0852 index, +0x8b085c count). An
unfragmented game packet therefore travelled with 1 + 4 + 1 + 1 + 4 + 16 + 8 + 8 = 43 bytes in front of it.

## 0.9.23: 2 bytes (more only when needed)

The sender (exe+0x8bb5a0) checks the frame's two ids against the two its connection already holds (exe+0x8bc27a):

- both the connection's own, in order: nothing is sent for them;
- both, swapped: a flag says so;
- anything else: the 16 bytes are sent, with a flag.

and builds each datagram (exe+0x8b810) as:

| bytes | when | content |
|---|---|---|
| 1 | always | the kind as a code -- `0x80 + 8 x k` (`MCL2` 0x80, `MCH1` 0x88, `MCV1` 0x90, `MCP1` 0x98, `MCA1` 0xA0, `MCT1` 0xA8, `MCM1` 0xB0, `MCF1` 0xB8, `MCJ1` 0xC0, `MCR1` 0xC8, `MCE1` 0xD0, `MCG1` 0xD8, `MCS1` 0xE0, `MCU1` 0xE8, `MCI1` 0xF0) -- OR the flags: 1 = fragmented, 2 = the ids are swapped, 4 = the ids follow |
| 1 | always | the delivery class (the sender's argument; 0.9.22's first byte) |
| 16 | flag 4 | the sender's and recipient's endpoint ids |
| 4 + 2 | flag 1 | the message id (big-endian) and `count << 8 | index` |
| .. | always | the payload (the frame from +0x24) |

The 4-byte tag became 5 bits, the session value is gone (one QUIC connection carries one session), the ids are gone whenever
they are the connection's own, and the fragment fields appear only on fragmented frames. (The receiving side must put the
header back together from its connection's values; that code was not traced.)

Measured (the packet log, the same local Valhalla match on each version, ~50 s in the match):

| | 0.9.22 | 0.9.23 |
|---|---|---|
| client -> server: datagrams / s, mean size | ~81, ~96 B | ~78, ~65 B |
| server -> client: datagrams / s, mean size | ~87, ~162 B | ~84, ~126 B |

~31 / ~36 bytes less per datagram on average: the 41 bytes on every datagram that carries a frame (some are QUIC-only, e.g.
acknowledgements).

## PELOAD_FRAMECAP=<n> (debug)

`shim/src/pktlog.c pk_framecap_install`: an entry hook on the client's sender (found by signature, 0.9.23's shape) logs the
first n frames of each kind it is given, before it compacts and encrypts them: `[framecap] MCL2 class 1 len 106 (payload 70)
session <16 bytes hex> ids <sender> -> <recipient>`. Nothing runs without the variable. The packet log never reads payloads;
this switch only reads the 36-byte header.
