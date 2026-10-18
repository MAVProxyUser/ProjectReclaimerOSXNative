#!/bin/bash
# Documentation pictures of the live stats page and the forensic archive viewer (README "Live game stats over HTTP",
# shim/docs/forensics.md), taken from a running game's own pages with ?demo=1: player names, ids and chat lines
# anonymized by the pages themselves. tools/pageshot.m (an offscreen WKWebView, built here for arm64) loads each page
# once and takes every section from that one load, so a player's alias is the same in all of a page's pictures.
#     bash shim/tools/pageshot.sh [http://127.0.0.1:8765]
#   OUT=<dir>       where the pictures go (default shim/docs/img)
#   TEXT=<dir>      also keep each picture's text (to check the anonymization: no real name, id or chat line)
#   FX_SESSION=<n>  the forensic session to show (default: the largest finished one)
# Only reads the pages (GET); it never presses a control. Run it while the game is in a busy match.
set -eu
cd "$(dirname "$0")/../.."
BASE=${1:-http://127.0.0.1:8765}; OUT=${OUT:-shim/docs/img}; BIN=shim/build/pageshot
if [ ! -x "$BIN" ] || [ shim/tools/pageshot.m -nt "$BIN" ]; then
    mkdir -p shim/build
    clang -arch arm64 -fobjc-arc -O1 -o "$BIN" shim/tools/pageshot.m -framework Cocoa -framework WebKit -framework ImageIO
fi
for i in $(seq 60); do   # a match with players in it (the client may still be loading)
    n=$(curl -s -m 3 "$BASE/stats.json" | python3 -c 'import json,sys; j=json.load(sys.stdin); print(len(j["players"]) if j.get("in_match") else 0)' 2>/dev/null || echo 0)
    [ "${n:-0}" -gt 0 ] && break; [ "$i" = 1 ] && echo "waiting for a match on $BASE ..."; sleep 5
done
[ "${n:-0}" -gt 0 ] || { echo "no match with players on $BASE"; exit 1; }
TX=(); [ -n "${TEXT:-}" ] && TX=(-x "$TEXT")

# stats page: the player with the most kills among those alive a while (not this client, a name nobody else has) selected,
# the table sorted by kills, the recordings list open, then the page frozen (its live stream closed) so every picture
# shows the same moment; the test walker's route (PELOAD_NAV, test runs only) is left off the map
STATS_JS='
const cnt={}; for(const p of snap.players) cnt[p.name]=(cnt[p.name]||0)+1;
const now=snap.players.filter(p=>p.alive&&!p.you&&cnt[p.name]===1), by=(a,b)=>(b.kills||0)-(a.kills||0)||(b.shots||0)-(a.shots||0);
const pick=now.filter(p=>p.life>20).sort(by)[0]||now.sort(by)[0];
if(pick) sel=pick.slot;
sortK="kills"; sortD=-1; sortMode="full"; sortHeads();
const dm=drawMap; drawMap=function(){ if(snap) snap.nav=null; dm(); };
es.close(); renderTable(); renderDet(); renderConn(); drawMap();
const st=document.createElement("style"); st.textContent="#rec{max-width:800px}"; document.head.appendChild(st);
document.getElementById("recl").open=true;
await new Promise(r=>setTimeout(r,1500));
return "selected slot "+sel+" ("+(pick?pick.kills+" kills, alive "+Math.round(pick.life)+" s":"nobody")+"), "+snap.players.length+" players, "+document.getElementById("dc").textContent;'
# then the table alone at full width (it is wider than its column) with its first ~22 rows
WIDE_JS='
const st=document.createElement("style");
st.textContent=".grid{grid-template-columns:minmax(0,1fr)!important}.grid>.panel:first-child .wrap{max-height:640px;overflow:hidden}";
document.head.appendChild(st); await new Promise(r=>setTimeout(r,800));'
"$BIN" -w 1440 -t 8 ${TX[@]+"${TX[@]}"} -j "$STATS_JS" "$BASE/?demo=1" "$OUT" \
    stats-overlay='#ovp' stats-record='#rec' stats-connected='#conn' stats-feed='#feed' stats-chat='#chat' \
    stats-rules='#rules' stats-heatmap@1.5='.col > .panel:first-child' stats-player='#det' \
    -- -w 1880 -j "$WIDE_JS" stats-players='.grid > .panel:first-child'

# forensic viewer: the largest finished session (or FX_SESSION), every lane shown, the list cut at 600 px and scrolled to
# its first player chat line (voice, chat and kills together)
FX_JS='
const st=document.createElement("style"); st.textContent="#sl,#tlw{max-height:none}#ls{max-height:600px}#pb{display:none}"; document.head.appendChild(st);
const want='"'${FX_SESSION:-}'"';
const pick=want||(S.sessions.filter(s=>s.name!==S.current).sort((a,b)=>b.voice_bytes-a.voice_bytes)[0]||{}).name||S.current;
await select(pick); await new Promise(r=>setTimeout(r,1000));
const ls=document.getElementById("ls"), rows=[...ls.querySelectorAll("tbody tr")];
const k=rows.findIndex(r=>{ const it=items[+r.dataset.i]; return it&&it.kind==="chat"&&it.e.a; });
if(k>3) ls.scrollTop=rows[k-3].offsetTop-ls.querySelector("thead").offsetHeight;
return "session "+pick+": "+X.utterances.length+" utterances, "+X.events.length+" events, list from row "+Math.max(0,k-3);'
"$BIN" -w 1440 -t 4 ${TX[@]+"${TX[@]}"} -j "$FX_JS" "$BASE/forensics?demo=1" "$OUT" \
    forensics-recording='h1, #hd, #settings' forensics-session='.grid > .panel:first-child, #sum' \
    forensics-timeline='.col > .panel:nth-child(2)' forensics-events='.col > .panel:nth-child(3)'
