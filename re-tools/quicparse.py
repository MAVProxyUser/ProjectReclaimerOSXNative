#!/usr/bin/env python3
"""Decrypt and label a captured QUIC exchange, packet by packet (RFC 9000 / 9001 / 8446).

    python3 re-tools/quicparse.py <loader stderr with PELOAD_TRACE_UDP=1 PELOAD_TRACE_UDP_HEX=1500> [keylog] [--peer host:port]

The trace's "[udp] TX/RX ... len=N" lines and their hex lines give the datagrams; the key log (PELOAD_KEYLOG, NSS format:
"LABEL <client random> <secret>") gives the Handshake and 1-RTT secrets. Initial packets need no key log: their keys come
from the client's first destination connection id and QUIC v1's public salt -- anyone on the path can read them.
Prints every datagram, every coalesced packet's header fields, every frame, the TLS handshake messages inside the CRYPTO
frames (ClientHello / ServerHello / EncryptedExtensions / Certificate / CertificateVerify / Finished, with each extension
and the QUIC transport parameters), and each stream's reassembled bytes. --peer keeps only datagrams to / from that peer
(default: the first one the client sent to). Needs the `cryptography` package (system python3 has it here).
Shown in shim/docs/server-status.md (the client's server status query, labelled field by field)."""
import re, sys, struct, hashlib, datetime
from cryptography.hazmat.primitives import hashes, hmac
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
from cryptography.hazmat.primitives.ciphers.aead import AESGCM, ChaCha20Poly1305
from cryptography import x509

V1_SALT = bytes.fromhex('38762cf7f55934b34d179ae6a4c80cadccbb7f0a')
SUITES = {0x1301: ('TLS_AES_128_GCM_SHA256', 16, hashes.SHA256), 0x1302: ('TLS_AES_256_GCM_SHA384', 32, hashes.SHA384),
          0x1303: ('TLS_CHACHA20_POLY1305_SHA256', 32, hashes.SHA256), 0x00ff: ('TLS_EMPTY_RENEGOTIATION_INFO_SCSV', 0, None)}
GROUPS = {0x001d: 'x25519', 0x0017: 'secp256r1', 0x0018: 'secp384r1', 0x0019: 'secp521r1', 0x001e: 'x448', 0x0100: 'ffdhe2048',
          0x11ec: 'X25519MLKEM768', 0x11eb: 'SecP256r1MLKEM768', 0x6399: 'X25519Kyber768Draft00', 0x0201: 'MLKEM768', 0x0202: 'MLKEM1024'}
SIGALGS = {0x0403: 'ecdsa_secp256r1_sha256', 0x0503: 'ecdsa_secp384r1_sha384', 0x0603: 'ecdsa_secp521r1_sha512', 0x0807: 'ed25519',
           0x0808: 'ed448', 0x0804: 'rsa_pss_rsae_sha256', 0x0805: 'rsa_pss_rsae_sha384', 0x0806: 'rsa_pss_rsae_sha512',
           0x0809: 'rsa_pss_pss_sha256', 0x080a: 'rsa_pss_pss_sha384', 0x080b: 'rsa_pss_pss_sha512', 0x0401: 'rsa_pkcs1_sha256',
           0x0501: 'rsa_pkcs1_sha384', 0x0601: 'rsa_pkcs1_sha512', 0x0201: 'rsa_pkcs1_sha1', 0x0203: 'ecdsa_sha1'}
EXTS = {0: 'server_name', 1: 'max_fragment_length', 5: 'status_request', 10: 'supported_groups', 11: 'ec_point_formats',
        13: 'signature_algorithms', 16: 'application_layer_protocol_negotiation', 18: 'signed_certificate_timestamp', 21: 'padding',
        23: 'extended_master_secret', 27: 'compress_certificate', 35: 'session_ticket', 41: 'pre_shared_key', 42: 'early_data',
        43: 'supported_versions', 44: 'cookie', 45: 'psk_key_exchange_modes', 47: 'certificate_authorities', 49: 'post_handshake_auth',
        50: 'signature_algorithms_cert', 51: 'key_share', 57: 'quic_transport_parameters', 0xff01: 'renegotiation_info'}
TPS = {0x00: 'original_destination_connection_id', 0x01: 'max_idle_timeout (ms)', 0x02: 'stateless_reset_token',
       0x03: 'max_udp_payload_size', 0x04: 'initial_max_data', 0x05: 'initial_max_stream_data_bidi_local',
       0x06: 'initial_max_stream_data_bidi_remote', 0x07: 'initial_max_stream_data_uni', 0x08: 'initial_max_streams_bidi',
       0x09: 'initial_max_streams_uni', 0x0a: 'ack_delay_exponent', 0x0b: 'max_ack_delay (ms)', 0x0c: 'disable_active_migration',
       0x0d: 'preferred_address', 0x0e: 'active_connection_id_limit', 0x0f: 'initial_source_connection_id',
       0x10: 'retry_source_connection_id', 0x11: 'version_information', 0x20: 'max_datagram_frame_size (RFC 9221)',
       0x2ab2: 'grease_quic_bit (RFC 9287)', 0xff04de1b: 'min_ack_delay (us, ack frequency draft)'}
TP_BYTES = {0x00, 0x02, 0x0d, 0x0f, 0x10, 0x11}
TP_FLAG = {0x0c, 0x2ab2}
HS = {1: 'ClientHello', 2: 'ServerHello', 4: 'NewSessionTicket', 8: 'EncryptedExtensions', 11: 'Certificate', 13: 'CertificateRequest',
      15: 'CertificateVerify', 20: 'Finished', 24: 'KeyUpdate'}
FRAMES = {0x00: 'PADDING', 0x01: 'PING', 0x02: 'ACK', 0x03: 'ACK+ECN', 0x04: 'RESET_STREAM', 0x05: 'STOP_SENDING', 0x06: 'CRYPTO',
          0x07: 'NEW_TOKEN', 0x10: 'MAX_DATA', 0x11: 'MAX_STREAM_DATA', 0x12: 'MAX_STREAMS (bidi)', 0x13: 'MAX_STREAMS (uni)',
          0x14: 'DATA_BLOCKED', 0x15: 'STREAM_DATA_BLOCKED', 0x16: 'STREAMS_BLOCKED (bidi)', 0x17: 'STREAMS_BLOCKED (uni)',
          0x18: 'NEW_CONNECTION_ID', 0x19: 'RETIRE_CONNECTION_ID', 0x1a: 'PATH_CHALLENGE', 0x1b: 'PATH_RESPONSE',
          0x1c: 'CONNECTION_CLOSE (transport)', 0x1d: 'CONNECTION_CLOSE (application)', 0x1e: 'HANDSHAKE_DONE',
          0x30: 'DATAGRAM', 0x31: 'DATAGRAM (with length)', 0xaf: 'ACK_FREQUENCY', 0x1f: 'IMMEDIATE_ACK'}
TERR = {0: 'NO_ERROR', 1: 'INTERNAL_ERROR', 2: 'CONNECTION_REFUSED', 3: 'FLOW_CONTROL_ERROR', 4: 'STREAM_LIMIT_ERROR',
        5: 'STREAM_STATE_ERROR', 6: 'FINAL_SIZE_ERROR', 7: 'FRAME_ENCODING_ERROR', 8: 'TRANSPORT_PARAMETER_ERROR',
        9: 'CONNECTION_ID_LIMIT_ERROR', 10: 'PROTOCOL_VIOLATION', 11: 'INVALID_TOKEN', 12: 'APPLICATION_ERROR',
        13: 'CRYPTO_BUFFER_EXCEEDED', 14: 'KEY_UPDATE_ERROR', 15: 'AEAD_LIMIT_REACHED', 16: 'NO_VIABLE_PATH'}


def hx(b, n=None):
    b = bytes(b)
    return b.hex() if n is None or len(b) <= n else b[:n].hex() + f'... ({len(b)} bytes)'


def varint(b, o):
    l = 1 << (b[o] >> 6); v = b[o] & 0x3f
    for i in range(1, l): v = (v << 8) | b[o + i]
    return v, o + l


def hkdf_extract(salt, ikm, h=hashes.SHA256):
    m = hmac.HMAC(salt, h()); m.update(ikm); return m.finalize()


def hkdf_expand_label(secret, label, length, h=hashes.SHA256, ctx=b''):
    full = b'tls13 ' + label.encode()
    info = struct.pack('>H', length) + bytes([len(full)]) + full + bytes([len(ctx)]) + ctx
    out, t, i = b'', b'', 1
    while len(out) < length:
        m = hmac.HMAC(secret, h()); m.update(t + info + bytes([i])); t = m.finalize(); out += t; i += 1
    return out[:length]


class Keys:
    def __init__(self, secret, suite):
        self.name, kl, h = SUITES[suite]; self.suite = suite
        self.key = hkdf_expand_label(secret, 'quic key', kl, h); self.iv = hkdf_expand_label(secret, 'quic iv', 12, h)
        self.hp = hkdf_expand_label(secret, 'quic hp', kl, h)

    def mask(self, sample):
        if self.suite == 0x1303:
            c = Cipher(algorithms.ChaCha20(self.hp, sample[:16]), mode=None).encryptor(); return c.update(b'\0' * 5)
        e = Cipher(algorithms.AES(self.hp), modes.ECB()).encryptor(); return e.update(sample)[:5]

    def open(self, pn, aad, ct):
        nonce = bytes(a ^ b for a, b in zip(self.iv, pn.to_bytes(12, 'big')))
        a = ChaCha20Poly1305(self.key) if self.suite == 0x1303 else AESGCM(self.key)
        return a.decrypt(nonce, ct, aad)


def read_trace(path):
    dgs, cur = [], None
    for line in open(path, errors='replace'):
        m = re.search(r'\[udp\] (TX|RX) fd=\d+ local:(\d+) (->|<-) ([0-9a-fA-F.:\[\]]+):(\d+) len=(\d+)', line)
        if m:
            cur = {'dir': m.group(1), 'lport': int(m.group(2)), 'peer': f'{m.group(4)}:{m.group(5)}', 'len': int(m.group(6)), 'hex': ''}
            dgs.append(cur); continue
        m = re.search(r'\[udp\]\s+([0-9a-f]{4}) ([0-9a-f]+)\s*$', line)
        if m and cur is not None: cur['hex'] += m.group(2)
    for d in dgs: d['b'] = bytes.fromhex(d['hex'])
    return dgs


def read_keylog(path):
    k = {}
    if path:
        for line in open(path):
            p = line.split()
            if len(p) == 3: k[(p[0], p[1])] = bytes.fromhex(p[2])
    return k


class Conn:
    def __init__(self, keylog):
        self.keylog = keylog; self.keys = {}; self.suite = None; self.client_random = None
        self.largest = {}; self.crypto = {}; self.streams = {}; self.cid_len = {}; self.tls_seen = set()
        self.notes = []

    def initial(self, dcid):
        s = hkdf_extract(V1_SALT, dcid)
        self.keys[('TX', 'Initial')] = Keys(hkdf_expand_label(s, 'client in', 32), 0x1301)
        self.keys[('RX', 'Initial')] = Keys(hkdf_expand_label(s, 'server in', 32), 0x1301)

    def derive(self):
        if self.suite is None or self.client_random is None: return
        cr = self.client_random.hex()
        for space, cl, sl in (('Handshake', 'CLIENT_HANDSHAKE_TRAFFIC_SECRET', 'SERVER_HANDSHAKE_TRAFFIC_SECRET'),
                              ('1-RTT', 'CLIENT_TRAFFIC_SECRET_0', 'SERVER_TRAFFIC_SECRET_0')):
            for d, lab in (('TX', cl), ('RX', sl)):
                sec = self.keylog.get((lab, cr))
                if sec is not None and (d, space) not in self.keys: self.keys[(d, space)] = Keys(sec, self.suite)


def decode_pn(trunc, nbits, largest):
    expected = largest + 1; win = 1 << nbits; hwin = win // 2; mask = win - 1
    cand = (expected & ~mask) | trunc
    if cand <= expected - hwin and cand < (1 << 62) - win: return cand + win
    if cand > expected + hwin and cand >= win: return cand - win
    return cand


def parse_tps(b, ind):
    o = 0; out = []
    while o < len(b):
        i, o = varint(b, o); l, o = varint(b, o); v = b[o:o + l]; o += l
        name = TPS.get(i, f'reserved (GREASE)' if i % 31 == 27 else f'unknown 0x{i:x}')
        if i in TP_BYTES: val = hx(v)
        elif i in TP_FLAG or not v: val = '(present)'
        else:
            try: val, _ = varint(v, 0)
            except Exception: val = hx(v)
        out.append(f'{ind}  0x{i:x} {name} = {val}')
    return out


def parse_exts(b, ind, hs):
    o = 0; out = []
    while o + 4 <= len(b):
        t, l = struct.unpack_from('>HH', b, o); v = b[o + 4:o + 4 + l]; o += 4 + l
        name = EXTS.get(t, 'GREASE' if (t & 0x0f0f) == 0x0a0a else f'unknown {t}')
        line = f'{ind}extension {t} {name} ({l} bytes)'
        if t == 0 and l:
            n = struct.unpack_from('>H', v, 3)[0]; line += f': host_name "{v[5:5 + n].decode(errors="replace")}"'
        elif t == 10: line += ': ' + ', '.join(GROUPS.get(x, f'0x{x:04x}') for x in struct.unpack_from(f'>{(l - 2) // 2}H', v, 2))
        elif t in (13, 50): line += ': ' + ', '.join(SIGALGS.get(x, f'0x{x:04x}') for x in struct.unpack_from(f'>{(l - 2) // 2}H', v, 2))
        elif t == 16:
            p, q = [], 2
            while q < len(v): n = v[q]; p.append(v[q + 1:q + 1 + n].decode(errors='replace')); q += 1 + n
            line += ': ' + ', '.join(repr(x) for x in p)
        elif t == 43:
            vs = struct.unpack_from(f'>{(l - 1) // 2}H', v, 1) if hs == 1 else struct.unpack_from('>H', v, 0)
            line += ': ' + ', '.join({0x0304: 'TLS 1.3', 0x0303: 'TLS 1.2'}.get(x, f'0x{x:04x}') for x in vs)
        elif t == 51:
            if hs == 1:
                q, ks = 2, []
                while q < len(v):
                    g, n = struct.unpack_from('>HH', v, q); ks.append(f'{GROUPS.get(g, hex(g))} ({n}-byte public key)'); q += 4 + n
                line += ': ' + ', '.join(ks)
            else:
                g, n = struct.unpack_from('>HH', v, 0); line += f': {GROUPS.get(g, hex(g))} ({n}-byte public key)'
        elif t == 45: line += ': ' + ', '.join({0: 'psk_ke', 1: 'psk_dhe_ke'}.get(x, str(x)) for x in v[1:])
        elif t == 57: out.append(line + ':'); out += parse_tps(v, ind); continue
        elif l: line += ': ' + hx(v, 24)
        out.append(line)
    return out


def parse_tls(msg_t, body, ind, conn, who):
    out = [f'{ind}TLS {HS.get(msg_t, msg_t)} ({len(body)} bytes)']
    i2 = ind + '  '
    if msg_t in (1, 2):
        ver = struct.unpack_from('>H', body, 0)[0]; rnd = body[2:34]; sl = body[34]; o = 35 + sl
        out.append(f'{i2}legacy_version 0x{ver:04x}; random {rnd.hex()}; legacy_session_id {sl} bytes')
        if msg_t == 1:
            conn.client_random = rnd
            n = struct.unpack_from('>H', body, o)[0]; cs = struct.unpack_from(f'>{n // 2}H', body, o + 2); o += 2 + n
            out.append(f'{i2}cipher_suites: ' + ', '.join(SUITES.get(x, (f'0x{x:04x}',))[0] for x in cs))
            n = body[o]; o += 1 + n
        else:
            cs = struct.unpack_from('>H', body, o)[0]; o += 3; conn.suite = cs
            out.append(f'{i2}cipher_suite: {SUITES.get(cs, (hex(cs),))[0]}')
        el = struct.unpack_from('>H', body, o)[0]; out += parse_exts(body[o + 2:o + 2 + el], i2, msg_t)
        conn.derive()
    elif msg_t == 8:
        el = struct.unpack_from('>H', body, 0)[0]; out += parse_exts(body[2:2 + el], i2, msg_t)
    elif msg_t == 13:
        cl = body[0]; o = 1 + cl; el = struct.unpack_from('>H', body, o)[0]
        out.append(f'{i2}the {who} asks for a CERTIFICATE from its peer (client authentication)'); out += parse_exts(body[o + 2:o + 2 + el], i2, msg_t)
    elif msg_t == 11:
        cl = body[0]; o = 1 + cl; total = int.from_bytes(body[o:o + 3], 'big'); o += 3; end = o + total; k = 0
        if total == 0: out.append(f'{i2}EMPTY certificate list (no certificate)')
        while o < end:
            n = int.from_bytes(body[o:o + 3], 'big'); der = body[o + 3:o + 3 + n]; o += 3 + n
            el = struct.unpack_from('>H', body, o)[0]; o += 2 + el
            try:
                c = x509.load_der_x509_certificate(der)
                try: san = c.extensions.get_extension_for_class(x509.SubjectAlternativeName).value; sans = [str(x.value) for x in san]
                except Exception: sans = []
                pk = c.public_key(); pkn = type(pk).__name__.replace('_', '')
                out.append(f'{i2}certificate {k}: {n} bytes DER, subject "{c.subject.rfc4514_string()}", issuer "{c.issuer.rfc4514_string()}"'
                           f"{' (self-signed)' if c.subject == c.issuer else ''}")
                out.append(f'{i2}  valid {c.not_valid_before_utc:%Y-%m-%d} .. {c.not_valid_after_utc:%Y-%m-%d}; key {pkn}; SAN {sans}; '
                           f'serial {c.serial_number:x}; SHA-256 {hashlib.sha256(der).hexdigest()}')
            except Exception as e:
                out.append(f'{i2}certificate {k}: {n} bytes (not X.509: {e}) {hx(der, 32)}')
            k += 1
    elif msg_t == 15:
        a, n = struct.unpack_from('>HH', body, 0); out.append(f'{i2}algorithm {SIGALGS.get(a, hex(a))}; signature {n} bytes')
    elif msg_t == 20:
        out.append(f'{i2}verify_data {len(body)} bytes')
    return out


def frames(p, conn, d, space, ind):
    o, out = 0, []
    while o < len(p):
        t, o0 = varint(p, o); o = o0
        name = FRAMES.get(t, 'STREAM' if 0x08 <= t <= 0x0f else f'unknown 0x{t:x}')
        if t == 0:
            n = 0
            while o < len(p) and p[o] == 0: o += 1; n += 1
            out.append(f'{ind}PADDING x{n + 1}'); continue
        if t in (1, 0x1e, 0x1f): out.append(f'{ind}{name}'); continue
        if t in (2, 3):
            la, o = varint(p, o); dl, o = varint(p, o); rc, o = varint(p, o); fr, o = varint(p, o); rng = []
            for _ in range(rc): g, o = varint(p, o); r, o = varint(p, o); rng.append((g, r))
            ecn = ''
            if t == 3:
                e0, o = varint(p, o); e1, o = varint(p, o); ec, o = varint(p, o); ecn = f', ECN {e0}/{e1}/{ec}'
            out.append(f'{ind}{name}: largest acknowledged {la}, ack delay {dl}, first range {fr}, {rc} more ranges {rng}{ecn}'); continue
        if t == 6:
            off, o = varint(p, o); n, o = varint(p, o); data = p[o:o + n]; o += n
            out.append(f'{ind}CRYPTO offset {off} length {n}')
            buf = conn.crypto.setdefault((d, space), {}); buf[off] = data
            # reassemble and parse complete TLS messages
            whole, x = b'', 0
            while x in buf: whole += buf[x]; x += len(buf[x])
            q = 0
            while q + 4 <= len(whole):
                mt = whole[q]; ml = int.from_bytes(whole[q + 1:q + 4], 'big')
                if q + 4 + ml > len(whole): break
                key = (d, space, q)
                if key not in conn.tls_seen:
                    conn.tls_seen.add(key)
                    out += parse_tls(mt, whole[q + 4:q + 4 + ml], ind + '  ', conn, 'server' if d == 'RX' else 'client')
                q += 4 + ml
            continue
        if t == 7:
            n, o = varint(p, o); out.append(f'{ind}NEW_TOKEN: {n}-byte address validation token {hx(p[o:o + n], 16)}'); o += n; continue
        if 0x08 <= t <= 0x0f:
            sid, o = varint(p, o); off = 0
            if t & 4: off, o = varint(p, o)
            if t & 2: n, o = varint(p, o)
            else: n = len(p) - o
            data = p[o:o + n]; o += n
            kind = ('client' if sid % 2 == 0 else 'server') + '-initiated ' + ('bidirectional' if sid & 2 == 0 else 'unidirectional')
            out.append(f'{ind}STREAM id {sid} ({kind}) offset {off} length {n}{" FIN" if t & 1 else ""}')
            s = conn.streams.setdefault((d, sid), {}); s[off] = data
            if t & 1: conn.streams.setdefault(('FIN', d, sid), True)
            continue
        if t in (0x10, 0x12, 0x13, 0x14, 0x16, 0x17):
            v, o = varint(p, o); out.append(f'{ind}{name}: {v}'); continue
        if t in (0x11, 0x15):
            s, o = varint(p, o); v, o = varint(p, o); out.append(f'{ind}{name}: stream {s}, {v}'); continue
        if t in (4, 5):
            s, o = varint(p, o); e, o = varint(p, o); extra = ''
            if t == 4: f, o = varint(p, o); extra = f', final size {f}'
            out.append(f'{ind}{name}: stream {s}, application error {e}{extra}'); continue
        if t == 0x18:
            seq, o = varint(p, o); rp, o = varint(p, o); n = p[o]; cid = p[o + 1:o + 1 + n]; tok = p[o + 1 + n:o + 17 + n]; o += 17 + n
            out.append(f'{ind}NEW_CONNECTION_ID: sequence {seq}, retire prior to {rp}, id {cid.hex()}, stateless reset token {tok.hex()}'); continue
        if t == 0x19:
            seq, o = varint(p, o); out.append(f'{ind}RETIRE_CONNECTION_ID: sequence {seq}'); continue
        if t in (0x1a, 0x1b):
            out.append(f'{ind}{name}: {p[o:o + 8].hex()}'); o += 8; continue
        if t in (0x1c, 0x1d):
            e, o = varint(p, o); ft = None
            if t == 0x1c: ft, o = varint(p, o)
            n, o = varint(p, o); reason = p[o:o + n]; o += n
            en = TERR.get(e, f'0x{e:x}' if e < 0x100 or e > 0x1ff else f'TLS alert {e - 0x100}') if t == 0x1c else f'application code {e}'
            out.append(f'{ind}{name}: {en}{"" if ft is None else f", frame type 0x{ft:x}"}, reason "{reason.decode(errors="replace")}"'); continue
        if t in (0x30, 0x31):
            n = len(p) - o
            if t == 0x31: n, o = varint(p, o)
            out.append(f'{ind}{name}: {n} bytes {hx(p[o:o + n], 32)}'); o += n; continue
        if t == 0xaf:
            a, o = varint(p, o); b, o = varint(p, o); c, o = varint(p, o); dd, o = varint(p, o)
            out.append(f'{ind}ACK_FREQUENCY: sequence {a}, ack-eliciting threshold {b}, request max ack delay {c} us, reordering {dd}'); continue
        out.append(f'{ind}{name}: (unparsed rest {hx(p[o:], 32)})'); break
    return out


def packet(b, o, d, conn, dcid_len):
    first = b[o]; out = []
    if first & 0x80:
        ver = struct.unpack_from('>I', b, o + 1)[0]; q = o + 5
        dl = b[q]; dcid = b[q + 1:q + 1 + dl]; q += 1 + dl; sl = b[q]; scid = b[q + 1:q + 1 + sl]; q += 1 + sl
        if ver == 0:
            vs = [hex(x) for x in struct.unpack_from(f'>{(len(b) - q) // 4}I', b, q)]
            return len(b), [f'  Version Negotiation: dcid {dcid.hex()} scid {scid.hex()} versions {vs}']
        ty = (first >> 4) & 3
        space = {0: 'Initial', 1: '0-RTT', 2: 'Handshake', 3: 'Retry'}[ty] if ver == 1 else f'type {ty} (version 0x{ver:08x})'
        hdr = f'  {space} packet (long header): first byte 0x{first:02x} [form 1, fixed bit {(first >> 6) & 1}' + \
              (' -- GREASED, RFC 9287' if not first & 0x40 else '') + f'], version 0x{ver:08x}{" (QUIC v1)" if ver == 1 else ""}, ' + \
              f'destination connection id ({dl}) {dcid.hex()}, source connection id ({sl}) {scid.hex()}'
        if ty == 3:
            return len(b), [hdr, f'    Retry token {hx(b[q:-16], 24)}, integrity tag {b[-16:].hex()}']
        if ty == 0:
            tl, q = varint(b, q); tok = b[q:q + tl]; q += tl; hdr += f', token ({tl}) {hx(tok, 16) if tl else "-"}'
            if d == 'TX' and ('TX', 'Initial') not in conn.keys: conn.initial(dcid); hdr += '  [Initial keys derived from this dcid]'
        if d == 'TX': conn.cid_len['RX'] = sl
        else: conn.cid_len['TX'] = sl
        ln, q = varint(b, q); end = q + ln; pn_off = q
        out.append(hdr + f', length {ln}')
    else:
        space = '1-RTT'; dl = dcid_len; dcid = b[o + 1:o + 1 + dl]; pn_off = o + 1 + dl; end = len(b)
        out.append(f'  1-RTT packet (short header): first byte 0x{first:02x} [form 0, fixed bit {(first >> 6) & 1}' +
                   (' -- GREASED, RFC 9287' if not first & 0x40 else '') + f'], destination connection id ({dl}) {dcid.hex()}')
    k = conn.keys.get((d, space))
    if not k:
        out.append(f'    (no {space} keys for {d}: payload {end - pn_off} bytes stays encrypted)'); return end, out
    sample = b[pn_off + 4:pn_off + 20]; m = k.mask(sample)
    f0 = first ^ (m[0] & (0x0f if first & 0x80 else 0x1f)); pnl = (f0 & 3) + 1
    pnb = bytes(x ^ y for x, y in zip(b[pn_off:pn_off + pnl], m[1:1 + pnl]))
    trunc = int.from_bytes(pnb, 'big'); sk = (d, 'Initial' if space == 'Initial' else 'Handshake' if space == 'Handshake' else 'App')
    pn = decode_pn(trunc, pnl * 8, conn.largest.get(sk, -1)); conn.largest[sk] = max(conn.largest.get(sk, -1), pn)
    aad = bytes([f0]) + b[o + 1:pn_off] + pnb
    extra = f', reserved bits {(f0 >> 2) & 3}' if first & 0x80 else f', spin bit {(f0 >> 5) & 1}, reserved bits {(f0 >> 3) & 3}, key phase {(f0 >> 2) & 1}'
    out.append(f'    header protection removed: packet number length {pnl}, packet number {pn}{extra}')
    try:
        pt = k.open(pn, aad, b[pn_off + pnl:end])
    except Exception as e:
        out.append(f'    decryption FAILED ({e.__class__.__name__})'); return end, out
    out.append(f'    payload {len(pt)} bytes + 16-byte AEAD tag ({k.name}):')
    out += frames(pt, conn, d, space, '      ')
    return end, out


def main():
    a = [x for x in sys.argv[1:] if not x.startswith('--')]
    peer = sys.argv[sys.argv.index('--peer') + 1] if '--peer' in sys.argv else None
    if peer: a = [x for x in a if x != peer]
    dgs = read_trace(a[0]); keylog = read_keylog(a[1] if len(a) > 1 else None)
    if not peer:
        peer = next((d['peer'] for d in dgs if d['dir'] == 'TX'), None)
    dgs = [d for d in dgs if d['peer'] == peer]
    conn = Conn(keylog)
    print(f'{len(dgs)} datagrams with {peer}; key log: {len(keylog)} secrets')
    for i, d in enumerate(dgs):
        b = d['b']
        print(f'\n#{i + 1} {d["dir"]} {"client -> server" if d["dir"] == "TX" else "server -> client"}, UDP {d["len"]} bytes'
              + (f' (trace holds {len(b)})' if len(b) != d['len'] else ''))
        o = 0
        while o < len(b):
            if not b[o] & 0x80 and o > 0 and all(x == 0 for x in b[o:]):
                print(f'  {len(b) - o} bytes of zero padding after the packets'); break
            end, lines = packet(b, o, d['dir'], conn, conn.cid_len.get(d['dir'], 8))
            print('\n'.join(lines)); o = end
    print('\n== stream data (reassembled)')
    for key in sorted(k for k in conn.streams if k[0] != 'FIN'):
        d, sid = key; parts = conn.streams[key]; data, x = b'', 0
        while x in parts: data += parts[x]; x += len(parts[x])
        fin = ('FIN', d, sid) in conn.streams
        print(f'{"client -> server" if d == "TX" else "server -> client"} stream {sid}: {len(data)} bytes{" (FIN)" if fin else ""}')
        for o in range(0, len(data), 32):
            ch = data[o:o + 32]
            print(f'  {o:04x} {ch.hex():<64} {"".join(chr(c) if 32 <= c < 127 else "." for c in ch)}')


if __name__ == '__main__':
    main()
