# The server list and the status query

What the Project Reclaimer client sends to every game server it lists, what it gets back, and where the list comes from.
Decrypted and labelled field by field (2026-10-25, client 0.9.23) from a status query to our local dedicated server, with
the client's own TLS secrets (`PELOAD_KEYLOG`) and `re-tools/quicparse.py`. Public servers run the same server code. Why it
matters: README "Who sees your IP address".

## Where the list lives

- **Discovery masters**: small HTTP services, normally on port 49175. Two are built into the client
  (`http://148.251.153.44:49175` and `http://23.159.176.140:49175`, plus `http://127.0.0.1:49175` for a master on the same
  PC); dedicated servers announce themselves to the built-in ones. The client's own help: `master-server` "Run the
  authenticated master server with expiring listings", `discovery-master` "Run a discovery-only bootstrap master (no
  registration secret)" -- anyone can run the second kind, and many server operators do, beside their game servers.
- **The client's cache of masters**: `C:\ReclaimerCache\discovery\peers.json` (in the prefix: `ReclaimerCache/discovery/`),
  `[{origin, seen, ip, failures}]` -- 64 masters here, 57 of them on port 49175 (`discovery-bootstrap` replaces the list).
- **Settings → Master servers**: more masters, one HTTP(S) address per line; "The built-in masters are always asked first".
- What a master returns, in the client's words: "listings are signed addresses, and servers report their own status" --
  addresses only; the name, map and player count come from each server itself (below). The client says "Limited
  discovery" when fewer than two sources answer. The masters are plain `http://` (unencrypted).

Seen on a public join (README): the client fetched the lists over TCP from the built-in master and two masters from its
cache (~105 KB each), then queried every listed server within ~3 seconds -- on every start, also when started straight
into a join link. (Its other plain-HTTP service, `http://148.251.153.44:8790`, is the community stats service: ranks.)

## The status query: one short QUIC connection per server

UDP, QUIC v1 (quinn 0.11 + rustls + ring), a new connection per server, all from one local UDP port. 16 datagrams with our
local server (in order; TX = client -> server):

| # | dir | UDP | QUIC packets | what is in them |
|---|---|---|---|---|
| 1 | TX | 1200 | Initial | TLS **ClientHello** + padding to 1200 (anti-amplification) |
| 2 | RX | 1200 | Initial + Handshake | ACK, TLS **ServerHello**; EncryptedExtensions, **Certificate**, CertificateVerify, Finished |
| 3 | RX | 138 | 1-RTT | 4 x NEW_CONNECTION_ID |
| 4 | TX | 1200 | Initial + Handshake | ACKs; the client's TLS **Finished** (padded) |
| 5 | RX | 209 | 1-RTT | HANDSHAKE_DONE; 2 x TLS NewSessionTicket |
| 6 | TX | 144 | 1-RTT | ACK, 4 x NEW_CONNECTION_ID |
| 7 | RX | 33 | 1-RTT | RETIRE_CONNECTION_ID 0, ACK |
| 8 | TX | 59 | 1-RTT | NEW_CONNECTION_ID 5, ACK |
| 9 | TX | 45 | 1-RTT | **STREAM 0: the request** (16 bytes, FIN) |
| 10 | RX | 31 | 1-RTT | ACK |
| 11 | RX | 30 | 1-RTT | STOP_SENDING stream 0 |
| 12 | RX | 250 | 1-RTT | **STREAM 0: the reply** (220 bytes, FIN) |
| 13 | TX | 32 | 1-RTT | ACK |
| 14 | RX | 30 | 1-RTT | MAX_STREAMS 2 |
| 15 | RX | 34 | 1-RTT | ACK, CONNECTION_CLOSE (application, code 0) |
| 16 | TX | 35 | 1-RTT | ACK, CONNECTION_CLOSE (NO_ERROR) |

About 2.7 KB sent and 2 KB received per server; a server that doesn't answer gets the Initial again and again (~7 KB).
Both sides grease the QUIC fixed bit (RFC 9287): some packets have it 0, which is why a bit-level classifier can't rely on
it. Initial packets are readable by anyone on the path (their keys come from the client's destination connection id and
QUIC v1's public salt); everything after needs the TLS secrets.

### The TLS handshake (what anyone on the path can read from packet 1)

ClientHello: TLS 1.3 only; cipher suites AES-256-GCM, AES-128-GCM, ChaCha20-Poly1305; groups x25519, secp256r1, secp384r1
(key share: x25519); signature algorithms ECDSA P-384 / P-256, Ed25519, RSA-PSS and PKCS#1 (SHA-256/384/512); OCSP
status_request; extended_master_secret; psk_dhe_ke; **server_name "reclaimer"** -- the same for every server, not its
address; no ALPN. QUIC transport parameters: max_udp_payload_size 1472, initial_max_data 2^62-1, stream data 1,250,000,
active_connection_id_limit 5, max_idle_timeout 2500 ms, max_datagram_frame_size 65535, min_ack_delay 1000 us, grease_quic_bit,
one GREASE parameter, initial_source_connection_id.

Server (encrypted from the Handshake packet on): TLS_AES_256_GCM_SHA384 with x25519; **a self-signed certificate** "CN=rcgen
self signed cert", SAN `reclaimer`, ECDSA P-256, valid 1975-4096 (made by the server software, rcgen); CertificateVerify
ecdsa_secp256r1_sha256; transport parameters (idle timeout 30 s, 1 bidirectional stream, 16 unidirectional, 1 MB of data,
datagrams 65535, a stateless reset token). **No CertificateRequest: the client presents no certificate**, and no player
identity is part of the handshake. The server hands out two session tickets.

### The request and the reply (stream 0, inside TLS)

Each message is a 4-byte big-endian length and then JSON:

```
client -> server   00 00 00 0c  {"status":2}
server -> client   00 00 00 d8  {"name":"Valhalla Test","host_name":"Mac test server","map":"Valhalla","mode":"Slayer",
                                 "players":0,"capacity":16,"ready":true,"password_required":false,"version":"0.9.23",
                                 "variant":"Custom Slayer","blocks_vpn":false}
```

| field | meaning |
|---|---|
| `status` | the request: `2` (what the number selects was not traced; it is the only field) |
| `name` | the server's name in the browser |
| `host_name` | the host's name |
| `map`, `mode`, `variant` | the map, the game type and the variant now in play |
| `players`, `capacity` | players in it and its maximum |
| `ready` | whether it takes joins now |
| `password_required` | a password is needed to join |
| `version` | the server's client version |
| `blocks_vpn` | it refuses VPN / datacenter addresses (the client shows this before you join) |

The client adds `address` and `ping_ms` (the round trip it measured) to what `server-status` prints.

### What a server learns from the query

Your public IP address and UDP port, the time, and a TLS / QUIC fingerprint every Reclaimer client shares (the
parameters above). Nothing names you: no player ID, nickname or profile is sent, and no client certificate.

## Reproducing it

```bash
bash shim/dedicated.sh                                  # our local server (127.0.0.1:49190)
PELOAD_KEYLOG=/tmp/keys.log PELOAD_TRACE_UDP=1 PELOAD_TRACE_UDP_HEX=1500 PELOAD_STATS_HTTP=0 \
  arch -x86_64 shim/build/peload client/project-reclaimer-client-v0.9.23.exe server-status 127.0.0.1:49190 2> /tmp/ss.err
python3 re-tools/quicparse.py /tmp/ss.err /tmp/keys.log
```

- `PELOAD_KEYLOG=<file>` (debug, pktlog.c `pk_keylog_install`): the client's TLS secrets in the NSS key log format. rustls asks
  its config's KeyLog; the client's is rustls' default NoKeyLog. Its vtable (found by its Debug function, which writes
  "NoKeyLog"; 0.9.23 exe+0x25ee408) gets two pointers of ours: `will_log` -> true, `log` -> a line in the file. Data only,
  no code. Every TLS connection of that process is affected (the game's own QUIC too): never on a normal run.
- `PELOAD_TRACE_UDP_HEX=<n>`: the loader's UDP trace also prints each datagram's first n bytes in hex.
- `re-tools/quicparse.py`: splits coalesced packets, removes header protection, decodes packet numbers, decrypts (Initial
  keys from the public salt, Handshake / 1-RTT keys from the key log; AES-GCM or ChaCha20), labels every frame, parses the
  TLS messages in CRYPTO frames (each extension, the transport parameters, the certificate) and reassembles the streams.
  Needs python3's `cryptography` package.
- `PELOAD_FRAMECAP=<n>` / `PELOAD_FRAMECAP_HEX=<n>` (client-transport.md) catch nothing here: the status query does not use
  the game connection's frame sender.

Not checked: whether a client that queries many servers in one run offers a session ticket from one server to the next
(every server has the server name "reclaimer"); a capture of a full browser refresh with the key log would show it.
