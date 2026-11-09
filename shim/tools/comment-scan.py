#!/usr/bin/env python3
"""Code swallowed by a mid-line // comment (2026-11-09): in aa7733b a comment appended to the walker's stuck test commented out the
statements after it on the same line (tjump = now; hh = 0; tback = now; ...) -- the stuck alarm fired every step for ~6 hours of
public play before a step trace found it. Prints every line whose // comment (outside a string) is preceded by code and ends like a
statement (";" with an assignment in the tail). shim/native/build.sh runs it on the walker sources as a warning.
    python3 shim/tools/comment-scan.py [files...]   (default: shim/native/*.c shim/src/nav.c); exit 1 when something is found"""
import glob, os, re, sys

HERE = os.path.dirname(os.path.abspath(__file__))
def first_comment(l):
    q = False
    for j in range(len(l) - 1):
        c = l[j]
        if c == '"' and (j == 0 or l[j - 1] != '\\'): q = not q
        if not q and l[j:j + 2] == '//': return j
    return -1
def main():
    files = sys.argv[1:] or sorted(glob.glob(os.path.join(HERE, '..', 'native', '*.c'))) + [os.path.join(HERE, '..', 'src', 'nav.c')]
    bad = 0
    for p in files:
        try: L = open(p, errors='replace').read().split('\n')
        except OSError: continue
        for i, l in enumerate(L, 1):
            k = first_comment(l)
            if k < 0 or not l[:k].strip(): continue
            tail = l[k + 2:]
            if re.search(r';\s*\}*\s*$', tail) and re.search(r'[a-z_]\w*\s*(=|\+=)\s*[\w(]', tail):
                print('%s:%d: code after a mid-line // comment? ...%s' % (os.path.relpath(p), i, tail[-110:])); bad += 1
    return 1 if bad else 0
if __name__ == '__main__':
    sys.exit(main())
