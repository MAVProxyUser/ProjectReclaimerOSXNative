#!/usr/bin/env python3
"""Make the native Reclaimer 0.10 app (macOS / Linux) accept a community profile made under the loader, WITHOUT changing the
identity (same Ed25519 key -> same community profile id and name).

Why: the 0.9 Windows client protects its key with DPAPI ("protection": "windows-dpapi-user-v1", profile "version": 1). Under
the loader CryptProtectData is a passthrough, so "protected_seed" holds the raw 32-byte seed. The native app unlocks DPAPI only
for blobs made by Wine's crypt32 and calls ours "not made by Wine" -> "locked here" (it does not replace it, but the player
can't use it). The native app's own format is "version": 2 with a plain "seed" (no protection field).

    profile-native.py check <profile.json>                      the seed derives identity.public_key? (prints the public key id only)
    profile-native.py convert <src profile.json> <dst profile.json> [--wine USER]
        writes a version-2 profile (seed) -- or with --wine, a version-1 profile whose protected_seed is a Wine CryptProtectData
        blob for Wine user USER (the native app tries WINEUSERNAME, LOGNAME, then "steamuser") -- after checking that the seed
        derives the profile's public key, and that the result decodes back to the same seed. Refuses an existing <dst>.

The seed is never printed. Wine's CryptProtectData (dlls/crypt32/protectdata.c): key = CryptDeriveKey(3DES-168,
SHA1(username + NUL + "I'm hunting wabbits" + salt16 +
entropy "MCC-community/profile/Ed25519/v1")) (MS expansion: SHA1(0x36-pad ^ h) || SHA1(0x5c-pad ^ h), first 24
bytes), 3DES-CBC with a zero IV and PKCS#5 padding, fingerprint = SHA1(plaintext); serialized as count0=1, info0="Wine Crypt32
ok\\0" (16 raw bytes, no length), count1=1, info1 (the same), null0=0, description (UTF-16, NUL-terminated), cipher_alg
0x6603, key_len 168, data0, null1=0, hash_alg 0x8004, hash_len 160, salt, cipher, fingerprint (from the description on, each
blob = u32 length + bytes)."""
import sys, os, json, hashlib, struct

def seed_of(prof):
    if prof.get('version') == 2 and 'seed' in prof: return bytes.fromhex(prof['seed'])
    if prof.get('version') == 1 and prof.get('protection') == 'windows-dpapi-user-v1':
        raw = bytes.fromhex(prof['protected_seed'])
        if len(raw) == 32: return raw                                   # loader passthrough: the raw seed
        sys.exit('protected_seed is a real DPAPI blob (not a loader passthrough): unlock it where it was made')
    sys.exit('unsupported profile format')

def pub_of(seed):
    from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
    from cryptography.hazmat.primitives import serialization
    k = Ed25519PrivateKey.from_private_bytes(seed).public_key()
    return k.public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw)

def check(prof):
    seed = seed_of(prof); pub = pub_of(seed); want = bytes(prof['identity']['public_key'])
    if pub != want: sys.exit('the seed does NOT derive identity.public_key: refusing')
    return seed, pub

SECRET = b"I'm hunting wabbits"; MAGIC = b'Wine Crypt32 ok\0'
ENTROPY = b'MCC-community/profile/Ed25519/v1'                      # the client's pOptionalEntropy (hashed after the salt)

def wine_key(user, salt):
    h = hashlib.sha1(user.encode() + b'\0' + SECRET + salt + ENTROPY).digest()
    b1 = bytes(0x36 ^ (h[i] if i < len(h) else 0) for i in range(64)); b2 = bytes(0x5c ^ (h[i] if i < len(h) else 0) for i in range(64))
    return (hashlib.sha1(b1).digest() + hashlib.sha1(b2).digest())[:24]

def tdes(key, data, enc):
    from cryptography.hazmat.primitives.ciphers import Cipher, modes
    from cryptography.hazmat.decrepit.ciphers.algorithms import TripleDES
    c = Cipher(TripleDES(key), modes.CBC(b'\0' * 8)); o = c.encryptor() if enc else c.decryptor()
    return o.update(data) + o.finalize()

def blob(b): return struct.pack('<I', len(b)) + b

def wine_protect(plain, user):
    salt = os.urandom(16); key = wine_key(user, salt); pad = 8 - len(plain) % 8
    cipher = tdes(key, plain + bytes([pad]) * pad, True)
    return (struct.pack('<I', 1) + MAGIC + struct.pack('<I', 1) + MAGIC + struct.pack('<I', 0) + blob('\0'.encode('utf-16-le'))
            + struct.pack('<II', 0x6603, 168) + blob(MAGIC) + struct.pack('<III', 0, 0x8004, 160) + blob(salt) + blob(cipher)
            + blob(hashlib.sha1(plain).digest()))

def wine_unprotect(data, user):
    p = 0
    def u32():
        nonlocal p; v = struct.unpack_from('<I', data, p)[0]; p += 4; return v
    def bl():
        nonlocal p; n = u32(); v = data[p:p + n]; p += n; return v
    def raw16():
        nonlocal p; v = data[p:p + 16]; p += 16; return v
    c0 = u32(); i0 = raw16(); c1 = u32(); i1 = raw16(); n0 = u32(); desc = bl(); alg, klen = u32(), u32(); d0 = bl(); n1 = u32()
    halg, hlen = u32(), u32(); salt = bl(); cipher = bl(); fp = bl()
    assert (c0, c1, n0, n1, i0, alg, klen, halg) == (1, 1, 0, 0, MAGIC, 0x6603, 168, 0x8004), 'not a Wine blob'
    pt = tdes(wine_key(user, salt), cipher, False); pt = pt[:-pt[-1]]
    assert hashlib.sha1(pt).digest() == fp, 'fingerprint mismatch (wrong user?)'
    return pt

def main():
    a = sys.argv[1:]
    if not a or a[0] not in ('check', 'convert'): sys.exit(__doc__)
    prof = json.load(open(a[1]))
    seed, pub = check(prof)
    print(f'{a[1]}: version {prof.get("version")}, the seed derives identity.public_key ({pub.hex()[:16]}...)')
    if a[0] == 'check': return
    dst = a[2]
    if os.path.exists(dst): sys.exit(f'{dst} exists: refusing to overwrite')
    out = {k: v for k, v in prof.items() if k not in ('protection', 'protected_seed', 'seed', 'version')}
    if '--wine' in a:
        user = a[a.index('--wine') + 1]; b = wine_protect(seed, user)
        assert wine_unprotect(b, user) == seed
        out = {'version': 1, **out, 'protection': 'windows-dpapi-user-v1', 'protected_seed': b.hex()}
    else:
        out = {'version': 2, **out, 'seed': seed.hex()}
    fd = os.open(dst, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, 'w') as f: json.dump(out, f, indent=2); f.write('\n')
    back = json.load(open(dst))                                          # read the written file back: same seed, same public key
    s2 = wine_unprotect(bytes.fromhex(back['protected_seed']), user) if '--wine' in a else seed_of(back)
    assert s2 == seed and pub_of(s2) == bytes(back['identity']['public_key'])
    print(f'wrote {dst} (version {out["version"]}, mode 600)')

if __name__ == '__main__':
    main()
