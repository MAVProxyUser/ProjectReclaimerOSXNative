#!/usr/bin/env python3
"""Megalo lab: run the Reclaimer client's own Megalo tools on scripts, and fuzz them (backlog 72).

The client (a Rust exe) carries a ReachVariantTool-dialect Megalo compiler + VM. Three of its subcommands reach the
compiler without MCC or a game, and run here under the loader in ~0.07 s:
    megalo-check <source>                         parse + type-check, print the required runtime capabilities
    mode-compile --output <preset> <source>       compile into a playable custom-game preset (JSON)
    mode-check <preset>                           validate a preset (also the JSON op-list `script` presets)

    python3 shim/tools/megalo-lab/megalo_fuzz.py corpus                          # the example scripts / presets embedded
                                                                                 # in the client -> ~/Library/Application
                                                                                 # Support/ReclaimerMac/megalo-corpus
    python3 shim/tools/megalo-lab/megalo_fuzz.py run  <cmd> <file> [args...]   # one run, filtered output + verdict
    python3 shim/tools/megalo-lab/megalo_fuzz.py fuzz [--n 2000] [--jobs 2] [--seed N] [--corpus DIR] [--out DIR]
    python3 shim/tools/megalo-lab/megalo_fuzz.py json [--n 2000] ...            # mutate presets for mode-check

Verdicts: OK (accepted), ERR (refused with a message: expected for most mutants), PANIC (a Rust panic: the client
would abort -- a crash, i.e. DoS), CRASH (a fault the loader reported: memory safety), HANG (timeout). Interesting
inputs (PANIC / CRASH / HANG, and every new distinct ERR message) are kept in --out with the tool's output beside them.
Front end only: the VM itself runs inside a match (see shim/docs/megalo.md for runtime tests on a local server).
Never runs a game; no network.
"""
import argparse, hashlib, json, os, random, re, subprocess, sys, tempfile, time
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..', '..', '..'))
LOADER = os.path.join(ROOT, 'shim', 'build', 'peload')
NOISE = re.compile(r'^\s*\[|^peload|^  (image|subsystem|mapped|TEB|guest|bound|static|block|TLS|entry|\(at)|DllMain|'
                   r'LoadLibrary|sigpatch|calling entry|unimplemented behavior|^\s*$')


def client_exe():
    c = sorted((f for f in os.listdir(os.path.join(ROOT, 'client')) if f.startswith('project-reclaimer-client-v') and f.endswith('.exe')),
               key=lambda s: [int(x) if x.isdigit() else x for x in re.split(r'(\d+)', s)])
    return os.path.join(ROOT, 'client', c[-1])


def run(args, timeout=20):
    """-> (verdict, filtered output, seconds)"""
    t0 = time.time()
    env = dict(os.environ, PELOAD_STATS_HTTP='0', PELOAD_FORENSIC='0')
    try:
        p = subprocess.run(['nice', '-n', '10', 'arch', '-x86_64', LOADER, client_exe()] + args, capture_output=True,
                           timeout=timeout, env=env)
        out = (p.stdout + p.stderr).decode('utf-8', 'replace')
    except subprocess.TimeoutExpired as e:
        out = ((e.stdout or b'') + (e.stderr or b'')).decode('utf-8', 'replace')
        return 'HANG', filt(out), time.time() - t0
    o = filt(out)
    if '[CRASH]' in out or '[seh] dispatch loop ended (unhandled)' in out or 'STATUS_ACCESS_VIOLATION' in out or 'exception 0xC0000005' in out:
        v = 'CRASH'
    elif 'panicked at' in out or 'stack overflow' in out.lower():
        v = 'PANIC'
    elif re.search(r'^Error:', o, re.M):
        v = 'ERR'
    else:
        v = 'OK'
    return v, o, time.time() - t0


def filt(out):
    keep = [l for l in out.splitlines() if not NOISE.search(l) or '[CRASH]' in l]
    return '\n'.join(keep[:200])


def err_key(o):
    m = re.search(r'^Error: (.*)$', o, re.M)
    if not m:
        return None
    return re.sub(r'\d+', 'N', m.group(1))[:160]   # the message without positions / numbers


# ---- mutations of Megalo source -------------------------------------------------------------------------------------
NUMS = ['0', '-1', '1', '15', '16', '17', '31', '32', '63', '64', '255', '256', '1023', '1024', '32767', '32768', '-32768',
        '65535', '65536', '2147483647', '2147483648', '-2147483648', '4294967295', '4294967296', '9223372036854775807',
        '-9223372036854775808', '18446744073709551616', '999999999999999999999999', '1e308', '0.5', '-0', '0x7fffffff']
KINDS = ['number', 'object', 'player', 'team', 'timer']
SCOPES = ['global', 'player', 'object', 'team']
TEXTS = ['', 'a' * 4096, '‮', '\x00', '%s%n%x', '"', '\\', '\n', '\u0000x', 'é' * 300, '🙂' * 200]


def tokens_of(corpus):
    ids = set()
    for s in corpus:
        ids.update(re.findall(r'[A-Za-z_][A-Za-z0-9_.]*', s))
    return sorted(ids)


def mutate(src, ids, rnd):
    s = src
    for _ in range(rnd.randint(1, 4)):
        k = rnd.randrange(12)
        if k == 0:     # a number -> an extreme
            ms = list(re.finditer(r'(?<![A-Za-z_\d])-?\d+(\.\d+)?', s))
            if ms:
                m = rnd.choice(ms); s = s[:m.start()] + rnd.choice(NUMS) + s[m.end():]
        elif k == 1:   # an index [N] -> extreme / another variable
            ms = list(re.finditer(r'\[(\d+)\]', s))
            if ms:
                m = rnd.choice(ms); v = rnd.choice(NUMS + ['current_player', 'global.number[0]', 'global.number[global.number[0]]'])
                s = s[:m.start()] + '[' + v + ']' + s[m.end():]
        elif k == 2:   # variable kind swap (type confusion): global.number[x] -> global.object[x] ...
            ms = list(re.finditer(r'\b(global|player|object|team)\.(number|object|player|team|timer)\b', s))
            if ms:
                m = rnd.choice(ms); s = s[:m.start()] + rnd.choice(SCOPES) + '.' + rnd.choice(KINDS) + s[m.end():]
        elif k == 3:   # an identifier -> another one from the corpus (API confusion)
            ms = list(re.finditer(r'[A-Za-z_][A-Za-z0-9_.]*', s))
            if ms:
                m = rnd.choice(ms); s = s[:m.start()] + rnd.choice(ids) + s[m.end():]
        elif k == 4:   # duplicate a line
            L = s.split('\n'); i = rnd.randrange(len(L)); L.insert(i, L[i]); s = '\n'.join(L)
        elif k == 5:   # delete a line
            L = s.split('\n'); del L[rnd.randrange(len(L))]; s = '\n'.join(L)
        elif k == 6:   # deep nesting
            d = rnd.choice([16, 64, 256, 2000])
            blk = 'if global.number[0] == 0 then\n' * d + 'global.number[0] = 1\n' + 'end\n' * d
            s += '\n' + blk
        elif k == 7:   # long expressions / deep parentheses
            d = rnd.choice([64, 1000, 20000])
            s += '\nglobal.number[0] = ' + rnd.choice(['(' * d + '1' + ')' * d, '+'.join(['1'] * d), '-' * d + '1'])
        elif k == 8:   # strings
            ms = list(re.finditer(r'"[^"\n]*"', s))
            if ms:
                m = rnd.choice(ms); s = s[:m.start()] + json.dumps(rnd.choice(TEXTS)) + s[m.end():]
        elif k == 9:   # call with wrong arity
            ms = list(re.finditer(r'\(([^()\n]*)\)', s))
            if ms:
                m = rnd.choice(ms); args = m.group(1)
                new = rnd.choice(['', args + ', ' + args, ', '.join(['0'] * rnd.choice([1, 9, 33, 300])), args + ',', '(' + args + ')'])
                s = s[:m.start()] + '(' + new + ')' + s[m.end():]
        elif k == 10:  # byte noise
            if s:
                i = rnd.randrange(len(s)); s = s[:i] + rnd.choice(['\x00', '\x7f', '\ud800'[:0] + '\xff', '\t', 'end', 'do', ':', '.', '[', ']']) + s[i:]
        else:          # many functions / events / aliases (capacity limits)
            n = rnd.choice([17, 33, 300])
            s += '\n' + '\n'.join('alias a%d = global.number[%d]' % (i, i % 16) for i in range(n))
    return s


# ---- mutations of a preset JSON (mode-check) --------------------------------------------------------------------------
JVALS = [None, True, False, 0, -1, 1, 2**31, 2**31 - 1, -2**31, 2**32, 2**63 - 1, -2**63, 2**64, 1e308, -1e308, 0.5, '',
         'a' * 5000, [], {}, [[[[[]]]]], {'op': 'set'}, '\u0000', '../../x']


def jmut(o, rnd, depth=0):
    if isinstance(o, dict) and o:
        k = rnd.choice(list(o))
        r = rnd.random()
        if r < 0.35: o[k] = rnd.choice(JVALS)
        elif r < 0.5: del o[k]
        elif r < 0.6: o[k + rnd.choice(['', '_', 'x'])] = rnd.choice(JVALS)
        else: jmut(o[k], rnd, depth + 1)
    elif isinstance(o, list) and o:
        i = rnd.randrange(len(o)); r = rnd.random()
        if r < 0.35: o[i] = rnd.choice(JVALS)
        elif r < 0.5: o.append(rnd.choice(o))
        elif r < 0.55: o.extend([o[i]] * rnd.choice([33, 300, 5000]))
        else: jmut(o[i], rnd, depth + 1)
    return o


CORPUS = os.path.expanduser('~/Library/Application Support/ReclaimerMac/megalo-corpus')


def extract_corpus():
    """the Megalo sources and presets the client carries (its examples / templates) -> CORPUS (not in git: theirs)"""
    d = open(client_exe(), 'rb').read(); os.makedirs(CORPUS, exist_ok=True); n = 0; seen = set()
    for m in re.finditer(rb'"megalo":\s*"', d):
        q = m.end() - 1
        try:
            src, _ = json.JSONDecoder().raw_decode(d[q:q + 400000].decode('utf-8', 'replace'))
        except ValueError:
            continue
        if src in seen: continue
        seen.add(src); n += 1
        first = re.sub(r'[^A-Za-z0-9]+', '_', src.split('\n', 1)[0])[:40]
        open(os.path.join(CORPUS, 'script%02d%s.megalo' % (n, first)), 'w').write(src)
    for m in re.finditer(rb'\{\s*"version":\s*1,\s*"name":', d):   # custom-game presets (JSON op-list scripts)
        try:
            obj, _ = json.JSONDecoder().raw_decode(d[m.start():m.start() + 200000].decode('utf-8', 'replace'))
        except ValueError:
            continue
        if 'mode' not in obj:   # (a preset's nested JSON op-list `script` matches too)
            continue
        nm = re.sub(r'[^A-Za-z0-9]+', '_', str(obj.get('name', 'preset')))[:40]
        json.dump(obj, open(os.path.join(CORPUS, 'preset_%s.json' % nm), 'w'), indent=1); n += 1
    print('%d files in %s' % (n, CORPUS))


def campaign(kind, a):
    corpus_dir = a.corpus or CORPUS
    if not os.path.isdir(corpus_dir) or not os.listdir(corpus_dir):
        extract_corpus()
    out = a.out or os.path.join(tempfile.gettempdir(), 'megalo-fuzz-%s-%d' % (kind, int(time.time())))
    os.makedirs(out, exist_ok=True)
    rnd = random.Random(a.seed)
    if kind == 'src':
        corpus = [open(os.path.join(corpus_dir, f), encoding='utf-8', errors='replace').read() for f in sorted(os.listdir(corpus_dir)) if f.endswith('.megalo')]
        ids = tokens_of(corpus)
    else:
        corpus = [json.load(open(os.path.join(corpus_dir, f))) for f in sorted(os.listdir(corpus_dir)) if f.endswith('.json')]
    seen_err, stats = {}, {}
    tmp = tempfile.mkdtemp(prefix='megalo-')

    def one(i):
        r = random.Random(rnd.random())
        if kind == 'src':
            text = mutate(r.choice(corpus), ids, r); ext = '.megalo'
            cmd = r.choice([['megalo-check'], ['mode-compile', '--output', os.path.join(tmp, 'o%d.json' % i)]])
        else:
            text = json.dumps(jmut(json.loads(json.dumps(r.choice(corpus))), r)); ext = '.json'; cmd = ['mode-check']
        f = os.path.join(tmp, 'm%d%s' % (i, ext))
        with open(f, 'w', encoding='utf-8', errors='surrogatepass') as fo:
            fo.write(text)
        v, o, dt = run(cmd + [f], timeout=a.timeout)
        try: os.unlink(f)
        except OSError: pass
        return i, text, ext, cmd[0], v, o, dt

    t0 = time.time()
    with ThreadPoolExecutor(a.jobs) as ex:
        for i, text, ext, cmd, v, o, dt in ex.map(one, range(a.n)):
            stats[v] = stats.get(v, 0) + 1
            keep = v in ('PANIC', 'CRASH', 'HANG')
            ek = err_key(o) if v == 'ERR' else None
            if ek and ek not in seen_err:
                seen_err[ek] = 0; keep = True
            if ek: seen_err[ek] += 1
            if keep:
                h = hashlib.sha1(text.encode('utf-8', 'surrogatepass')).hexdigest()[:10]
                base = os.path.join(out, '%s_%s_%s' % (v, cmd, h))
                open(base + ext, 'w', encoding='utf-8', errors='surrogatepass').write(text)
                open(base + '.out', 'w').write('%s %s (%.2fs)\n%s\n' % (v, cmd, dt, o))
                if v != 'ERR':
                    print('  %s %s %s (%.1fs): %s' % (v, cmd, os.path.basename(base), dt, o.splitlines()[-1] if o else ''), flush=True)
            if (i + 1) % 200 == 0:
                print('  %d/%d %s  %.1f/s  distinct errors %d' % (i + 1, a.n, stats, (i + 1) / (time.time() - t0), len(seen_err)), flush=True)
    print('done: %s; %d distinct error messages; kept in %s' % (stats, len(seen_err), out))
    with open(os.path.join(out, 'errors.txt'), 'w') as fo:
        for k, n in sorted(seen_err.items(), key=lambda x: -x[1]):
            fo.write('%6d  %s\n' % (n, k))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    sub = ap.add_subparsers(dest='what', required=True)
    r = sub.add_parser('run'); r.add_argument('args', nargs=argparse.REMAINDER)
    sub.add_parser('corpus')
    for name in ('fuzz', 'json'):
        f = sub.add_parser(name)
        f.add_argument('--n', type=int, default=2000); f.add_argument('--jobs', type=int, default=2)
        f.add_argument('--seed', type=int, default=1); f.add_argument('--timeout', type=float, default=20)
        f.add_argument('--corpus'); f.add_argument('--out')
    a = ap.parse_args()
    if a.what == 'corpus':
        extract_corpus(); return
    if a.what == 'run':
        v, o, dt = run(a.args); print(o); print('== %s (%.2fs)' % (v, dt)); return
    campaign('src' if a.what == 'fuzz' else 'json', a)


if __name__ == '__main__':
    main()
