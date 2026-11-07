#!/usr/bin/env python3
"""List Project Reclaimer 0.10+ servers: players, name, map / mode, address, ping, mods (shim/docs/server-status.md, 0.10).

    python3 shim/tools/servers10.py [count]            # the busiest 25 (default)
    python3 shim/tools/servers10.py --json             # every answer, raw (one JSON object per line)
    python3 shim/tools/servers10.py --names 10         # also the players' names
Join one with:  bash shim/native.sh --join-server <address>   (NATIVE_REAL=1 for your own profile)

What it does is what the 0.10 client's server browser does (traced 2026-11-07 with nhook's NHOOK_UDP_HEX):
  1. GET http://<master>/v3/discovery from the built-in masters and the masters the app remembered (its
     discovery/h3list/peers.json): {"version":3, "listings":[{"listing":{key, address, rendezvous, origin?, issued,
     expires}, "signature"}], "masters", ...} -- signed addresses only.
  2. one UDP datagram per listed address: a6 01 00 <8-byte nonce>, zero-padded to 1229 bytes (the client pads it: a reply
     is never larger than the request); the server answers a7 <the same nonce> {"info":{name, map, mode, players,
     max_players, password, dedicated, host_name, variant, version, player_names, mods[{id, title, size, available,
     maps, campaign}], joins_closed, max_ping, queue_room, ...}, "protocol":1, "key", "address"}.
No player identity is sent (the query is the same for everyone). Signatures and keys are not checked here (the client
does). The 0.9 client's `discover` (shim/tools/servers.sh) does not list 0.10 servers: 0.9 queried over QUIC.
"""
import json, os, re, select, socket, sys, time, urllib.request

MASTERS = ['http://148.251.153.44:44340', 'http://23.159.176.138:49175', 'http://148.251.153.44:49175', 'http://23.159.176.140:49175']
PEERS = os.path.expanduser('~/Library/Application Support/ProjectReclaimer/discovery/h3list/peers.json')
_CTL = re.compile('[\x00-\x1f\x7f-\x9f‎‏‪-‮⁦-⁩]')
def C(v):   # server-supplied text: no terminal control / escape / bidi characters
    return _CTL.sub('', str(v))

def masters(extra=8):
    out = list(MASTERS)
    try:
        for p in sorted(json.load(open(PEERS)), key=lambda p: p.get('failures', 0)):
            o = p.get('origin')
            if o and o.startswith('http') and o not in out and len(out) < len(MASTERS) + extra: out.append(o)
    except Exception: pass
    return out

def listings(ms, timeout=6):
    seen, answered = {}, 0
    for m in ms:
        try:
            with urllib.request.urlopen(urllib.request.Request(m.rstrip('/') + '/v3/discovery', headers={'User-Agent': 'reclaimer-list'}), timeout=timeout) as r:
                d = json.load(r)
            answered += 1
            for x in d.get('listings', []):
                l = x.get('listing') or {}
                a = l.get('address')
                if a and a not in seen: seen[a] = l
        except Exception as e:
            print(f'  master {m}: {e}', file=sys.stderr)
    return seen, answered

def query(addrs, timeout=2.5, tries=2):
    """one datagram per address from one local port (like the client); a second try for the silent ones"""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.setblocking(False)
    want, got, t0 = {}, {}, {}
    def send(a):
        h, p = a.rsplit(':', 1)
        try: ip = socket.gethostbyname(h.strip('[]'))
        except OSError: return
        n = os.urandom(8); want[(ip, int(p))] = (a, n); t0[a] = time.time()
        q = bytes([0xa6, 1, 0]) + n; q += bytes(1229 - len(q))
        try: s.sendto(q, (ip, int(p)))
        except OSError: pass
    def drain(wait):
        r, _, _ = select.select([s], [], [], wait)
        while r:
            try: d, src = s.recvfrom(65536)
            except OSError: return
            w = want.get(src)
            if w and len(d) >= 10 and d[0] == 0xa7 and d[1:9] == w[1] and w[0] not in got:
                try: j = json.loads(d[9:])
                except ValueError: j = None
                if isinstance(j, dict): j['ping_ms'] = int((time.time() - t0[w[0]]) * 1000); got[w[0]] = j
            r, _, _ = select.select([s], [], [], 0)
    for k in range(tries):
        for a in addrs:
            if a not in got: send(a)
            drain(0.002)   # (a burst of a few hundred datagrams: paced a little; answers read as they come, for their ping)
        end = time.time() + timeout
        while time.time() < end and len(got) < len(addrs): drain(0.1)
    return got

def main():
    args = sys.argv[1:]; raw = '--json' in args; names = '--names' in args
    nums = [int(a) for a in args if a.isdigit()]; N = nums[0] if nums else 25
    ms = masters(); L, nm = listings(ms)
    if not L: sys.exit(f'no master answered ({len(ms)} asked)')
    got = query(list(L))
    if raw:
        for a, j in got.items(): print(json.dumps(dict(j, address=a, listing=L[a])))
        return
    live = sorted(got.items(), key=lambda kv: -(kv[1].get('info') or {}).get('players', 0))
    print(f'{len(got)} of {len(L)} listed servers answered ({nm} of {len(ms)} masters)\n')
    for a, j in live[:N]:
        i = j.get('info') or {}
        flags = (' [password]' if i.get('password') else '') + (' [joins closed]' if i.get('joins_closed') else '') + ('' if i.get('dedicated', True) else ' [player host]')
        print(f"{i.get('players', 0):>3}/{i.get('max_players', '?'):<3} {a:<22} {C(i.get('name', ''))[:46]:<46} {C(i.get('map', ''))[:20]:<20} {C(i.get('mode', ''))[:28]:<28} {j['ping_ms']:>4} ms {C(i.get('version', ''))}{flags}")
        for m in i.get('mods') or []:
            print(f"{'':8}mod: {C(m.get('title') or m.get('id', '?')[:16])} ({m.get('size', 0) / 1e9:.1f} GB{', not available' if not m.get('available', True) else ''})")
        if names and i.get('player_names'): print(f"{'':8}players: {C(', '.join(i['player_names']))[:300]}")

if __name__ == '__main__':
    main()
