#!/usr/bin/env python3
"""Totals of a PELOAD_SAMPLE_DUMP file (the loader's sampler, PELOAD_SAMPLE=<us>: each thread's real x86 instruction pointer,
guest code mapped to its function by .pdata): per thread, the share of its busy samples in each function over the whole run.
With PELOAD_SAMPLE_STACK the dump also holds call stacks ("S" lines) and this prints inclusive times (the function or anything
it calls), self times, and a top-down tree.
    python3 shim/tools/sample-dump.py <dump file> [--thread T] [--top N] [--tree [--depth D] [--min PCT]] [--focus FUNC]
        --thread  substring of the thread name (default guest-0x18000a120 = halo3's engine thread)
        --tree    the call tree from the outermost frame down (children >= --min % of all samples, default 0.5)
        --focus   one function (e.g. halo3+0x16a4c4): its callers and callees, inclusive
        --match   only the 10 s intervals at least SECS (default 20) into gameplay (the dump's "T" lines; a dump also holds the
                  menu and loading screens otherwise)
        --dll     halo3.dll: a sample in a leaf function (no .pdata entry: the sampler keys it by its raw address) is named by the
                  leaf's start instead (halo3+0x9f5a4 rather than +0x9f5b2); guessed from the int3 padding before it"""
import sys, collections, argparse, struct, bisect
ap = argparse.ArgumentParser(); ap.add_argument('dump'); ap.add_argument('--thread', default='guest-0x18000a120')
ap.add_argument('--top', type=int, default=40); ap.add_argument('--tree', action='store_true'); ap.add_argument('--depth', type=int, default=14)
ap.add_argument('--min', type=float, default=0.5); ap.add_argument('--focus'); ap.add_argument('--dll')
ap.add_argument('--match', type=float, nargs='?', const=20.0, default=None)
a = ap.parse_args(); th = a.thread
leaf = lambda k: k
if a.dll:
    d = open(a.dll, 'rb').read(); pe = struct.unpack_from('<I', d, 0x3c)[0]; ns = struct.unpack_from('<H', d, pe + 6)[0]; so = struct.unpack_from('<H', d, pe + 20)[0]
    secs = [struct.unpack_from('<IIII', d, pe + 24 + so + i * 40 + 8) for i in range(ns)]
    def fo(r):
        for vs, va, rs, ra in secs:
            if va <= r < va + max(vs, rs): return ra + r - va
    prva, psz = struct.unpack_from('<II', d, pe + 24 + 112 + 3 * 8)
    fns = sorted(struct.unpack_from('<III', d, fo(prva) + i * 12) for i in range(psz // 12)); begins = [f[0] for f in fns]; ends = sorted(f[1] for f in fns)
    cache = {}
    def leaf(k):
        if not k.startswith('halo3+0x'): return k
        r = int(k[8:], 16); i = bisect.bisect_right(begins, r) - 1
        if i >= 0 and fns[i][0] <= r < fns[i][1]: return k                 # inside a .pdata function: the sampler named it already
        if r in cache: return cache[r]
        e = ends[bisect.bisect_right(ends, r) - 1] if bisect.bisect_right(ends, r) else 0
        st = e; o = fo(r)
        if o is None: return k
        if o >= len(d): return k                                            # not code (past the section's file data)
        for q in range(r, max(e, r - 4096), -1):
            if d[o - (r - q) - 1] == 0xCC: st = q; break
        while st < r and d[o - (r - st)] == 0xCC: st += 1
        cache[r] = f'halo3+{st:#x}'; return cache[r]
cnt = collections.Counter(); stacks = collections.Counter(); lost = 0; keep = a.match is None; nint = 0
for line in open(a.dump, errors='replace'):
    p = line.rstrip('\n').split('\t')
    if p[0] == 'T' and len(p) == 3:
        keep = a.match is None or float(p[2]) >= a.match; nint += keep; continue
    if not keep: continue
    if p[0] == 'S' and len(p) == 4:
        if th in p[1]: stacks[tuple(leaf(f) for f in p[3].split(';'))] += int(p[2])
        continue
    if p[0] == 'S-lost': lost += int(p[1]); continue
    if len(p) != 4 or th not in p[0]: continue
    cnt[leaf(p[1])] += int(p[2])
tot = sum(cnt.values()) or 1
mods = collections.Counter()
for k, v in cnt.items(): mods[k.split('+')[0].split(':')[0]] += v
print(f'{th}: {tot} busy samples' + (f' ({nint} intervals of 10 s, {a.match:.0f}+ s into gameplay)' if a.match is not None else ''))
print('  by module: ' + ', '.join(f'{m} {100 * v / tot:.1f}%' for m, v in mods.most_common(6)))
acc = 0
print('  self time (the sample was in the function itself):')
for k, v in cnt.most_common(a.top):
    acc += v; print(f'  {100 * v / tot:5.2f}%  (cum {100 * acc / tot:5.1f}%)  {k}')
if not stacks: sys.exit(0)
stot = sum(stacks.values())
roots = collections.Counter()
for s, v in stacks.items(): roots[s[0]] += v
print(f'\ncall stacks: {stot} samples ({100 * stot / tot:.0f}% of the busy samples walked){", lost (table full) " + str(lost) if lost else ""}')
print('  outermost frames: ' + ', '.join(f'{k} {100 * v / stot:.1f}%' for k, v in roots.most_common(5)))
incl = collections.Counter()
for s, v in stacks.items():
    for f in set(s): incl[f] += v
print('  inclusive time (the function or anything it called):')
for k, v in incl.most_common(a.top): print(f'  {100 * v / stot:5.1f}%  {k}')
if a.focus:
    F = a.focus; callers = collections.Counter(); callees = collections.Counter(); selfv = 0; fin = 0
    for s, v in stacks.items():
        if F not in s: continue
        fin += v; i = s.index(F)
        callers[s[i - 1] if i > 0 else '(outermost)'] += v
        if i + 1 < len(s): callees[s[i + 1]] += v
        else: selfv += v
    print(f'\n{F}: inclusive {100 * fin / stot:.1f}%, self {100 * selfv / stot:.1f}%')
    print('  called from: ' + ', '.join(f'{k} {100 * v / stot:.1f}%' for k, v in callers.most_common(8)))
    print('  calls:'); [print(f'    {100 * v / stot:5.1f}%  {k}') for k, v in callees.most_common(30)]
if a.tree:
    tree = {}
    for s, v in stacks.items():
        n = tree
        for f in s[:a.depth]:
            e = n.setdefault(f, [0, {}]); e[0] += v; n = e[1]
    def show(n, ind):
        for f, (v, ch) in sorted(n.items(), key=lambda x: -x[1][0]):
            if 100 * v / stot < a.min: continue
            selfv = v - sum(c[0] for c in ch.values())
            print(f'{"  " * ind}{100 * v / stot:5.1f}%  {f}' + (f'   (self {100 * selfv / stot:.1f}%)' if ch and 100 * selfv / stot >= a.min else ''))
            show(ch, ind + 1)
    print('\ncall tree (inclusive % of walked samples):'); show(tree, 1)
