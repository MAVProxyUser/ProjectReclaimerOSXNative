#!/bin/bash
# Offline test of the native hook's packet log (shim/native/nnet.c): a small program sends UDP datagrams (a QUIC-looking
# long header, a short header, a plaintext tag) and opens a TCP connection with nhook.dylib injected
# (NHOOK_SELFTEST=1 starts the stats server without the app, on NHOOK_STATS_HTTP, default 8799 here), then reads
# /packets.json (with ?rdns=1: 127.0.0.1 must come back as localhost) and checks the records; it also writes one of the app's
# "[voice] active ... heard ..." reports to stderr (only the sender above -60 dBFS talks). No game, no network beyond 127.0.0.1.
set -e
cd "$(dirname "$0")/../.."
bash shim/native/build.sh >/dev/null
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
cat > "$T/t.c" <<'C'
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
int main(void) {
    int u = socket(AF_INET, SOCK_DGRAM, 0), r = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a = {0}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(r, (struct sockaddr*)&a, sizeof a); socklen_t al = sizeof a; getsockname(r, (struct sockaddr*)&a, &al);
    unsigned char lh[16] = {0xc3, 0, 0, 0, 1, 8}, sh[8] = {0x41, 1, 2, 3}, tg[8] = {'M', 'C', 'H', '1'};
    sendto(u, lh, sizeof lh, 0, (struct sockaddr*)&a, sizeof a);
    struct iovec io = {sh, sizeof sh}; struct msghdr m = {0}; m.msg_name = &a; m.msg_namelen = sizeof a; m.msg_iov = &io; m.msg_iovlen = 1; sendmsg(u, &m, 0);
    sendto(u, tg, sizeof tg, 0, (struct sockaddr*)&a, sizeof a);
    char b[64]; struct sockaddr_in f; socklen_t fl = sizeof f; for (int i = 0; i < 3; i++) recvfrom(r, b, sizeof b, 0, (struct sockaddr*)&f, &fl);
    int ls = socket(AF_INET, SOCK_STREAM, 0); struct sockaddr_in t = {0}; t.sin_family = AF_INET; t.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(ls, (struct sockaddr*)&t, sizeof t); listen(ls, 1); socklen_t tl = sizeof t; getsockname(ls, (struct sockaddr*)&t, &tl);
    int c = socket(AF_INET, SOCK_STREAM, 0); connect(c, (struct sockaddr*)&t, sizeof t); int s = accept(ls, 0, 0);
    write(c, "hello", 5); read(s, b, 5); close(c); close(s);
    // the app's voice report (nstats.c st_voice_report): one talker, one open microphone sending silence, one hissing
    const char* vr = "[voice] active true sending standard members 3 sent 0 (0 bytes) received 300 relayed 0 mixed 200 frames rms -25.0 dBFS; heard "
                     "0x00000000000000aa: 88 frames (0 concealed) rms -29.4 dBFS peak -19.6 dBFS, 0x00000000000000bb: 100 frames (0 concealed) rms -120.0 dBFS peak -120.0 dBFS, "
                     "0x00000000000000cc: 7 frames (0 concealed) rms -112.4 dBFS peak -90.3 dBFS\n";
    write(2, vr, strlen(vr));
    sleep(2); return 0;
}
C
clang -O1 -o "$T/t" "$T/t.c"
PORT=${NHOOK_STATS_HTTP:-8799}
PELOAD_FORENSIC_DIR="$T/forensics" NHOOK_HEATMAP_DIR="$T/heatmaps" NHOOK_SELFTEST=1 NHOOK_VOICE_LOG=1 NHOOK_STATS_HTTP=$PORT DYLD_INSERT_LIBRARIES="$PWD/shim/build/nhook.dylib" "$T/t" 2>"$T/err" &
P=$!; sleep 1.5
curl -s -m 3 "http://127.0.0.1:$PORT/packets.json?max=50&rdns=1" > /dev/null || true   # (queues the reverse lookups)
sleep 0.5; curl -s -m 3 "http://127.0.0.1:$PORT/packets.json?max=50&rdns=1" > "$T/pk.json" || true
wait $P || true
python3 - "$T/pk.json" <<'PY'
import json, sys
d = json.load(open(sys.argv[1])); ev = d['events']; types = [(e['dir'], e['type']) for e in ev]
want = [('tx', 'quic-initial'), ('tx', 'quic-1rtt'), ('tx', 'tag MCH1'), ('rx', 'quic-initial'), ('rx', 'quic-1rtt'), ('rx', 'tag MCH1'), ('tx', 'tcp-connect'), ('tx', 'tcp-close')]
bad = [w for w in want if w not in types]
for e in ev: print(' ', e['dir'], e['type'], e['len'], e.get('peer', ''), e.get('rdns', ''))
if not any(e.get('rdns') == 'localhost' for e in ev): bad.append(('reverse DNS of 127.0.0.1', 'missing'))
tc = [e for e in ev if e['type'] == 'tcp-close']
if tc and tc[0]['len'] != 5: bad.append(('tcp bytes', tc[0]['len']))
print('PASS' if not bad else 'FAIL: missing %s' % bad); sys.exit(1 if bad else 0)
PY
# the voice report: only 0xaa talks
V=$(grep -c 'voice report: ' "$T/err" || true); VT=$(grep 'voice report: ' "$T/err" | grep -c 'talking' || true)
grep 'voice report: ' "$T/err" | sed 's/^/  /'
if [ "$V" = 3 ] && [ "$VT" = 1 ] && grep -q 'voice report: 00000000000000aa 88 frames.*talking' "$T/err"; then echo "PASS voice report"; else echo "FAIL voice report ($V senders, $VT talking)"; exit 1; fi
