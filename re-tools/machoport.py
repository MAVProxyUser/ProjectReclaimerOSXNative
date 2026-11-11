#!/usr/bin/env python3
"""Port nhook's fixed app addresses (shim/native) from one native Reclaimer build to another -- the arm64 Mach-O counterpart of
sigport.py / patchscan.py for the 0.9 PE clients. Every point the dylib uses is listed in POINTS below with its address in the
reference build (0.10.0) and how to find it again:
  code  a function or an instruction inside one: the function's instructions with every build-dependent immediate masked
        (b / bl targets, adrp pages, the add / ldr / str offsets that follow an adrp, literal loads) are looked up in the new
        build's functions (LC_FUNCTION_STARTS); same masked function = found. Else windows of masked instructions around the
        point that occur exactly once in the new __text (strict masks, then loose: conditional branch offsets masked too).
  data  a global: one of its users (an adrp + add / ldr / ldrb / str site in the reference build) is ported as a code point,
        and the new address is read from the instructions found there (the adrp page + the offset).
Then each point's MEANING is checked in the new build (CHECKS: e.g. set_muted's first instructions reach the mute list, the voice
mixer's bl calls the Opus decode, the chat gate's ldrb reads the chat_log byte) and the log lines nst_on_line parses are looked
up as text (LOGTEXT). A point that is not found or fails its check is reported and left 0 in the table: the dylib keeps that
feature off ("hooks check": GET /hooks.json, the stats page's Hooks window, shim/tools/native-hooks-test.sh).
    python3 re-tools/machoport.py port  <reference binary> <new binary> [--apply]   ports every point; --apply adds / replaces
                                                     the new build's row in shim/native/napp_builds.h (keyed by its LC_UUID)
                                                     and in ~/Library/Application Support/ReclaimerMac/native/app-builds.txt
                                                     (read at run time by an nhook.dylib built without that row)
    python3 re-tools/machoport.py check <binary>     the table row for that binary (by UUID): every point's meaning check again
                                                     (exit 2 = no row for it: shim/native.sh then ports it by itself)
    python3 re-tools/machoport.py selftest <reference binary> | <sigs.json> <reference binary>   ports the reference build onto
                                                     itself, from the binary or from its signatures (must give its own addresses)
    python3 re-tools/machoport.py sigs <reference binary> [<out.json>]   the reference as signatures (default
                                                     re-tools/machoport-sigs.json): the functions every point needs, raw words --
                                                     `port re-tools/machoport-sigs.json <new binary>` then needs no old binary
                                                     (the app replaces itself on update; shim/native.sh ports from the sigs)
The binaries: ~/Library/Application Support/ReclaimerMac/native/v<version>/client/Project Reclaimer.app/Contents/MacOS/
project-reclaimer (the release zips, kept; shim/docs/native-client.md "When the app updates"). Needs numpy."""
import sys, os, re, struct, bisect, hashlib, json
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import machoref

HERE = os.path.dirname(os.path.abspath(__file__))
TABLE = os.path.join(HERE, '..', 'shim', 'native', 'napp_builds.h')
SIGS = os.path.join(HERE, 'machoport-sigs.json')   # the 0.10.0 reference as signatures (`sigs`): port needs no 0.10.0 binary
# the same rows for an already built nhook.dylib (a release bundle, no compiler): nhook.c app_build_pick reads it when the
# app's UUID is not in its compiled table
RUNTIME = os.path.join(os.path.expanduser('~'), 'Library', 'Application Support', 'ReclaimerMac', 'native', 'app-builds.txt')

# name, kind, reference (0.10.0) value, how, what it is. Kinds:
#   code    by its function's shape / a unique window (above)
#   data    how = reference code sites that reference it (each ported as a code point, the new target read there)
#   call    how = (callee field, offset of the point from the bl, the words that must come right before the bl): the bl to the
#           callee's new address whose preceding words are those (exact)
#   infunc  how = (field whose function holds it, mask, value, next word or None): the first instruction there with
#           (word & mask) == value (and that next word after it)
#   imm     how = (field, 'imm12'): a value decoded from the instruction at that field's address (not an address)
#   mean    found by meaning only (MEAN below; no reference shape)
POINTS = [
    ('image',       'data',   0x103f94000, [0x1004779f4, 0x10047ab60, 0x100531fe8, 0x1005391a4], "the guest halo3 image: a reserved range of the app"),
    ('codetab',     'data',   0x103f44418, [0x1032964c0, 0x10324d580], "{ptr, count} of the sorted {guest address, host code} pairs"),
    ('opus_decode', 'code',   0x1003f1e7c, None, "the app's Opus decode (opus-rs): x0 decoder, x1 packet, x2 length"),
    # (since 0.10.2 the registers vary -- mov x1, x26 ; mov x2, x25 and the entry in x22 --: the moves' sources are masked, and
    #  the id load is any sub x8, x<19..28>, #off ; ldr x<19..28>, [x8], its base = the mixer's entry register)
    ('mixer_call',  'call',   0x10215256c, ('opus_decode', -12, [(0xaa0003e1, 0xffe0ffff), (0xaa0003e2, 0xffe0ffff), (0x52807803, 0xffffffff)]),
                    "the voice mixer's decode call: mov x1, x? ; mov x2, x? ; mov w3, #0x3c0 ; bl decode"),
    ('mixer_id',    'infunc', 0x102152444, ('mixer_call', 0xffc0001f, 0xd1000008, (0xffffffe0, 0xf9400100), lambda w, n: 19 <= (w >> 5) & 31 <= 28 and 19 <= n & 31 <= 28 and 0x100 <= (w >> 10) & 0xfff < 0x800),
                    "the voice mixer's sender id load: sub x8, x<entry>, #<id offset> ; ldr x<n>, [x8]"),
    ('mixer_id_off','imm',    0x278,       ('mixer_id', 'imm12'), "where the mixer's sender entry keeps the peer id: x26 - this"),
    ('frame_fn',    'code',   0x1029ed000, None, "the app's per-frame function (host tools' frame hook)"),
    ('chat_gate_a', 'code',   0x100d2a6fc, None, "the chat code's chat_log test (adrp ; ldrb), first"),
    ('chat_gate_b', 'code',   0x100d2a720, None, "the chat code's chat_log test (adrp ; ldrb), second"),
    ('chat_log',    'data',   0x103efcc9b, [0x100d2a700, 0x100d2a724], "the chat_log byte (a field of the chat state)"),
    ('muted',       'data',   0x103f0a1e0, [0x102154f14, 0x10214dbb4], "the mute list: static Mutex<Vec<u64>> (lock +0, cap +0x10, ptr +0x18, len +0x20)"),
    ('set_muted',   'code',   0x102154ef0, None, "set_muted(id, bool)"),
    # (2026-11-10, 0.10.2: the sorted table above holds 561 functions, the other ~22k are in a hash map -- nhook.c ct_build merges both)
    ('codehash',    'mean',   0,           None, "the second guest -> host code table: a hash map {ptr, cap, shift} (0.10.2+; 0 = none in the build)"),
]
FIELDS = [p[0] for p in POINTS]

# log text the dylib's line parsers need (nhook.c on_line, nstats.c nst_on_line, nnet.c, nspawns.c): each must still be in the app
# ('linked recompiled engine, image at 0x' and '[menus] open' are not stored as plain text: the hooks check counts them live)
LOGTEXT = ['[switch] game ', 'leaving the game', '[loading] map ', '[online] joining ',
           '[online] peer ', '[capacity] ', 'game_globals', 'statborg', '[c1] game results: ', 'network sessions moved to ',
           '[voice] active ', '[menus] joining the game in progress', '[engine] HUD notice: ', '[shell] pregame lobby: ',
           '[chat] line ', '[playlist] ballot ', '[playlist] voting ', '[host] stopping']

# ---- instruction masks --------------------------------------------------------------------------------------------------
def masked(w, loose=False):
    """w: uint32 array of instructions -> the same with build-dependent immediates zeroed."""
    w = w.astype(np.uint32); m = w.copy()
    isb = (w & 0x7c000000) == 0x14000000; m[isb] = w[isb] & 0xfc000000                       # b / bl
    isadr = (w & 0x1f000000) == 0x10000000; m[isadr] = w[isadr] & 0x9f00001f                 # adr / adrp
    islit = (w & 0x3b000000) == 0x18000000; m[islit] = w[islit] & 0xff00001f                 # ldr (literal)
    # add #imm / ldr|str #imm whose base register an adrp set within the 8 instructions before
    rn = (w >> 5) & 31; after = np.zeros(len(w), bool)
    isadrp = (w & 0x9f000000) == 0x90000000
    for k in range(1, 9):
        prev = np.concatenate([np.zeros(k, np.uint32), w[:-k]]); pa = np.concatenate([np.zeros(k, bool), isadrp[:-k]])
        after |= pa & ((prev & 31) == rn)
    isadd = (w & 0x7fc00000) == 0x11000000; isls = (w & 0x3b000000) == 0x39000000
    sel = after & (isadd | isls); m[sel] = m[sel] & np.uint32(~0x003ffc00 & 0xffffffff)
    if loose:
        isbc = (w & 0xff000010) == 0x54000000; m[isbc] = w[isbc] & 0xff00001f               # b.cond
        iscb = (w & 0x7e000000) == 0x34000000; m[iscb] = w[iscb] & 0xff00001f               # cbz / cbnz
        istb = (w & 0x7e000000) == 0x36000000; m[istb] = w[istb] & 0xfff8001f               # tbz / tbnz
    return m

class Bin:
    def __init__(self, path):
        self.path = path
        self.d, self.segs, self.secs, self.fs = machoref.load(path)
        self.ta, self.tz, self.to = machoref.text_sec(self.secs)
        self.w = np.frombuffer(self.d, dtype='<u4', count=self.tz // 4, offset=self.to)
        self._m = {}; self._fh = {}
        self.uuid = self.read_uuid()
    def read_uuid(self):
        d = self.d; ncmds = struct.unpack_from('<I', d, 16)[0]; off = 32
        for _ in range(ncmds):
            cmd, sz = struct.unpack_from('<II', d, off)
            if cmd == 0x1b: u = d[off + 8:off + 24].hex().upper(); return '%s-%s-%s-%s-%s' % (u[:8], u[8:12], u[12:16], u[16:20], u[20:])
            off += sz
        return None
    def m(self, loose=False):
        if loose not in self._m: self._m[loose] = masked(self.w, loose)
        return self._m[loose]
    def idx(self, vm): return (vm - self.ta) // 4
    def word(self, vm): return int(self.w[self.idx(vm)])
    def fwords(self, fs, fe):   # the function's raw words with PRE words before it (the masks look back for adrp)
        i, e = self.idx(fs), self.idx(fe); return self.w[max(0, i - PRE):e]
    def func(self, vm): return machoref.func_of(self.fs, vm)
    def fhash(self, loose):
        if loose in self._fh: return self._fh[loose]
        mm = self.m(loose); h = {}
        for i, a in enumerate(self.fs):
            e = self.fs[i + 1] if i + 1 < len(self.fs) else self.ta + self.tz
            if not (self.ta <= a < self.ta + self.tz) or e <= a: continue
            k = hashlib.blake2b(mm[self.idx(a):self.idx(e)].tobytes(), digest_size=12).digest()
            h.setdefault(k, []).append(a)
        self._fh[loose] = h; return h
    def section_of(self, vm):
        for seg, sn, a, z, o in self.secs:
            if a <= vm < a + z: return seg + ',' + sn
        return None
    def version(self):
        p = self.d.find(b'CFBundleShortVersionString')
        info = os.path.join(os.path.dirname(os.path.dirname(self.path)), 'Info.plist')
        try:
            t = open(info, 'rb').read().decode('utf8', 'replace'); m = re.search(r'CFBundleShortVersionString</key>\s*<string>([^<]+)', t)
            if m: return m.group(1)
        except OSError: pass
        return '?'

PRE = 8   # words before a function kept with it (the adrp look-back of the masks)

class SigRef:
    """The reference build as signatures (re-tools/machoport-sigs.json, made by `sigs`): the raw words of every function a
    point lies in or a data point's user lies in -- all `port` needs from the reference, so no reference binary is needed."""
    def __init__(self, path):
        j = json.load(open(path)); self.path = path; self.uuid = j['uuid']; self._ver = j['version']
        self.f = {int(k, 16): (int(v['end'], 16), np.array([int(x, 16) for x in (v['words'].split() if isinstance(v['words'], str) else v['words'])], dtype=np.uint32))
                  for k, v in j['funcs'].items()}
        self.starts = sorted(self.f)
    def version(self): return self._ver
    def func(self, vm):
        i = bisect.bisect_right(self.starts, vm) - 1
        if i < 0: return None, None
        fs = self.starts[i]; fe = self.f[fs][0]
        return (fs, fe) if vm < fe else (None, None)
    def fwords(self, fs, fe): return self.f[fs][1]
    def word(self, vm):
        fs, fe = self.func(vm)
        if fs is None:   # (a few words before a function: its PRE words)
            for s0 in self.starts:
                if s0 - 4 * PRE <= vm < s0: fs = s0; break
            else: raise IndexError('%#x is not in the signatures' % vm)
            w = self.f[fs][1]; pre = len(w) - (self.f[fs][0] - fs) // 4; return int(w[pre - (fs - vm) // 4])
        w = self.f[fs][1]; pre = len(w) - (fe - fs) // 4; return int(w[pre + (vm - fs) // 4])

def make_sigs(b, out):
    need = set()
    for f, kind, ref, arg, what in POINTS:
        if kind == 'code': need.add(b.func(ref))
        elif kind == 'data': need.update(b.func(u) for u in arg)
    funcs = {}
    for fs, fe in sorted(need):
        funcs['%#x' % fs] = {'end': '%#x' % fe, 'words': ' '.join('%08x' % int(x) for x in b.fwords(fs, fe))}
    json.dump({'about': 'nhook port signatures (re-tools/machoport.py sigs): the reference build\'s functions that hold a point or a data point\'s user, '
               'raw arm64 words with %d words before each; machoport.py port <this file> <new binary> needs nothing else' % PRE,
               'version': b.version(), 'uuid': b.uuid, 'funcs': funcs}, open(out + '.tmp', 'w'), indent=1)
    os.replace(out + '.tmp', out)
    return len(funcs)

def local_masked(old, vm, loose):
    """(masked words of vm's function, index of vm in them, function start/end) -- the PRE words masked along, then dropped"""
    fs, fe = old.func(vm)
    if fs is None: return None, None, None, None
    w = old.fwords(fs, fe); pre = len(w) - (fe - fs) // 4
    return masked(w, loose)[pre:], (vm - fs) // 4, fs, fe

def find_window(old, new, vm, loose):
    """vm (old) -> new address by a window of masked instructions around it that is unique in the new __text."""
    om, i, fs, fe = local_masked(old, vm, loose)
    if om is None: return None
    nm = new.m(loose); lo, hi = 0, len(om)
    cands = []
    for back in (0, 4, 8, 16, 32):
        for fwd in (8, 12, 16, 24, 32, 48):
            a, b = max(lo, i - back), min(hi, i + fwd)
            if b - a < 6: continue
            pat = om[a:b]; pos = np.nonzero(nm == pat[0])[0]
            for k in range(1, len(pat)):
                if not len(pos): break
                pos = pos[pos + k < len(nm)]; pos = pos[nm[pos + k] == pat[k]]
            if len(pos) == 1: cands.append((b - a, new.ta + (int(pos[0]) + (i - a)) * 4))
    if not cands: return None
    cands.sort(reverse=True); best = cands[0][1]
    return best if all(c[1] == best for c in cands[:3]) else None

def port_code(old, new, vm):
    for loose in (False, True):
        om, i, fs, fe = local_masked(old, vm, loose)
        if om is not None:
            k = hashlib.blake2b(om.tobytes(), digest_size=12).digest()
            hits = new.fhash(loose).get(k, [])
            if len(hits) == 1: return hits[0] + (vm - fs), 'same function' + (' (loose)' if loose else '')
        a = find_window(old, new, vm, loose)
        if a: return a, 'window' + (' (loose)' if loose else '')
    return None, 'not found'

def ref_target(b, vm):
    """the data address an instruction at vm reaches through an adrp before it (add / ldr / str / ldrb ..., unsigned offset)."""
    n = b.word(vm); rn = (n >> 5) & 31
    for j in range(1, 13):
        p = b.word(vm - 4 * j)
        if (p & 0x9f000000) == 0x90000000 and (p & 31) == rn:
            imm = ((p >> 29) & 3) | (((p >> 5) & 0x7FFFF) << 2)
            if imm & (1 << 20): imm -= 1 << 21
            page = ((vm - 4 * j) & ~0xFFF) + (imm << 12)
            if (n & 0x7fc00000) == 0x11000000: return page + ((n >> 10) & 0xFFF)
            if (n & 0x3b000000) == 0x39000000:
                sz = n >> 30; v = (n >> 26) & 1; opc = (n >> 22) & 3
                if v and opc >= 2: sz = 4
                return page + ((n >> 10) & 0xFFF) * (1 << sz)
            return None
    return None

def port_data(old, new, vm, users):
    votes = {}
    for u in users:
        t0 = ref_target(old, u)
        if t0 is None: continue
        nu, how = port_code(old, new, u)
        if not nu: continue
        t1 = ref_target(new, nu)
        if t1 is None: continue
        a = t1 + (vm - t0); votes.setdefault(a, []).append('%#x' % u)
    if not votes: return None, 'no user found'
    best = max(votes, key=lambda a: len(votes[a]))
    how = 'from %d of %d users' % (len(votes[best]), len(users)) + ('' if len(votes) == 1 else ' (OTHERS DISAGREE: %s)' % ', '.join('%#x' % a for a in votes if a != best))
    return best, how

def find_call(b, callee, off, before):
    if not callee: return None, 'no callee'
    bl = np.nonzero((b.w & 0xFC000000) == 0x94000000)[0]
    imm = (b.w[bl] & 0x3FFFFFF).astype(np.int64); imm = np.where(imm & (1 << 25), imm - (1 << 26), imm)
    sites = [b.ta + int(i) * 4 for i in bl[(b.ta + bl.astype(np.int64) * 4 + imm * 4) == callee]]
    pat = [(x, 0xffffffff) if isinstance(x, int) else x for x in before]
    good = [s for s in sites if all((b.word(s - 4 * (len(pat) - k)) & m) == v for k, (v, m) in enumerate(pat))]
    if len(good) == 1: return good[0] + off, 'the one of %d call(s) with those words before it' % len(sites)
    return None, '%d of %d calls have those words before them' % (len(good), len(sites))

def find_infunc(b, inside, mask, value, nxt, cond=None):
    if not inside: return None, 'no function'
    fs, fe = b.func(inside)
    nm, nv = (0xffffffff, nxt) if isinstance(nxt, int) else nxt if nxt else (0, 0)
    for x in range(fs, fe, 4):
        w = b.word(x)
        if (w & mask) == value and (b.word(x + 4) & nm) == nv and (cond is None or cond(w, b.word(x + 4))): return x, 'in the function at %#x' % fs
    return None, 'not in the function at %#x' % fs

# ---- meaning checks (run on the build's own addresses) ------------------------------------------------------------------
def bl_target(b, vm):
    w = b.word(vm)
    if (w & 0xfc000000) != 0x94000000: return None
    off = w & 0x3ffffff
    if off & (1 << 25): off -= 1 << 26
    return vm + off * 4

def reaches(b, f, lim, want):
    for k in range(0, lim, 4):
        try:
            if ref_target(b, f + k) == want: return True
        except IndexError: return False
    return False

def movz_movk(w, i, lo, hi):
    """w[i] = mov w?, #lo and w[i+1] = movk w?, #hi, lsl #16 (any register)"""
    return (int(w[i]) & 0xffffffe0) == (0x52800000 | lo << 5) and (int(w[i + 1]) & 0xffffffe0) == (0x72a00000 | hi << 5)

def frame_shape(b, f):
    """the per-frame function (nhost.c ht_frame_ok): sub sp, sp, #n ; stp x3 (the text hook's four displaced words), and in its first
    48 instructions halo3's frame flag (image+0x46bb83d) followed within 10 by the frame count (image+0x46bba40)"""
    w = [b.word(f + 4 * k) for k in range(48)]
    if (w[0] & 0xffc003ff) != 0xd10003ff or any((w[k] & 0xffc00000) != 0xa9000000 for k in (1, 2, 3)): return False
    return any(movz_movk(w, k, 0xb83d, 0x46b) and any(movz_movk(w, j, 0xba40, 0x46b) for j in range(k + 2, min(k + 10, 47))) for k in range(4, 47))

# ---- finders by MEANING (2026-11-10, 0.10.2: built with unwind tables, +8 MB; register allocation and prologues moved, so several
#      shapes did not carry over). Run when a point's shape finder fails or its result fails the meaning check.
def mean_codetab(b, res):
    """the sorted {guest, host} table's {ptr, count}: the binary search after mov x?, #0x180000000 (guest = host - image +
    0x180000000) loads it with adrp ; add ; ldp. 0.10.2 put a hash map (its own global) in front: that site has ldr, not ldp."""
    w = b.w; out = set()
    mz = np.nonzero(((w & 0xffffffe0) == 0xd2c00300) | ((w & 0xffffffe0) == 0xb26107e0))[0]
    for i in mz:
        for k in range(int(i) + 1, min(int(i) + 16, len(w) - 2)):
            p, a, l = int(w[k]), int(w[k + 1]), int(w[k + 2])
            if (p & 0x9f000000) == 0x90000000 and (a & 0xffc00000) == 0x91000000 and (a & 31) == (p & 31) == (a >> 5) & 31 \
               and (l & 0xffc00000) == 0xa9400000 and (l >> 5) & 31 == p & 31 and (l >> 15) & 0x7f == 0:
                t = ref_target(b, b.ta + (k + 1) * 4)
                if t: out.add(t)
                break
    return (out.pop(), 'by meaning: the binary search after mov #0x180000000') if len(out) == 1 else (None, 'by meaning: %d candidates' % len(out))

def mean_codehash(b, res=None):
    """the hash map of guest -> host code (0.10.2+): after mov x?, #0x180000000 the app hashes the guest address with the golden
    ratio (movk #0x9e37, lsl #48) and loads {ptr, cap, shift} with adrp ; add X ; ldr x?, [X, #8]. No golden-ratio site at all =
    the build has no hash map (0); every site that names its global must name the same one."""
    w = b.w; out = set(); golden = 0
    mz = np.nonzero(((w & 0xffffffe0) == 0xd2c00300) | ((w & 0xffffffe0) == 0xb26107e0))[0]
    for i in mz:
        i = int(i)
        if not np.any((w[i:i + 24] & 0xffffffe0) == (0xf2e00000 | 0x9e37 << 5)): continue
        golden += 1
        for k in range(i + 1, min(i + 16, len(w) - 5)):
            p, a = int(w[k]), int(w[k + 1])
            if (p & 0x9f000000) == 0x90000000 and (a & 0xffc00000) == 0x91000000 and (a & 31) == (p & 31) == (a >> 5) & 31:
                r = p & 31
                if any((int(w[k + 2 + j]) & 0xffffffe0) == (0xf9400000 | 1 << 10 | r << 5) for j in range(3)):
                    t = ref_target(b, b.ta + (k + 1) * 4)
                    if t: out.add(t)
                break
    if not golden: return 0, 'by meaning: no hash lookup in this build (none)'
    return (out.pop(), 'by meaning: the golden-ratio lookup after mov #0x180000000 (%d site(s))' % golden) if len(out) == 1 else (None, 'by meaning: %d candidates' % len(out))
MEAN = {'codehash': mean_codehash}

def mean_frame_fn(b, res):
    w = b.w; out = set()
    for i in np.nonzero((w & 0xffffffe0) == (0x52800000 | 0xb83d << 5))[0]:
        i = int(i)
        if i + 12 < len(w) and movz_movk(w, i, 0xb83d, 0x46b) and any(movz_movk(w, j, 0xba40, 0x46b) for j in range(i + 2, i + 10)):
            fs = b.func(b.ta + i * 4)[0]
            if fs and frame_shape(b, fs): out.add(fs)
    return (out.pop(), 'by meaning: halo3\'s frame flag + frame count') if len(out) == 1 else (None, 'by meaning: %d candidates' % len(out))

def mean_chat(b, res):
    """the chat_log gate(s): adrp x8 ; ldrb w8, [x8, #off] ; tbz|tbnz w8, #0 within 0x200 bytes before the first exact "[chat] line "
    reference: the nearest one's byte, and the (up to two) nearest tests of that byte (0.10.1 had two gates, 0.10.2 one: then
    both fields are that one; other flags of the chat state are tested a little earlier)"""
    t = b'[chat] line '; pos = b.d.find(t)
    vms = []
    while pos >= 0:
        for seg, sn, a, z, o in b.secs:
            if o <= pos < o + z and sn in ('__cstring', '__const'): vms.append(a + pos - o)
        pos = b.d.find(t, pos + 1)
    w = b.w; refs = []
    for i in np.nonzero((w & 0x9f000000) == 0x90000000)[0]:
        i = int(i)
        if i + 1 < len(w) and (int(w[i + 1]) & 0xffc00000) == 0x91000000:
            tg = ref_target(b, b.ta + (i + 1) * 4)
            if tg is not None and any(v - 1 <= tg <= v for v in vms): refs.append(b.ta + (i + 1) * 4)
    if not refs: return {}, 'by meaning: no "[chat] line " reference'
    r0 = min(refs); gates = []
    for x in range(r0 - 0x200, r0, 4):
        p, l, tb = b.word(x), b.word(x + 4), b.word(x + 8)
        if (p & 0x9f00001f) == 0x90000008 and (l & 0xffc003ff) == 0x39400108 and (tb & 0x7ef8001f) == 0x36000008: gates.append(x)
    if not gates: return {}, 'by meaning: no byte test before "[chat] line "'
    byte = ref_target(b, gates[-1] + 4); gates = [g for g in gates if ref_target(b, g + 4) == byte][-2:]   # (the nearest test's byte)
    return {'chat_gate_a': gates[0], 'chat_gate_b': gates[-1], 'chat_log': byte}, 'by meaning: %d gate(s) right before "[chat] line "' % len(gates)

def mean_muted(b, res):
    f = res.get('set_muted')
    if not f: return None, 'by meaning: no set_muted'
    for k in range(0, 64, 4):
        p, a = b.word(f + k), b.word(f + k + 4)
        if (p & 0x9f000000) == 0x90000000 and (a & 0xffc00000) == 0x91000000:
            t = ref_target(b, f + k + 4)
            if t: return t, 'by meaning: set_muted\'s first adrp + add'
    return None, 'by meaning: set_muted reaches no global'

def checks(b, a):
    """{field: (ok, why)} for one build's addresses a (dict)."""
    r = {}
    def ok(f, cond, why): r[f] = (bool(cond), why)
    g = lambda f: a.get(f) or 0
    ok('image', g('image') and b.section_of(g('image')) == '__DATA,__common' and g('image') % 0x1000 == 0, 'page-aligned, inside __DATA,__common')
    ok('codetab', g('codetab') and b.section_of(g('codetab')) == '__DATA,__data', 'in __DATA,__data')
    ok('opus_decode', g('opus_decode') and b.word(g('opus_decode')) == 0x6db923e9 and b.func(g('opus_decode'))[0] == g('opus_decode'),
       'a function start: stp d9, d8, [sp, #-0x70]!')
    mi = g('mixer_id'); er = (b.word(mi) >> 5) & 31 if mi else None   # the mixer's entry register (x26 to 0.10.1, x22 in 0.10.2)
    ok('mixer_id', mi and (b.word(mi) & 0xffc0001f) == 0xd1000008 and (b.word(mi + 4) & 0xffffffe0) == 0xf9400100 and 19 <= er <= 28
       and 19 <= b.word(mi + 4) & 31 <= 28 and b.func(mi)[0] == b.func(g('mixer_call'))[0],
       'sub x8, x<19..28>, #off ; ldr x<19..28>, [x8] in the mixer function (nhook.c voice_hook_install)')
    ok('mixer_id_off', mi and g('mixer_id_off') == (b.word(mi) >> 10) & 0xfff and 0x100 <= g('mixer_id_off') < 0x800, "the id load's offset (0x100..0x800)")
    mc = g('mixer_call')
    ok('mixer_call', mc and (b.word(mc) & 0xffe0ffff) == 0xaa0003e1 and (b.word(mc + 4) & 0xffe0ffff) == 0xaa0003e2 and b.word(mc + 8) == 0x52807803
       and bl_target(b, mc + 12) == g('opus_decode') and er is not None
       and any(b.word(x) == (0x385f9008 | er << 5) for x in range(b.func(mc)[0], b.func(mc)[1], 4)),
       'mov x1, x? ; mov x2, x? ; mov w3, #0x3c0 ; bl opus_decode; the function reads the channel byte (ldurb w8, [x<entry>, #-7])')
    ff = g('frame_fn'); ok('frame_fn', ff and b.func(ff)[0] == ff and frame_shape(b, ff),
                         'a function start: sub sp, sp, #n ; stp ; stp ; stp, then halo3\'s frame flag +0x46bb83d and count +0x46bba40 (nhost.c ht_frame_ok)')
    for f in ('chat_gate_a', 'chat_gate_b'):
        x = g(f); ok(f, x and (b.word(x) & 0x9f00001f) == 0x90000008 and (b.word(x + 4) & 0xffc003ff) == 0x39400108 and ref_target(b, x + 4) == g('chat_log'),
                     'adrp x8 ; ldrb w8, [x8, #chat_log]')
    ok('chat_log', g('chat_log') and b.section_of(g('chat_log')) in ('__DATA,__data', '__DATA,__bss', '__DATA,__common'), 'a byte in __DATA')
    ok('muted', g('muted') and b.section_of(g('muted')) in ('__DATA,__data', '__DATA,__bss', '__DATA,__common') and g('muted') % 8 == 0
       and (not g('set_muted') or reaches(b, g('set_muted'), 64, g('muted'))),
       'an aligned global in __DATA that set_muted reaches (2026-11-10: a data vote in 0.10.2 found another Mutex<Vec>)')
    mh = mean_codehash(b)[0]
    ok('codehash', mh is not None and g('codehash') == mh and (not mh or b.section_of(mh) in ('__DATA,__data', '__DATA,__bss', '__DATA,__common')),
       'the one global the golden-ratio code lookup loads {ptr, cap, shift} from, or 0 when the build has no such lookup')
    ok('set_muted', g('set_muted') and b.func(g('set_muted'))[0] == g('set_muted') and reaches(b, g('set_muted'), 64, g('muted')),
       'a function start reaching the mute list (adrp + add) in its first 64 bytes')
    return r

def logtext(b):
    return [(t, b.d.find(t.encode()) >= 0) for t in LOGTEXT]

# ---- the C table --------------------------------------------------------------------------------------------------------
HDR = """// Generated by re-tools/machoport.py (port <ref> <new> --apply): nhook's app addresses per native Reclaimer build, keyed by the
// app binary's LC_UUID (nhook.c app_build_pick). 0 = not found / failed its meaning check in that build: the feature stays off.
// Unslid vmaddrs. Do not edit by hand; to add a build: python3 re-tools/machoport.py port <0.10.0 binary> <new binary> --apply
"""
def read_runtime():
    rows = []
    if not os.path.exists(RUNTIME): return rows
    for l in open(RUNTIME):
        p = l.split()
        if len(p) < 2 or l.startswith('#'): continue
        rows.append((p[0], p[1], {k: int(v, 16) for k, v in (x.split('=') for x in p[2:])}))
    return rows

def read_table():
    rows = []
    if not os.path.exists(TABLE): return read_runtime()   # (a release bundle: no shim/native sources, the run-time rows only)
    for m in re.finditer(r'\{"([^"]+)", "([0-9A-F-]+)", \{([^}]*)\}\}', open(TABLE).read()):
        vals = {}
        for kv in m.group(3).split(','):
            kv = kv.strip()
            if kv: k, v = kv.split('='); vals[k.strip().lstrip('.')] = int(v.strip().rstrip('ull'), 16)
        rows.append((m.group(1), m.group(2), vals))
    return rows

def write_runtime(rows):
    os.makedirs(os.path.dirname(RUNTIME), exist_ok=True)
    with open(RUNTIME, 'w') as f:
        f.write('# nhook app builds (re-tools/machoport.py port ... --apply): <version> <uuid> <field>=<hex> ...\n')
        for ver, uuid, vals in rows: f.write('%s %s %s\n' % (ver, uuid, ' '.join('%s=%x' % (k, vals.get(k, 0)) for k in FIELDS)))

def write_table(rows):
    if '--no-runtime' not in sys.argv: write_runtime(rows)
    if not os.path.isdir(os.path.dirname(TABLE)): return   # (a release bundle: the dylib reads app-builds.txt)
    out = [HDR, 'static const AppBuild g_app_builds[] = {']
    for ver, uuid, vals in rows:
        body = ', '.join('.%s = 0x%xull' % (f, vals.get(f, 0)) for f in FIELDS)
        out.append('    {"%s", "%s", {%s}},' % (ver, uuid, body))
    out.append('};\n')
    open(TABLE, 'w').write('\n'.join(out))

def report(b, a, how=None):
    c = checks(b, a); bad = 0
    for f, _, ref, _, what in POINTS:
        okk, why = c[f]; bad += not okk
        print('  %-12s %-14s %s  %s%s' % (f, '%#x' % a[f] if a.get(f) else '-', 'ok  ' if okk else 'FAIL', what, '' if okk else '   [check: %s]' % why)
              + ('   (%s)' % how[f] if how else ''))
    lt = logtext(b); miss = [t for t, f in lt if not f]
    print('  log text: %d of %d found%s' % (len(lt) - len(miss), len(lt), '' if not miss else '; MISSING: ' + ', '.join(repr(t) for t in miss)))
    return bad, c

def main():
    a = sys.argv[1:]
    if len(a) < 2: print(__doc__); sys.exit(1)
    cmd = a[0]
    if cmd == 'sigs':
        b = Bin(a[1]); n = make_sigs(b, a[2] if len(a) > 2 else SIGS); print('%s (UUID %s): %d functions -> %s' % (b.version(), b.uuid, n, os.path.relpath(a[2] if len(a) > 2 else SIGS))); sys.exit(0)
    if cmd in ('port', 'selftest'):
        old = SigRef(a[1]) if a[1].endswith('.json') else Bin(a[1])
        new = Bin(a[2] if cmd == 'port' else (a[2] if len(a) > 2 else a[1]))
        print('reference %s (UUID %s) -> %s %s (UUID %s)' % (old.version(), old.uuid, 'new' if cmd == 'port' else 'itself', new.version(), new.uuid))
        res, how = {}, {}
        for f, kind, ref, arg, what in POINTS:
            if kind == 'code': res[f], how[f] = port_code(old, new, ref)
            elif kind == 'data': res[f], how[f] = port_data(old, new, ref, arg)
            elif kind == 'call': res[f], how[f] = find_call(new, res.get(arg[0]), arg[1], arg[2])
            elif kind == 'infunc': res[f], how[f] = find_infunc(new, res.get(arg[0]), *arg[1:])
            elif kind == 'mean': res[f], how[f] = MEAN[f](new, res)
            elif kind == 'imm': res[f], how[f] = (((new.word(res[arg[0]]) >> 10) & 0xfff), 'decoded') if res.get(arg[0]) else (None, 'no ' + arg[0])
            if res[f] is None: res[f] = 0
        c = checks(new, res)
        if not c['codetab'][0]: res['codetab'], how['codetab'] = (lambda r: (r[0] or 0, r[1]))(mean_codetab(new, res))
        if not c['frame_fn'][0]: res['frame_fn'], how['frame_fn'] = (lambda r: (r[0] or 0, r[1]))(mean_frame_fn(new, res))
        if not (c['chat_gate_a'][0] and c['chat_gate_b'][0] and c['chat_log'][0]):
            m, why = mean_chat(new, res)
            for f in ('chat_gate_a', 'chat_gate_b', 'chat_log'): res[f] = m.get(f, 0); how[f] = why
        if not c['muted'][0] or not c['set_muted'][0]: res['muted'], how['muted'] = (lambda r: (r[0] or 0, r[1]))(mean_muted(new, res))
        bad, c = report(new, res, how)
        for f in FIELDS:
            if not c[f][0]: res[f] = 0
        if cmd == 'selftest':
            exp = {f: r for f, _, r, _, _ in POINTS}; diff = [f for f in FIELDS if res[f] != exp[f]]
            print('selftest: %s' % ('PASS' if not diff else 'FAIL: ' + ', '.join('%s %#x (want %#x)' % (f, res[f], exp[f]) for f in diff)))
            sys.exit(1 if diff else 0)
        if '--apply' in a:
            rows = [r for r in read_table() if r[1] != new.uuid]; rows.append((new.version(), new.uuid, res)); rows.sort(key=lambda r: [int(x) if x.isdigit() else x for x in re.split(r'[.]', r[0])])
            write_table(rows); print('table: %s row for %s %s written (%s%s)' % ('a' if bad == 0 else 'a PARTIAL', new.version(), new.uuid,
                  os.path.relpath(TABLE) + ' + ' if os.path.isdir(os.path.dirname(TABLE)) else '', RUNTIME))
        sys.exit(1 if bad else 0)
    if cmd == 'check':
        b = Bin(a[1]); rows = [r for r in read_table() if r[1] == b.uuid]
        if not rows: print('%s (UUID %s): no row in %s -- run port' % (b.version(), b.uuid, os.path.relpath(TABLE))); sys.exit(2)
        print('%s (UUID %s): the table row' % (rows[0][0], b.uuid)); bad, _ = report(b, rows[0][2]); sys.exit(1 if bad else 0)
    print(__doc__); sys.exit(1)

if __name__ == '__main__':
    main()
