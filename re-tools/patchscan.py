#!/usr/bin/env python3
"""Every client-exe patch point / hook the loader uses, found in any number of client builds the way the loader finds them --
statically (nothing is run). The porting check for a new Project Reclaimer release (shim/docs/porting.md).

    python3 re-tools/patchscan.py                      # every release in the corpus (shim/tools/fetch-releases.sh) + client/
    python3 re-tools/patchscan.py client/x.exe ...     # given exes
    python3 re-tools/patchscan.py --md                 # a markdown table (porting.md / patch-addresses.md)
    python3 re-tools/patchscan.py --json               # machine-readable (one object per build)
    python3 re-tools/patchscan.py --diff A.exe B.exe   # the two builds side by side, changed points marked
    python3 re-tools/patchscan.py --rev abce1d1 ...    # with the signatures of that commit (what the loader built from it finds)

The signatures are READ FROM THE C SOURCES (shim/src/peload.c g_sigpatches, shim/src/stats.c g_sig_<point> lists), so this never
drifts from what the loader uses; if the sources gain a signature this script doesn't know how to judge, it says so and
exits 2. The checks mirror the C:
  sig_find      unique match in the executable sections (overlapping matches counted), first byte literal
  g_sigpatches  per slot the first variant (table order) that matches uniquely AND whose original bytes at every patch
                offset are as expected; "orig!=" = a unique match with other bytes at the patch site (a false positive caught)
  stats lists   per point the first signature (list order) that matches uniquely AND passes the point's check:
  set_muted     its first `lock cmpxchg [rip+d],cl` within 0x40 bytes is also locked by `mov dl,1; xor eax,eax; lock
                cmpxchg [rip+d],dl` (is_muted) somewhere (st_voice_install)
  voice_recv / push_history   the match's first 19 bytes equal the pattern's (the entry hook's compared prologue)
  moderation    the "You are banned ..." literal -> a lea to it -> the first `lock cmpxchg byte [rip+d]` + jne within 0x800
                bytes before -> lock; players Vec len = most common `imul r64,[rip+d],0x68` target in (lock+8, lock+0x400]
  max_players   either getter signature -> its `call clone; test rax,rax` -> clone prologue check -> the Arc slot + the
                `movzx esi, byte [rax+d32]` offset (st_slots_install)
  cast_vote     signature + a Vote message build (movabs 0x80000000000000xx; mov [rbp-0x58]) within 0x120 bytes;
  ballot        cache signature -> cache .. receiver size, plausible 0x100 .. 0x1000 (st_bv_install)
Presence: FIRST (below) is the release each point first exists in (the static survey of every release,
shim/docs/patch-addresses.md section 3); a build at or after it where the point isn't found is a MISS -- a false negative
to port -- unless that section documents the miss (the old releases our signatures never covered: "MISS (known, real
<rva>)" with the anchor-found place). Exit status: 0 = nothing unexpected, 1 = an undocumented MISS, orig!=, TWO variants
or a failed check, 2 = the sources changed in a way this script doesn't understand.
"""
import sys, os, re, struct, glob, json, collections

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..'))
CORPUS = os.path.expanduser('~/Library/Application Support/ReclaimerMac/releases')


# ---- PE image, mapped like the loader maps it: re-tools/pelib.py ---------------------------------------------------------
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pelib import Image   # noqa: E402


def sig_bytes(sig):   # sig_parse: ?? -> 0
    return bytes(0 if t == '??' else int(t, 16) for t in sig.split())


# ---- the loader's signatures, from its sources --------------------------------------------------------------------------
def c_strings(body):
    return re.findall(r'"((?:[0-9A-F]{2}|\?\?)(?: (?:[0-9A-F]{2}|\?\?))+)"', body)


def func_body(src, name):
    m = re.search(r'static [^\n;]*\b' + re.escape(name) + r'\(void\)\{', src)
    if not m: return None
    i = m.end(); depth = 1
    while depth and i < len(src):
        depth += {'{': 1, '}': -1}.get(src[i], 0); i += 1
    return src[m.start():i]


def source(path, rev=None):
    if rev is None: return open(os.path.join(ROOT, path)).read()
    import subprocess   # the file as it was in that commit (what the loader of that commit would find)
    return subprocess.run(['git', '-C', ROOT, 'show', f'{rev}:{path}'], check=True, capture_output=True, text=True).stdout


def load_sources(rev=None):
    pc = source('shim/src/peload.c', rev); sc = source('shim/src/stats.c', rev)
    t = pc[pc.index('static const SigPatch g_sigpatches[]={'):]; t = t[:t.index('\n};')]
    ent = re.findall(r'\{"([^"]+)","([^"]+)",(\d),"([0-9A-F? ]+)",\s*(\d),\{([^}]*)\},\{([^}]*)\},\{([^}]*)\}\}', t, re.S)
    sigpatches = []
    for name, slot, dflt, sig, n, offs, orig, repl in ent:
        sigpatches.append(dict(name=name, slot=slot, dflt=int(dflt), sig=sig, offs=[int(x, 0) for x in offs.split(',')][:int(n)],
                               orig=re.findall(r'"([^"]*)"', orig)[:int(n)], repl=re.findall(r'"([^"]*)"', repl)[:int(n)]))
    rows = len(re.findall(r'\{"[^"]+","[^"]+",\d,"', t))   # table rows (each starts {"name","slot",dflt,"sig")
    if len(sigpatches) != rows: raise SystemExit(f'patchscan: parsed {len(sigpatches)} of {rows} g_sigpatches rows -- update the parser')
    st = {}
    for name in STATS_POINTS:   # stats.c's per-point signature lists (g_sig_<point>[] = { "..", .., 0 })
        m = re.search(r'static const char\*const g_sig_' + name + r'\[\]=\{(.*?)\n\s*0 \};', sc, re.S)
        if m: st[name] = c_strings(m.group(1))
    if not st:   # sources before 2026-10-24 (patchscan --rev of an older commit): the strings inside the install functions
        want = {'st_voice_install': 2, 'st_chat_install': 1, 'st_slots_install': 2, 'st_bv_install': 2}; old = {}
        for fn, k in want.items():
            b = func_body(sc, fn)
            if b is None or len(c_strings(b)) != k: raise SystemExit(f'patchscan: stats.c {fn} is not what this script knows (exit 2)')
            old[fn] = c_strings(b)
        st = dict(set_muted=old['st_voice_install'][:1], voice_recv=old['st_voice_install'][1:], push_history=old['st_chat_install'],
                  max_players=old['st_slots_install'], cast_vote=old['st_bv_install'][:1], ballot=old['st_bv_install'][1:])
    for name in STATS_POINTS:
        if not st.get(name):
            print(f'patchscan: stats.c has no g_sig_{name} list: update patchscan.py', file=sys.stderr); sys.exit(2)
    return sigpatches, st


# ---- presence: the release each point first exists in (static survey of every release, shim/docs/patch-addresses.md
# section 3: the parenthesised cells are places our signatures miss). A build at or after it that lacks the point is a MISS
# (a false negative to port); an unknown build (no version in its path) is assumed to have every point.
STATS_POINTS = ('set_muted', 'voice_recv', 'push_history', 'max_players', 'cast_vote', 'ballot')
# points the loader no longer needs by default (still reported, never a failure): the xaudio patches, since halo3's XAudio2
# delay slots are left lazy (2026-10-24) and the client's check passes by itself; they apply only with PELOAD_XA_EAGER=1
OPTIONAL = {'xaudio1', 'xaudio2', 'xaudio3', 'xaudio4'}
FIRST = {'ui-reentrancy': (0, 9, 2), 'xaudio': (0, 8, 1), 'watchdog': (0, 8, 1), 'set_muted': (0, 8, 1), 'voice_recv': (0, 8, 1),
         'push_history': (0, 8, 1), 'moderation': (0, 9, 2), 'max_players': (0, 9, 2), 'cast_vote': (0, 9, 15)}
DL_LOCK = bytes([0xB2, 0x01, 0x31, 0xC0, 0xF0, 0x0F, 0xB0, 0x15])   # mov dl,1 ; xor eax,eax ; lock cmpxchg [rip+d],dl (is_muted's lock)


def set_muted_lock(im, fn):
    """the lock byte set_muted takes: its first `lock cmpxchg byte [rip+d],cl` (F0 0F B0 0D) within 0x40 bytes"""
    q = im.b.find(b'\xf0\x0f\xb0\x0d', fn, fn + 0x40)
    return q + 8 + im.i32(q + 4) if q >= 0 else None


def dl_lockers(im, lock):
    """code taking the same lock with dl (is_muted) -- the Forge setter that shares set_muted's shape in 0.8.16 .. 0.9.1
    has none"""
    out = []; i = 0x1000
    while True:
        i = im.b.find(DL_LOCK, i)
        if i < 0 or i + 12 > im.size: return out
        if i + 12 + im.i32(i + 8) == lock: out.append(i)
        i += 1


DOC_POINT = {'xaudio slot 1 jne': 'xaudio1', 'xaudio slot 2 jne': 'xaudio2', 'xaudio slot 3 jne': 'xaudio3', 'xaudio slot 4 jne': 'xaudio4',
             'watchdog je (sig+0xa)': 'watchdog', 'set_muted (entry)': 'set_muted', 'voice_recv (entry)': 'voice_recv',
             'push_history (entry)': 'push_history', 'ui-reentrancy jne (sig+8)': 'ui-reentrancy'}
_DOC = None


def doc_truth():
    """the places shim/docs/patch-addresses.md section 3 gives for the client exe ({(tag, point): rva}): found by anchors
    (string references, halo3 constants, the call graph), including where our signatures miss them ("(58a775)") or hit
    the wrong function ("**414520 !** (real 14aa00)" -> 14aa00)"""
    global _DOC
    if _DOC is not None: return _DOC
    doc = open(os.path.join(ROOT, 'shim/docs/patch-addresses.md')).read()
    sec = doc[doc.index('## 3. Where each point is'):doc.index('## 4.')]; _DOC = {}
    for tbl in re.split(r'\n### ', sec):
        if not tbl.startswith('Client exe, v0.8') and not tbl.startswith('Client exe, v0.9.7'): continue   # the per-release grids
        lines = [l for l in tbl.splitlines() if l.startswith('|')]
        tags = [c.strip() for c in lines[0].split('|')[2:-1]]
        for l in lines[2:]:
            c = [x.strip() for x in l.split('|')[1:-1]]
            pt = DOC_POINT.get(c[0])
            if not pt: continue
            for tag, v in zip(tags, c[1:]):
                m = re.search(r'real ([0-9a-f]+)\)', v) or re.fullmatch(r'\(?([0-9a-f]+)\)?(?: \?| =fixed)?', v)
                if m: _DOC[(tag, pt)] = int(m.group(1), 16)
    return _DOC


def present(ver, point):
    if ver is None: return True
    return ver >= FIRST.get(point, (0, 0, 0))


# ---- the checks ---------------------------------------------------------------------------------------------------------
def scan(im, sigpatches, st, ver=None, tag=None):
    R = collections.OrderedDict()
    # signature patches: per slot, first variant (table order) unique + original bytes
    slots = collections.OrderedDict()
    for sp in sigpatches: slots.setdefault(sp['slot'], []).append(sp)
    for slot, vs in slots.items():
        # exactly the loader's reentrancy_fix: EVERY variant is tried; unique + expected bytes -> applied; unique with other
        # bytes or ambiguous -> it prints "NOT applied" for that variant
        applied = []; notes = []
        for vi, sp in enumerate(vs):
            hit, nh = im.find_unique(sp['sig'])
            if hit is None:
                if nh > 1: notes.append(f'v{vi}:ambiguous({nh})')
                continue
            ok = all(im.b[hit + o:hit + o + len(sig_bytes(ob))] == sig_bytes(ob) for o, ob in zip(sp['offs'], sp['orig']))
            if not ok: notes.append(f'v{vi}:orig!=@{hit + sp["offs"][0]:#x}'); continue
            applied.append((vi, hit + sp['offs'][0]))
        if len(applied) == 1:
            vi, rva = applied[0]
            res = dict(status='ok', rva=rva, variant=vi, name=vs[0]['name'])
            if notes: res['notes'] = notes; res['noisy'] = True   # works; the loader also logs the other variants' NOT applied
        elif applied:   # two variants applied to one build: a too-loose signature
            res = dict(status='TWO variants', rva=applied[0][1], name=vs[0]['name'], notes=notes + [f'v{vi} applied @{a:#x}' for vi, a in applied])
        else:
            res = dict(status='orig!=' if any('orig!=' in n for n in notes) else ('AMBIG' if notes else 'none'), notes=notes, name=vs[0]['name'])
        R[slot] = res
    # set_muted: the first unique match whose lock is also taken by an is_muted (stats.c st_voice_install)
    R['set_muted'] = dict(status='none'); amb = False
    for sig in st['set_muted']:
        hit, nh = im.find_unique(sig)
        if hit is None: amb |= nh > 1; continue
        lockb = set_muted_lock(im, hit)
        if lockb is not None and dl_lockers(im, lockb): R['set_muted'] = dict(status='ok', rva=hit, muted=lockb); break
        R['set_muted'] = dict(status='no is_muted pair', rva=hit)
    if R['set_muted']['status'] == 'none' and amb: R['set_muted'] = dict(status='AMBIG')
    # entry hooks: the first unique match whose first 19 bytes equal the pattern's (entry_hook compares them; a ?? there
    # is 0 in the pattern)
    for key in ('voice_recv', 'push_history'):
        R[key] = dict(status='none')
        for sig in st[key]:
            hit, nh = im.find_unique(sig)
            if hit is None:
                if nh > 1 and R[key]['status'] == 'none': R[key] = dict(status='AMBIG')
                continue
            if im.b[hit:hit + 19] != sig_bytes(sig)[:19]: R[key] = dict(status='prologue has ?? bytes', rva=hit); continue
            R[key] = dict(status='ok', rva=hit); break
    R['moderation'] = moderation(im)
    R['max_players'] = max_players(im, st['max_players'])
    R['cast_vote'] = cast_vote(im, st['cast_vote'], st['ballot'])
    for k, v in R.items():
        p = present(ver, v.get('name', k) if k in ('xaudio1', 'xaudio2', 'xaudio3', 'xaudio4') else k)
        v['anchor'] = p
        if v['status'] != 'ok' and p:   # the feature is there, the signature isn't
            real = doc_truth().get((tag, k)) if tag else None
            if real is not None: v['why'] = v['status']; v['status'] = 'MISS(known)'; v['real'] = real   # documented: an old build
            else: v['status'] = 'MISS:' + v['status']
    return R


def moderation(im):
    needle = b'You are banned from this server. Reason: '; s = None
    for va, vs in im.data:
        j = im.b.find(needle, va, va + vs)
        if j >= 0: s = j; break
    if s is None: return dict(status='none')
    lock = None
    for va, vs in im.exec:
        for m in re.finditer(rb'[\x48\x4c]\x8d[\x05\x0d\x15\x1d\x25\x2d\x35\x3d]', im.b[va:va + vs]):
            k = va + m.start(); t = k + 7 + im.i32(k + 3)
            if not (s - 8 <= t <= s): continue
            q = k
            while q > k - 0x800 and q > va:
                if im.b[q:q + 4] == b'\xf0\x0f\xb0\x0d' and im.b[q + 8] == 0x75: lock = q + 8 + im.i32(q + 4); break
                q -= 1
            if lock is not None: break
        if lock is not None: break
    if lock is None: return dict(status='no lock')
    c = collections.Counter()
    for va, vs in im.exec:
        for m in re.finditer(rb'[\x48\x4c]\x6b[\x05\x0d\x15\x1d\x25\x2d\x35\x3d]', im.b[va:va + vs]):
            k = va + m.start()
            if im.b[k + 7] != 0x68: continue
            t = k + 8 + im.i32(k + 3)
            if lock + 8 < t <= lock + 0x400: c[t] += 1
    if not c: return dict(status='no players Vec', lock=lock)
    L, n = c.most_common(1)[0]
    if n < 3: return dict(status=f'players Vec read {n}x (<3)', lock=lock)   # the real one: 6-7 sites (stats.c)
    return dict(status='ok', rva=lock, players=L - lock)


def max_players(im, sigs):
    """stats.c st_slots_install: the first unique getter match whose clone call and max_players offset check out"""
    res = dict(status='none')
    pro = bytes([0x56, 0x48, 0x83, 0xEC, 0x20, 0xB1, 0x01, 0x31, 0xC0, 0xF0, 0x0F, 0xB0, 0x0D])
    for s in sigs:
        hit, nh = im.find_unique(s)
        if hit is None: continue
        C = None
        for q in range(hit, hit + 0x30):
            if im.b[q] == 0xE8 and im.b[q + 5:q + 8] == b'\x48\x85\xc0': C = q + 5 + im.i32(q + 1); break
        if C is None or im.b[C:C + 13] != pro: res = dict(status='clone shape', rva=hit); continue
        L = C + 0x11 + im.i32(C + 0xd); off = None
        for q in range(hit, hit + 0x100):
            if im.b[q:q + 3] == b'\x0f\xb6\xb0' and im.b[q + 7:q + 11] == b'\xf0\x48\xff\x08': off = im.i32(q + 3); break
        if off is None or not (0x18 < off < 0x1000): res = dict(status='offset', rva=hit); continue
        return dict(status='ok', rva=hit, slot=L + 8, off=off)
    return res


def cast_vote(im, sigs, bsigs):
    """stats.c st_bv_install: the first unique cast_vote match that builds a Vote message, then the first unique ballot
    match with a plausible cache size"""
    res = dict(status='none'); fn = None
    for s in sigs:
        hit, nh = im.find_unique(s)
        if hit is None:
            if nh > 1 and res['status'] == 'none': res = dict(status='AMBIG')
            continue
        for q in range(hit, hit + 0x120):
            b = im.b[q:q + 14]
            if b[0:2] == b'\x48\xb8' and b[3:9] == b'\0' * 6 and b[9] == 0x80 and b[10:14] == b'\x48\x89\x45\xa8': fn = hit; break
        if fn is not None: break
        res = dict(status='no Vote message', rva=hit)
    if fn is None: return res
    res = dict(status='ballot none', rva=fn)
    for s in bsigs:
        h, nh = im.find_unique(s)
        if h is None:
            if nh > 1: res = dict(status='ballot AMBIG', rva=fn)
            continue
        cache = h + 9 + im.i32(h + 5); recv = h + 0x18 + im.i32(h + 0x14); sz = recv - cache
        if not (0x100 <= sz <= 0x1000): res = dict(status=f'ballot size {sz:#x}', rva=fn); continue
        return dict(status='ok', rva=fn, cache=cache, size=sz, ballot=h)
    return res


# ---- output ---------------------------------------------------------------------------------------------------------------
def cell(v):
    s = v['status']
    if s == 'ok':
        x = f"{v['rva']:x}"
        if 'variant' in v and v['variant']: x += f" v{v['variant']}"
        if v.get('noisy'): x += '*'   # (the loader logs another variant's NOT applied: see the notes)
        if 'players' in v: x += f" +{v['players']:x}"
        if 'size' in v: x += f" {v['size']:x}"
        if 'off' in v: x += f" +{v['off']:x}"
        return x
    if v.get('anchor') is False: return '(absent)'
    if s == 'MISS(known)': return f"miss (known: {v['real']:x})"
    return s


def version_of(path):
    m = re.search(r'v(\d+\.\d+\.\d+)', path)
    if m: return m.group(1)
    m = re.search(r'/v(\d+\.\d+\.\d+)/', path); return m.group(1) if m else os.path.basename(path)


def corpus():
    out = []
    for d in glob.glob(os.path.join(CORPUS, 'v*')):
        ex = [x for x in glob.glob(d + '/*.exe') if 'dedicated' not in x and 'rcon' not in x and 'editor' not in x]
        if ex: out.append(ex[0])
    def key(p): return tuple(int(x) for x in re.search(r'/v([\d.]+)/', p).group(1).split('.'))
    return sorted(out, key=key)


def main():
    a = sys.argv[1:]; md = '--md' in a; js = '--json' in a; diff = '--diff' in a; rev = None
    if '--rev' in a: i = a.index('--rev'); rev = a[i + 1]; del a[i:i + 2]
    files = [x for x in a if not x.startswith('--')] or corpus()
    if not files: raise SystemExit('no exes (run shim/tools/fetch-releases.sh, or pass exe paths)')
    sigpatches, st = load_sources(rev)
    rows = []
    for f in files:
        im = Image(f); tag = re.search(r'/(v[\d.]+)/', f); tag = tag.group(1) if tag else 'v' + version_of(f)
        m = re.fullmatch(r'v(\d+)\.(\d+)\.(\d+)', tag); ver = tuple(int(x) for x in m.groups()) if m else None
        rows.append((tag, scan(im, sigpatches, st, ver, tag)))
    cols = list(rows[0][1].keys())
    if js:
        print(json.dumps([{'build': t, **{k: v for k, v in r.items()}} for t, r in rows], indent=1, default=str)); return
    # bad: a point that should be there (its FIRST release or later) and isn't, unless documented; a build before FIRST
    # rejecting a lookalike is right ("(absent)")
    bad = any(v.get('anchor') and v['status'] not in ('ok', 'MISS(known)') and k not in OPTIONAL for _, r in rows for k, v in r.items())
    if diff and len(rows) == 2:
        (ta, ra), (tb, rb) = rows
        print(f'| point | {ta} | {tb} | how |\n|---|---|---|---|')
        for c in cols:
            x, y = cell(ra[c]), cell(rb[c])
            if rb[c]['status'] != 'ok': how = 'not found (optional: not needed by default)' if c in OPTIONAL else '**BROKEN**: port it (re-tools/sigport.py)'
            elif ra[c]['status'] != 'ok': how = 'found again'
            elif ra[c].get('variant') != rb[c].get('variant'): how = f"another variant (v{rb[c].get('variant')})"
            elif x == y: how = 'same place'
            else: how = 'moved, same signature'
            print(f'| {c} | {x} | {y} | {how} |')
        sys.exit(1 if bad else 0)
    if md:
        print('| build | ' + ' | '.join(cols) + ' |'); print('|' + '---|' * (len(cols) + 1))
        for t, r in rows: print(f'| {t} | ' + ' | '.join(cell(r[c]) for c in cols) + ' |')
    else:
        w = max(len(c) for c in cols)
        for t, r in rows:
            print(f'== {t}')
            for c in cols: print(f'  {c.ljust(w)}  {cell(r[c])}' + (f"   ({'; '.join(r[c]['notes'])})" if r[c].get('notes') else ''))
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
