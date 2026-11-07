#!/usr/bin/env python3
"""Where a thread's samples END, by binary (macOS `sample` output): its own time in each image -- the translated guest code
("Rosetta JIT" / "<unknown binary>"), Rosetta's runtime helpers ("Rosetta Runtime Routines"), the loader (peload), system
libraries -- and the top leaf functions inside the Rosetta images.
    python3 shim/tools/sample-binaries.py <sample.txt> <thread name filter> [more filters]"""
import re, sys, collections
L = open(sys.argv[1], errors='replace').read().split('\n'); filt = sys.argv[2:]
i = next(k for k, l in enumerate(L) if l.startswith('Call graph:'))
cur = None; path = []; by = collections.defaultdict(collections.Counter); fn = collections.defaultdict(collections.Counter); tot = collections.Counter()
def close(node, th):
    d, lab, self_ = node
    if self_ > 0:
        m = re.search(r'\(in ([^)]*)\)', lab); b = m.group(1) if m else '?'
        by[th][b] += self_
        if 'Rosetta' in b or b == '<unknown binary>': fn[th][re.sub(r'\s*\(in [^)]*\).*', '', lab).strip()[:60] + ' | ' + b] += self_
for l in L[i + 1:]:
    if l.startswith('Total number in stack'): break
    m = re.match(r'^\s{4}(\d+) (Thread_\S+.*)$', l)
    if m:
        while path: close(path.pop(), cur)
        name = m.group(2); cur = name if any(f in name for f in filt) else None
        if cur: tot[cur] = int(m.group(1))
        continue
    if not cur: continue
    m = re.match(r'^(\s+[+!:| ]*)(\d+) (.*)$', l)
    if not m: continue
    d, n, lab = len(m.group(1)), int(m.group(2)), m.group(3)
    while path and path[-1][0] >= d: close(path.pop(), cur)
    if path: path[-1][2] -= n
    path.append([d, lab, n])
while path: close(path.pop(), cur)
for th in by:
    t = tot[th] or 1; print(f'== {th[:80]} ({t} samples)')
    for b, v in by[th].most_common(10): print(f'  {100 * v / t:5.1f}%  {b}')
    for f, v in fn[th].most_common(8): print(f'      {100 * v / t:5.1f}%  {f}')
