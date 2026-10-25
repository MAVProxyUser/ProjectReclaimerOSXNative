#!/usr/bin/env python3
"""Where each thread of a game client spends its time, from a macOS `sample` file -- the way the HugeGrass render stall was
found (2026-10-25: halo3's render thread waiting in sub_drain <- ctx_Map, the GL thread asleep in glh_end_sync).

    sample <pid of the process serving the stats page> 5 -file /tmp/s.txt      (5 s; the game keeps running)
    python3 shim/tools/sample-threads.py /tmp/s.txt [--depth N] [thread name filter ...]

For every thread (or those whose name contains a filter, e.g. guest-0x18000a120 = halo3's engine thread,
guest-0x1800d1e9c = halo3's render / worker threads, shim-gl-submit, shim-compute) it prints the share of samples ending in
each leaf, with the named frames above it ("_pthread_cond_wait <- sub_drain <- ctx_Map"). Unnamed guest frames (halo3 /
client code translated by Rosetta, no frame pointers) show as their addresses; idle worker threads are left out."""
import re, sys, collections

depth = 4

def threads(path):
    L = open(path, errors='replace').read().split('\n')
    i = next(k for k, l in enumerate(L) if l.startswith('Call graph:'))
    out = []
    cur = None
    path_ = []
    for l in L[i + 1:]:
        if l.startswith('Total number in stack') or l.startswith('Sort by top of stack'):
            break
        m = re.match(r'^\s{4}(\d+) (Thread_\S+.*)$', l)
        if m:
            cur = {'name': m.group(2), 'total': int(m.group(1)), 'nodes': []}
            out.append(cur)
            path_ = []
            continue
        m = re.match(r'^(\s+[+!:| ]*)(\d+) (.*)$', l)
        if not m or cur is None:
            continue
        d, n, lab = len(m.group(1)), int(m.group(2)), m.group(3)
        while path_ and path_[-1][0] >= d:
            path_.pop()
        if path_:
            path_[-1][3] -= n
        node = [d, n, lab, n, [p[2] for p in path_]]
        path_.append(node)
        cur['nodes'].append(node)
    return out

def short(lab):
    s = re.sub(r'\s*\(in [^)]*\)\s*\+?\s*\d*\s*', ' ', lab).strip()
    s = re.sub(r'\s+\[0x[0-9a-f]+\]', '', s) if not s.startswith('???') else s
    return s.strip()

def main():
    global depth
    a = sys.argv[1:]
    depth = 4
    if '--depth' in a:
        i = a.index('--depth'); depth = int(a[i + 1]); del a[i:i + 2]
    f = a[0]
    filt = a[1:]
    for t in threads(f):
        if filt and not any(x in t['name'] for x in filt):
            continue
        selfc = collections.Counter()
        for d, n, lab, s, anc in t['nodes']:
            if s <= 0:
                continue
            named = [short(a) for a in anc + [lab]]
            named = [x for x in named if x and x != '???' and not x.startswith('??? [0x7ff')]   # (unnamed system frames: noise)
            selfc[' <- '.join(reversed(named[-depth:]))] += s
        tot = sum(selfc.values()) or 1
        waity = sum(v for k, v in selfc.items() if re.search(r'cond_wait|semwait|mach_msg|kevent|nanosleep|psynch|ulock|workq|__select|lock_wait|usleep', k))
        if not filt and waity > 0.97 * tot:
            continue   # an idle thread
        print(f"== {t['name'][:70]}  ({tot} samples, {100 * waity / tot:.0f}% in waits)")
        for k, v in selfc.most_common(8):
            print(f'  {100 * v / tot:5.1f}%  {k[:150]}')

if __name__ == '__main__':
    main()
