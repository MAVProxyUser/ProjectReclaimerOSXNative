"""Which match recordings a WALKER made (2026-11-09; the user: the You tab must use "only real stats"). nhook records every match
it is in (ntracks.c) with "you_plays": its local player is a real player -- also when a walker drives that player (the public walker,
walker battles). Such a recording's "you" is the walker, not a human: coach.py must never take it for the user, and engage-extract.py
leaves the walker's slot out of the players' data. A walker's recording is one whose header says "walker": true (ntracks.c, since
2026-11-09) or one named in a walker's log ("[tracks] recording <map> (...) -> <file>"; battles/*.log are walker logs) -- the logs
cover the recordings made before the header field existed.
    import walkerrec; walkerrec.walker_files() -> set of recording file names (basenames)
    python3 shim/tools/walkerrec.py   prints them"""
import glob, gzip, json, os, re

SUP = os.path.expanduser('~/Library/Application Support/ReclaimerMac')
REC = re.compile(rb'\[tracks\] recording .* -> (\S+\.jsonl\.gz)')

def header(path):
    try:
        with gzip.open(path, 'rt', errors='replace') as f: return json.loads(f.readline())
    except Exception: return {}

def walker_files(tracks=None, logs=None):
    tracks = tracks or os.path.join(SUP, 'tracks'); out = set()
    for lp in (logs if logs is not None else glob.glob(os.path.join(SUP, 'battles', '*.log'))):
        try:
            with open(lp, 'rb') as f:
                for l in f:
                    if b'[tracks] recording' in l:
                        m = REC.search(l)
                        if m: out.add(m.group(1).decode(errors='replace'))
        except OSError: pass
    for p in glob.glob(os.path.join(tracks, '*.jsonl.gz')):
        b = os.path.basename(p)
        if b not in out and header(p).get('walker'): out.add(b)
    return out

if __name__ == '__main__':
    for b in sorted(walker_files()): print(b)
