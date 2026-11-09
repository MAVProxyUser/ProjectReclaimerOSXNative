#!/usr/bin/env python3
"""The game's window mode for one launch (2026-11-08, play-vs-walkers.sh; the user: "don't full screen me though, I want to watch
the web stats at the same time"). The app's --settings flag takes the ENGINE's settings, not the profile's, so the profile's
settings.json is changed in place -- graphics.display_mode / width / height ONLY -- and put back after the game:
    python3 shim/tools/play-window.py set <settings.json> <W>x<H>      prints the old three values (JSON), sets "windowed" + the size
    python3 shim/tools/play-window.py restore <settings.json> '<json>'  puts those three values back; everything else is left alone"""
import json, os, sys

def save(p, d):
    t = p + '.pv.tmp'; json.dump(d, open(t, 'w'), indent=2); os.chmod(t, 0o600); os.replace(t, p)

def main():
    cmd, p = sys.argv[1], sys.argv[2]; d = json.load(open(p)); g = d.setdefault('graphics', {})
    if cmd == 'set':
        print(json.dumps({k: g.get(k) for k in ('display_mode', 'width', 'height')}))
        w, h = (int(x) for x in sys.argv[3].split('x')); g['display_mode'] = 'windowed'; g['width'] = w; g['height'] = h; save(p, d)
    elif cmd == 'restore':
        for k, v in json.loads(sys.argv[3]).items():
            if v is None: g.pop(k, None)
            else: g[k] = v
        save(p, d); print('the window settings are back: %s' % sys.argv[3])
    return 0

if __name__ == '__main__':
    sys.exit(main())
