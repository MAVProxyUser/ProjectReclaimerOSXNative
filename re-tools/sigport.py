#!/usr/bin/env python3
"""Port one of the loader's signatures to a client build where it no longer matches (shim/docs/porting.md, step 3).

    python3 re-tools/sigport.py --point watchdog --old client/...v0.9.22.exe --new client/...v0.9.23.exe
    python3 re-tools/sigport.py --point voice_recv --old A.exe --new B.exe --corpus      # + check the result on every release
    python3 re-tools/sigport.py --selftest [--points a,b]                                # replay history (see below)
    python3 re-tools/sigport.py --rev abce1d1 --point watchdog --old ...0.9.22.exe --new ...0.9.23.exe   # as before the port

Points: ui-reentrancy, xaudio1..4, watchdog (patches: shim/src/peload.c g_sigpatches) and set_muted, voice_recv, push_history,
max_players, cast_vote, ballot (stats.c hooks / calls). The old site comes from the loader's own signatures (re-tools/
patchscan.py logic) in --old, or --old-site <rva>.

How (the technique, in order; shim/docs/porting.md section 9):
 1. THE OLD PLACE: the point's current signatures in --old, accepted only when patchscan's meaning check passes there.
 2. CANDIDATES in --new, by evidence: the point's own anchors (watchdog: the function loading "still running after";
    set_muted: functions whose `cl` lock some `dl` code shares, the is_muted pairing); the distinctive 4- / 8-byte
    constants and the texts of the old window and function, each weighted by rarity (1 / the number of functions holding
    it -- 0x3b9aca00 is also Rust's Option<Duration>::None niche, in hundreds); the old function's CALLERS' texts found in
    the new build, whose callees are candidates (a function without texts is known by who calls it: the mute UI, "Network
    receive"); for function points, functions starting with the same bytes.
 3. ALIGNMENT: instructions as shapes (mnemonic + operand kinds: register size, memory base class, distinctive
    constants), aligned with difflib. A patch / site: the old window + 8 instructions each side, scored on the matching
    stretch of the candidate only. A function: the whole body, 2 x matches / (old + new length), times
    0.5 + 0.5 x the similarity of the direct-caller counts. The best must score >= 0.6 and beat every other by 0.1.
 4. SYNTHESIS: around the mapped site, rel8/rel32 branch and call targets, RIP-relative displacements and 32-bit rbp/rsp
    frame displacements become ??; one instruction more until it matches exactly once (>= 12 bytes; the first byte
    literal; an entry hook's first 19 bytes literal and ending on an instruction boundary). A patch whose instruction
    changed encoding is re-derived (NOP the real length; je -> jmp for rel8 / rel32).
 5. CHECKS: patchscan's meaning check with only the new signature; --corpus: every release it matches and whether that
    agrees with the existing rows (CONFLICT = too loose).
Output: the C to paste (or --apply writes it into shim/src), the evidence and the scores.

--selftest replays history: for every release where a point's signature changed (or never matched), it ports the point
from the nearest release the loader supports to that one AS IF it were unknown and compares with the right place (the
loader's checked signature there, or shim/docs/patch-addresses.md section 3's anchor-found place). Exit 0 when nothing is
WRONG ("port by hand" is an acceptable answer, a wrong place never is). 2026-10-24: 160 ports, 84 right, 0 wrong.
"""
import sys, os, re, difflib, collections, json, argparse
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pelib import Image, cx86   # noqa: E402
import patchscan as PS          # noqa: E402

HOOK_PROLOGUE = 19   # stats.c entry_hook(..., 19, ...): those bytes are compared, copied to the trampoline and run there
MIN_SCORE = 0.6      # share of the old window (+ context) that must align with the candidate
MIN_MARGIN = 0.1     # the best candidate must beat every other by this much
FRAME = {cx86.X86_REG_RBP, cx86.X86_REG_RSP}


# ---- the points --------------------------------------------------------------------------------------------------------
REV = None   # --rev: the signatures of that commit


def catalogue():
    sp, st = PS.load_sources(REV)
    pts = collections.OrderedDict()
    for v in sp:
        pts.setdefault(v['slot'], dict(kind='patch', variants=[]))['variants'].append(v)
    for name, kind in (('set_muted', 'call'), ('voice_recv', 'hook'), ('push_history', 'hook'), ('max_players', 'call'),
                       ('cast_vote', 'call'), ('ballot', 'site')):
        pts[name] = dict(kind=kind, sigs=st[name])   # stats.c g_sig_<name>
    return pts, sp, st


_SCANS = {}


def scan_of(im):
    """patchscan's verdicts for im (the loader's own checks: original bytes, is_muted pairing, clone shape, Vote message ...)"""
    if im.path not in _SCANS:
        sp, st = PS.load_sources(REV); _SCANS[im.path] = PS.scan(im, sp, st)
    return _SCANS[im.path]


def locate(pt, im, name=None):
    """(window start, signature, patch offsets, orig, repl) of the point in im via the existing signatures, or None. With
    name: only when patchscan's meaning check passes there too (a unique signature hit can be the wrong function: set_muted
    in 0.8.16 .. 0.9.1 hits a Forge setter)"""
    loc = locate_raw(pt, im)
    if loc is None or name is None: return loc
    r = scan_of(im).get('cast_vote' if name == 'ballot' else name)
    if r is None or r['status'] != 'ok': return None
    if name != 'ballot' and r.get('rva') != loc[0] + loc[2][0]: return None
    return loc


def locate_raw(pt, im):
    if pt['kind'] == 'patch':
        for v in pt['variants']:
            h, n = im.find_unique(v['sig'])
            if h is None: continue
            if all(im.b[h + o:h + o + len(PS.sig_bytes(ob))] == PS.sig_bytes(ob) for o, ob in zip(v['offs'], v['orig'])):
                return h, v['sig'], v['offs'], v['orig'], v['repl']
        return None
    for s in pt['sigs']:
        h, n = im.find_unique(s)
        if h is not None: return h, s, [0], [], []
    return None


# ---- instruction shapes ------------------------------------------------------------------------------------------------
def distinctive(v):
    v &= 0xffffffffffffffff
    return v >= 0x10000 and v not in (0xffffffff, 0xffffffffffffffff, 0x80000000, 0x7fffffff) and (v & 0xffff0000) != 0xffff0000


def shape(ins):
    ops = []
    for op in ins.operands:
        if op.type == cx86.X86_OP_REG: ops.append('r%d' % op.size)
        elif op.type == cx86.X86_OP_IMM:
            if 'branch_relative' in [ins.group_name(g) for g in ins.groups]: ops.append('rel')
            else: ops.append('i%x' % (op.imm & 0xffffffffffffffff) if distinctive(op.imm) else 'i')
        elif op.type == cx86.X86_OP_MEM:
            b = op.mem.base
            if b == cx86.X86_REG_RIP: ops.append('m.rip')
            elif b in FRAME: ops.append('m.frame')
            else: ops.append('m.%x' % (op.mem.disp & 0xffffffff) if distinctive(op.mem.disp) else 'm')
    return ins.mnemonic + ' ' + ','.join(ops)


def anchors_of(im, insns):
    """distinctive constants ((value, width) of 4- / 8-byte immediates and non-frame displacements) and the text literals
    these instructions load"""
    consts, texts = set(), set()
    for ins in insns:
        rel = 'branch_relative' in [ins.group_name(g) for g in ins.groups]
        for op in ins.operands:
            if op.type == cx86.X86_OP_IMM and not rel and distinctive(op.imm) and ins.imm_size in (4, 8):
                consts.add((op.imm & ((1 << (8 * ins.imm_size)) - 1), ins.imm_size))
            elif op.type == cx86.X86_OP_MEM:
                if op.mem.base == cx86.X86_REG_RIP:
                    t = ins.address + ins.size + op.mem.disp
                    if not im.is_exec(t) and 0 <= t < im.size:
                        m = re.match(rb'[\x20-\x7e]{8,}', im.b[t:t + 120])
                        if m: texts.add(m.group(0)[:48])
                elif op.mem.base not in FRAME and distinctive(op.mem.disp) and ins.disp_size == 4: consts.add((op.mem.disp & 0xffffffff, 4))
    return consts, texts


def const_sites(im, c):
    v, w = c; pat = v.to_bytes(w, 'little'); out = []
    for va, vs in im.exec:
        i = im.b.find(pat, va, va + vs)
        while i >= 0 and len(out) < 400:
            out.append(i); i = im.b.find(pat, i + 1, va + vs)
    return out


def weigh(new, groups, weight=1.0):
    """each group (the code sites of one anchor) gives `weight` split over the functions holding it: a constant or text in
    hundreds of functions (0x3b9aca00 is also Rust's Option<Duration>::None niche) counts for little, one in a single
    function for a lot"""
    out = collections.Counter()
    for sites in groups:
        fs = {new.func_of(x) for x in sites} - {None}
        for f in fs: out[f] += weight / len(fs)
    return out


def caller_candidates(old, new, f_old):
    """the old function's counterparts found through its callers: every caller's texts are looked up in the new build,
    the functions loading most of them are the new callers, and their direct callees are the candidates. A function
    with no texts of its own is still known by who calls it (set_muted: the mute UI; voice_recv: "Network receive")"""
    out = collections.Counter(); ctexts = []
    for site in old.callers_of(f_old[0])[:24]:
        g = old.func_of(site)
        if g and g[1] - g[0] < 0x40000:
            t = anchors_of(old, old.insns(g[0], g[1]))[1]
            if t: ctexts.append(sorted(t, key=len, reverse=True)[:12])
    refs = new.text_refs_many([t for ts in ctexts for t in ts])
    for ts in ctexts:
        nc = weigh(new, [refs[t] for t in ts])
        for f, w in nc.most_common(3):
            if w < 0.5: continue   # (a new function sharing only some of that caller's texts by chance)
            for c in set(new.callees(f)):
                fc = new.func_of(c)
                if fc and fc[0] == c: out[fc] += w / len(ts)
    return out


# ---- per-point anchors (knowledge that survives a rebuild) -----------------------------------------------------------------
def special_candidates(name, im):
    out = collections.Counter()
    if name == 'watchdog':
        for s in im.text_refs(b'still running after'):
            f = im.func_of(s)
            if f: out[f] += 5
    if name == 'set_muted':   # functions taking (with cl, near their entry) a lock that a function takes with dl at its entry (is_muted)
        cl = collections.defaultdict(list)
        for va, vs in im.exec:
            i = im.b.find(b'\xf0\x0f\xb0\x0d', va, va + vs)
            while i >= 0:
                cl[i + 8 + im.i32(i + 4)].append(i); i = im.b.find(b'\xf0\x0f\xb0\x0d', i + 1, va + vs)
        i = 0x1000
        while True:
            i = im.b.find(PS.DL_LOCK, i)
            if i < 0 or i + 12 > im.size: break
            f = im.func_of(i)
            if f and i - f[0] <= 8:
                for site in cl.get(i + 12 + im.i32(i + 8), []):
                    g = im.func_of(site)
                    if g and g != f and site - g[0] < 0x40: out[g] += 1
            i += 1
    return out


# ---- the port ----------------------------------------------------------------------------------------------------------
MAX_FUNC_INSNS = 1500   # whole-function alignment cap (voice_recv is ~900 instructions)


def window_insns(im, start, length):
    f = im.func_of(start); a = f[0] if f else start
    ins = im.insns(a, (f[1] if f else start + 0x400))
    idx = [i for i, x in enumerate(ins) if start <= x.address < start + length]
    return ins, idx


def port(name, pt, old, new, old_site=None, verbose=False):
    loc = locate(pt, old, name) if old_site is None else (old_site, None, [0], [], [])
    if loc is None: return dict(status='old: not found', name=name)
    h, sig, offs, orig, repl = loc
    length = len(sig.split()) if sig else 32
    oins, oidx = window_insns(old, h, length)
    if not oidx: return dict(status='old: no instructions', name=name)
    f_old = old.func_of(h)
    whole = pt['kind'] in ('hook', 'call')   # a function: compared as a whole; a patch / site: its window inside one
    o_site_addr = h + offs[0]
    o_site = next(i for i in oidx if oins[i].address <= o_site_addr < oins[i].address + oins[i].size)
    if whole: otok = [shape(x) for x in oins[:MAX_FUNC_INSNS]]; o_rel = 0
    else:
        lo, hi = max(0, oidx[0] - 8), min(len(oins), oidx[-1] + 9)   # the window + 8 instructions of context each side
        otok = [shape(x) for x in oins[lo:hi]]; o_rel = o_site - lo
    w_consts, w_texts = anchors_of(old, oins if whole else oins[lo:hi]); f_consts, f_texts = anchors_of(old, oins)
    # candidates, by evidence: the point's own anchors (special), the window's constants and texts (x2), the function's
    # (x1), and the counterparts of the old function's callers' callees (x2)
    special = special_candidates(name, new)
    refs = new.text_refs_many(list(w_texts | f_texts))
    cand = collections.Counter(special)
    cand.update(weigh(new, [const_sites(new, c) for c in w_consts] + [refs[t] for t in w_texts], 2.0))
    cand.update(weigh(new, [const_sites(new, c) for c in f_consts - w_consts] + [refs[t] for t in f_texts - w_texts], 1.0))
    if f_old:
        for f, w in caller_candidates(old, new, f_old).items(): cand[f] += 2.0 * w
    if whole and f_old:   # functions starting with the same bytes (a prologue that never changed: push_history's 35)
        twins = []
        for L in (16, 24, 32, 40, 48):   # the shortest prefix that leaves at most 100 functions
            pro = old.b[f_old[0]:f_old[0] + L]; tw = [f for f in new.funcs if new.b[f[0]:f[0] + L] == pro]
            if not tw: break
            twins = tw
            if len(tw) <= 100: break
        if 0 < len(twins) <= 100:
            for f in twins: cand[f] += 1.0 / len(twins)
            special = collections.Counter(special); special.update({f: 0 for f in twins})   # (aligned, like the specials)
    if not cand: return dict(status='no anchor in the new build', name=name)
    best = None; scores = {}; n_old_callers = len(old.callers_of(f_old[0])) if f_old else 0
    todo = dict(cand.most_common(40))
    if len(special) <= 200: todo.update({f: cand[f] for f in special})   # every per-point candidate is aligned
    for f, w in sorted(todo.items(), key=lambda x: -x[1]):
        if f[1] - f[0] > 0x40000: continue
        nins = new.insns(f[0], f[1]); ntok = [shape(x) for x in nins[:MAX_FUNC_INSNS]]
        if whole:
            sm = difflib.SequenceMatcher(None, otok, ntok, autojunk=False)
            ni, score = 0, 2.0 * sum(m.size for m in sm.get_matching_blocks()) / (len(otok) + len(ntok))
            # a function's number of direct callers rarely changes between builds; a lookalike sibling's usually differs
            # (old push_history: 4-5 callers; its 1-caller sibling, same prologue and deque cap, aligned better)
            nc = len(new.callers_of(f[0])); cf = (min(n_old_callers, nc) + 1) / (max(n_old_callers, nc) + 1)
            score *= 0.5 + 0.5 * cf
        else: ni, score = align(otok, o_rel, ntok)
        if ni is None: continue
        scores[f] = score
        if best is None or (score, w) > (best[0], best[1]): best = (score, w, f, nins, ni)
    if best is None: return dict(status='no aligned candidate', name=name, candidates=len(cand))
    score, w, f, nins, ni = best
    others = sorted(((v, k) for k, v in scores.items() if k != f), reverse=True)
    runner, rfunc = others[0] if others else (0.0, None)
    # confidence: most of the old code must be there, and clearly better than in any other candidate (Rust monomorphises
    # generics: identical copies of a function make two equal candidates -- then there is no answer)
    if score < MIN_SCORE: return dict(status=f'best alignment {score:.2f} < {MIN_SCORE} (the code changed too much: port by hand)', name=name, score=round(score, 3))
    if runner > score - MIN_MARGIN: return dict(status=f'two candidates align alike ({score:.2f} at {f[0]:#x} / {runner:.2f} at {rfunc[0]:#x}): port by hand', name=name, score=round(score, 3))
    site_ins = nins[ni]
    if site_ins.mnemonic != oins[o_site].mnemonic and pt['kind'] == 'patch':
        return dict(status=f'mapped to {site_ins.mnemonic}, expected {oins[o_site].mnemonic}', name=name, score=score)
    res = synthesize(name, pt, new, f, nins, ni, oins, o_site, h, offs, orig, repl)
    res.update(score=round(score, 3), runner_up=round(runner, 3), runner_func=hex(rfunc[0]) if rfunc else None, evidence=round(w, 2), func=hex(f[0]))
    return res


PAD = 8   # instructions of slack on each side of the old window when the alignment is scored locally


def align(otok, o_rel, ntok):
    """(index in ntok of the old window's patched instruction, local score) or (None, 0). The whole function is aligned
    first to find where the window went; the score is then taken again on just that stretch of the new function (window
    + PAD each side): in a long function difflib otherwise collects scattered matches from all over it"""
    sm = difflib.SequenceMatcher(None, otok, ntok, autojunk=False); ni = None
    for m in sm.get_matching_blocks():
        if m.a <= o_rel < m.a + m.size: ni = m.b + (o_rel - m.a)
    if ni is None: return None, 0.0
    lo = max(0, ni - o_rel - PAD); hi = min(len(ntok), ni + (len(otok) - o_rel) + PAD)
    sm = difflib.SequenceMatcher(None, otok, ntok[lo:hi], autojunk=False); n2 = None; matched = 0
    for m in sm.get_matching_blocks():
        matched += m.size
        if m.a <= o_rel < m.a + m.size: n2 = lo + m.b + (o_rel - m.a)
    if n2 is None: return None, 0.0
    return n2, matched / len(otok)


def ins_pattern(ins, wild_frame=True):
    b = list('%02X' % x for x in ins.bytes)
    if 'branch_relative' in [ins.group_name(g) for g in ins.groups] and ins.imm_size:
        for k in range(ins.imm_offset, ins.imm_offset + ins.imm_size): b[k] = '??'
    for op in ins.operands:
        if op.type == cx86.X86_OP_MEM and ins.disp_size:
            if op.mem.base == cx86.X86_REG_RIP or (wild_frame and op.mem.base in FRAME and ins.disp_size == 4):
                for k in range(ins.disp_offset, ins.disp_offset + ins.disp_size): b[k] = '??'
    return b


def synthesize(name, pt, im, f, nins, ni, oins, o_site, h, offs, orig, repl):
    site = nins[ni]
    if pt['kind'] == 'patch':
        pre = sum(1 for x in oins if h <= x.address < oins[o_site].address)
        start = max(0, ni - pre)
    else:
        start = ni
    for end in range(ni + 1, min(len(nins), ni + 40)):
        toks = []
        for x in nins[start:end + 1]: toks += ins_pattern(x)
        if pt['kind'] == 'hook' and '??' in toks[:HOOK_PROLOGUE]:
            return dict(status='the hook prologue (first 19 bytes) has variable bytes: needs a hand-written entry hook', name=name)
        if pt['kind'] == 'hook':
            ends, a = set(), 0
            for x in nins[start:end + 1]: a += x.size; ends.add(a)
            if a >= HOOK_PROLOGUE and HOOK_PROLOGUE not in ends:
                return dict(status='no instruction ends at byte 19: the entry hook needs another prologue length', name=name)
        if toks[0] == '??' or len(toks) > 96: break
        if len(toks) < (HOOK_PROLOGUE if pt['kind'] == 'hook' else 12): continue
        s = ' '.join(toks)
        hit, n = im.find_unique(s)
        if hit is None: continue
        out = dict(status='ok', name=name, site=hex(site.address), sig=s, start=hex(hit))
        if pt['kind'] == 'patch':
            off = site.address - hit
            o_b = bytes(PS.sig_bytes(orig[0])); s_b = site.bytes
            if s_b[:len(o_b)] == o_b:
                out.update(offs=[off], orig=orig[:1], repl=repl[:1])
            else:   # the branch has another encoding now: re-derive the same edit
                if all(r == '90' for r in repl[0].split()):
                    out.update(offs=[off], orig=[' '.join('%02X' % x for x in s_b[:2 if s_b[0] == 0x0F else 1])], repl=[' '.join(['90'] * site.size)])
                elif repl[0] == 'EB' and s_b[0] in range(0x70, 0x80):
                    out.update(offs=[off], orig=['%02X' % s_b[0]], repl=['EB'])
                elif repl[0] == 'EB' and s_b[0] == 0x0F:
                    out.update(offs=[off], orig=['0F %02X' % s_b[1]], repl=['90 E9'])
                else:
                    return dict(status=f'patch {repl[0]} has no rule for {s_b[:2].hex()}', name=name)
            out['c'] = '  {"%s","%s",%d,"%s",\n     1,{0x%x},{"%s"},{"%s"}},' % (pt['variants'][0]['name'], name, pt['variants'][0].get('dflt', 0), s, out['offs'][0], out['orig'][0], out['repl'][0])
        else:
            out['c'] = '"%s"' % s
        return out
    return dict(status='no unique signature within 40 instructions', name=name, site=hex(site.address))


# ---- verification ------------------------------------------------------------------------------------------------------
def meaning_check(name, res, im, sp, st):
    """run patchscan's own check with the new signature in place of the old ones"""
    if res.get('status') != 'ok': return None
    sp2 = list(sp); st2 = {k: list(v) for k, v in st.items()}
    if 'offs' in res:
        v0 = next(v for v in sp if v['slot'] == name)
        sp2 = [dict(name=v0['name'], slot=name, sig=res['sig'], offs=res['offs'], orig=res['orig'], repl=res['repl'])] + [v for v in sp if v['slot'] != name]
    else: st2[name] = [res['sig']]   # only the new signature: the check must pass with it alone
    r = PS.scan(im, sp2, st2)
    key = 'cast_vote' if name == 'ballot' else name
    return r[key]['status']


def corpus_check(name, res, pts):
    """where else the new signature matches, and whether that agrees with the existing signatures"""
    rows = []
    for f in PS.corpus():
        im = Image(f); tag = re.search(r'/(v[\d.]+)/', f).group(1)
        hit, n = im.find_unique(res['sig'])
        if hit is None and n == 0: continue
        site = hit + (res['offs'][0] if 'offs' in res else 0) if hit is not None else None
        known = locate(pts[name], im); ksite = (known[0] + known[2][0]) if known else None
        rows.append((tag, 'ambiguous(%d)' % n if hit is None else hex(site), 'new' if ksite is None else ('agrees' if ksite == site else 'CONFLICT %x' % ksite)))
    return rows


# ---- self-test: history replay -----------------------------------------------------------------------------------------
DOC_POINT = {'xaudio slot 1 jne': 'xaudio1', 'xaudio slot 2 jne': 'xaudio2', 'xaudio slot 3 jne': 'xaudio3', 'xaudio slot 4 jne': 'xaudio4',
             'watchdog je (sig+0xa)': 'watchdog', 'set_muted (entry)': 'set_muted', 'voice_recv (entry)': 'voice_recv',
             'push_history (entry)': 'push_history', 'ui-reentrancy jne (sig+8)': 'ui-reentrancy'}


def doc_truth():
    """the anchor-found places of shim/docs/patch-addresses.md section 3 (client exe tables): {(tag, point): rva}"""
    doc = open(os.path.join(PS.ROOT, 'shim/docs/patch-addresses.md')).read()
    sec = doc[doc.index('## 3. Where each point is'):doc.index('## 4.')]
    truth = {}
    for tbl in re.split(r'\n### ', sec):
        if not tbl.startswith('Client exe, v0.8') and not tbl.startswith('Client exe, v0.9.7'): continue
        lines = [l for l in tbl.splitlines() if l.startswith('|')]
        tags = [c.strip() for c in lines[0].split('|')[2:-1]]
        for l in lines[2:]:
            c = [x.strip() for x in l.split('|')[1:-1]]
            p = DOC_POINT.get(c[0])
            if not p: continue
            for tag, cell in zip(tags, c[1:]):
                m = re.search(r'real ([0-9a-f]+)\)', cell) or re.fullmatch(r'\(?([0-9a-f]+)\)?(?: \?| =fixed)?', cell)
                if m: truth[(tag, p)] = int(m.group(1), 16)
    return truth


def selftest(only=None):
    pts, sp, st = catalogue(); files = PS.corpus(); truth = doc_truth()
    tags = [re.search(r'/(v[\d.]+)/', f).group(1) for f in files]
    ims = {}
    def img(i):
        if i not in ims: ims[i] = Image(files[i])
        return ims[i]
    rows = []; stats = collections.Counter()
    for name, pt in pts.items():
        if only and name not in only: continue
        found = [locate(pt, img(i), name) for i in range(len(tags))]   # where the loader's own (checked) signatures work
        # the right place: the documented anchor-found one (patch-addresses.md section 3), else the checked signature's
        known = [truth.get((t, name), (found[i][0] + found[i][2][0]) if found[i] else None) for i, t in enumerate(tags)]
        for i in range(len(tags)):
            if known[i] is None: continue
            j = next((k for k in range(i - 1, -1, -1) if found[k]), None)   # the nearest older release the loader supports
            if j is None:   # nothing older: port backwards from the nearest newer one
                j = next((k for k in range(i + 1, len(tags)) if found[k]), None)
            if j is None: continue
            if found[i] and found[i][1] == found[j][1]: continue   # the same signature works in both: nothing to port
            r = port(name, pt, img(j), img(i))
            got = int(r['site'], 16) if r.get('status') == 'ok' else None
            if pt['kind'] in ('hook', 'call') and got is not None: got = int(r['func'], 16)
            if got is not None:
                mc = meaning_check(name, r, img(i), sp, st)
                if mc != 'ok': r = dict(status=f'meaning check: {mc}', score=r.get('score')); got = None
            if got is None: verdict = 'no answer'
            elif got == known[i]: verdict = 'RIGHT'
            else: verdict = 'WRONG'
            stats[verdict] += 1
            rows.append((name, tags[j], tags[i], hex(known[i]), '-' if got is None else hex(got), r.get('score', '-'),
                         verdict if got is not None else verdict + ': ' + r['status']))
    print('| point | from | to | right place | sigport | score | verdict |\n|---|---|---|---|---|---|---|')
    for row in rows: print('| ' + ' | '.join(str(x) for x in row) + ' |')
    print(f'\n{stats["RIGHT"]} right, {stats["WRONG"]} wrong, {stats["no answer"]} without a confident answer')
    return 0 if stats['WRONG'] == 0 else 1


def apply(name, pt, r, old_path, new_path):
    """write r's signature into the sources: a g_sigpatches row after the slot's last variant (peload.c) or a line at the
    end of the point's g_sig_<point> list (stats.c), tagged with the build and the evidence"""
    vo, vn = PS.version_of(old_path), PS.version_of(new_path)
    note = f'v{vn}: re-tools/sigport.py from v{vo} (score {r["score"]:.2f}, runner-up {r["runner_up"]:.2f})'
    if pt['kind'] == 'patch':
        path = os.path.join(PS.ROOT, 'shim/src/peload.c'); src = open(path).read()
        t0 = src.index('static const SigPatch g_sigpatches[]={'); t1 = src.index('\n};', t0)
        rows = list(re.finditer(r'\{"[^"]+","([^"]+)",\d,"[0-9A-F? ]+",\s*\d,\{[^}]*\},\{[^}]*\},\{[^}]*\}\},', src[t0:t1]))
        last = [m for m in rows if m.group(1) == name][-1]
        at = t0 + last.end(); src = src[:at] + '\n  // ' + note + '\n' + r['c'] + src[at:]
    else:
        path = os.path.join(PS.ROOT, 'shim/src/stats.c'); src = open(path).read()
        m = re.search(r'static const char\*const g_sig_' + re.escape(name) + r'\[\]=\{.*?(\n\s*0 \};)', src, re.S)
        at = m.start(1); src = src[:at] + f'\n    "{r["sig"]}",   // {note}' + src[at:]
    open(path, 'w').write(src)
    return os.path.relpath(path, PS.ROOT)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--point'); ap.add_argument('--old'); ap.add_argument('--new'); ap.add_argument('--old-site', type=lambda x: int(x, 16))
    ap.add_argument('--corpus', action='store_true'); ap.add_argument('--selftest', action='store_true'); ap.add_argument('--points')
    ap.add_argument('--json', action='store_true'); ap.add_argument('--rev', help='use the signatures of this git commit')
    ap.add_argument('--apply', action='store_true', help='write the new signature into shim/src (only when every check passed)')
    a = ap.parse_args()
    global REV; REV = a.rev
    if a.selftest: sys.exit(selftest(a.points.split(',') if a.points else None))
    if not (a.point and a.old and a.new): ap.error('--point, --old and --new (or --selftest)')
    pts, sp, st = catalogue()
    if a.point not in pts: ap.error('points: ' + ', '.join(pts))
    old, new = Image(a.old), Image(a.new)
    r = port(a.point, pts[a.point], old, new, a.old_site)
    if r.get('status') == 'ok': r['meaning_check'] = meaning_check(a.point, r, new, sp, st)
    if a.corpus and r.get('status') == 'ok': r['corpus'] = corpus_check(a.point, r, pts)
    if a.json: print(json.dumps(r, indent=1)); return
    for k, v in r.items():
        if k == 'corpus': print('corpus:'); [print('   ', *row) for row in v]
        elif k != 'c': print(f'{k}: {v}')
    ok = r.get('status') == 'ok' and r.get('meaning_check') == 'ok'
    if 'c' in r and not a.apply:
        print('\npaste into ' + ('shim/src/peload.c g_sigpatches (after the last ' + a.point + ' row):' if 'offs' in r
                                else f'shim/src/stats.c g_sig_{a.point} (before its 0):') + '\n' + r['c'] + '\n(or rerun with --apply)')
    if a.apply:
        if not ok: print('\n--apply: not written (the port did not pass every check)'); sys.exit(1)
        if a.rev: print('\n--apply: not with --rev (the sources are today\'s)'); sys.exit(1)
        if 'offs' in r and old.find_unique(r['sig'])[0] is not None:   # the loader applies every matching patch variant
            print(f'\nnote: the new signature also matches {a.old}: the loader would apply two variants there (harmless: the'
                  ' second finds the patched bytes and logs NOT applied) -- a longer signature avoids it')
        f = apply(a.point, pts[a.point], r, a.old, a.new)
        print(f'\nwritten to {f}. Next: bash shim/build.sh && python3 re-tools/patchscan.py --diff {a.old} {a.new} '
              '&& bash shim/tools/port-verify.sh ' + a.new)
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
