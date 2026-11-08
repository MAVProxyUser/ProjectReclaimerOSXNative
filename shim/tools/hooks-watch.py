#!/usr/bin/env python3
"""Watch the native app's hooks check while someone plays (shim/native/nhchk.c GET /hooks.json): one line per item whose verdict
changes (waiting -> ok, anything -> FAILED / silent), the summary when it changes, and a line when the page stops answering.
    python3 shim/tools/hooks-watch.py [port] [seconds between polls]      (default 8765, 10 s; Ctrl-C ends it)"""
import json, sys, time, urllib.request
port = int(sys.argv[1]) if len(sys.argv) > 1 else 8765; every = float(sys.argv[2]) if len(sys.argv) > 2 else 10
seen, summ, up = {}, None, None
while True:
    try:
        j = json.load(urllib.request.urlopen('http://127.0.0.1:%d/hooks.json' % port, timeout=4))
        if up is not True: print('[watch] hooks check answering (app %s, %s)' % (j['app']['version'], 'known build' if j['app']['known'] else 'UNKNOWN build'), flush=True); up = True
        for x in j['items']:
            v = x['verdict']; old = seen.get(x['name'])
            if old != v:
                if old is not None or v in ('FAILED', 'silent'):
                    print('[watch] %-40s %s -> %s  (fired %d; %s)' % (x['name'], old or '-', v, x['fires'], x['why'][:120] or x['feature'][:120]), flush=True)
                seen[x['name']] = v
        s = j['summary']; k = (s['ok'], s['failed'], s['silent'])
        if k != summ: print('[watch] summary: %d ok, %d failed, %d silent, %d waiting, %d off; %d s in matches' % (s['ok'], s['failed'], s['silent'], s['waiting'], s['off'], j['match_seconds']), flush=True); summ = k
    except Exception as e:
        if up is not False: print('[watch] the page does not answer (%s)' % type(e).__name__, flush=True); up = False
    time.sleep(every)
