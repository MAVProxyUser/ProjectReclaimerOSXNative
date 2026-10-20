#!/bin/bash
# Offline test of the loader's Win32 clipboard (no game): extracts the REAL clipboard + GlobalAlloc code from shim/src/peload.c
# (from "// ---- Clipboard (2026-10-20) ----" to cocoa_make_gl_window), compiles it for x86_64 (it runs under Rosetta like the
# loader) with a few stubs and checks it against a PRIVATE named pasteboard (PELOAD_PASTEBOARD), so your own clipboard is
# never touched:
#   Global* blocks (zero fill, size, lock count, bad handles), open / close / empty rules (owner thread, ERROR_CLIPBOARD_NOT_OPEN,
#   another thread refused), copy = CF_UNICODETEXT -> the pasteboard at CloseClipboard (CRLF -> LF, non-ASCII), paste = the
#   pasteboard -> CF_UNICODETEXT / CF_TEXT blocks, registered formats, EnumClipboardFormats, the sequence number, and
#   PELOAD_NO_CLIPBOARD=1 (the old failing OpenClipboard).
#   bash shim/tools/clipboard-test.sh
set -e
cd "$(dirname "$0")/.."
T=$(mktemp -d "${TMPDIR:-/tmp}/cliptest.XXXXXX"); trap 'rm -rf "$T"' EXIT
cut_block(){ awk -v s="$2" -v e="$3" 'index($0,s)==1{on=1} on && index($0,e)==1 && !(index($0,s)==1){exit} on{print}' "$1"; }
{
cat <<'EOF'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include <pthread.h>
#include <objc/runtime.h>
#include <objc/message.h>
#define MSABI __attribute__((ms_abi))
extern void* objc_autoreleasePoolPush(void); extern void objc_autoreleasePoolPop(void*);
static uint32_t g_lasterr;
static void MSABI i_SetLastError(uint32_t e){ g_lasterr=e; }
static uint32_t MSABI i_GetCurrentThreadId(void){ return (uint32_t)pthread_mach_thread_np(pthread_self()); }
static id ocls(const char*n){ return (id)objc_getClass(n); }
static SEL osel(const char*n){ return sel_registerName(n); }
static id om0(id o,const char*s){ return ((id(*)(id,SEL))objc_msgSend)(o,osel(s)); }
EOF
cut_block src/peload.c '// ---- Clipboard (2026-10-20) ----' 'static CGLContextObj cocoa_make_gl_window'
cat <<'EOF'
static int fails;
#define CHECK(c) do{ if(!(c)){ printf("FAIL line %d: %s\n",__LINE__,#c); fails++; } }while(0)
static id nsstr(const char*u){ return ((id(*)(id,SEL,const char*))objc_msgSend)((id)ocls("NSString"),osel("stringWithUTF8String:"),u); }
static void pb_write(const char*u){ void*p=objc_autoreleasePoolPush(); cb_pb_put(nsstr(u)); objc_autoreleasePoolPop(p); }
static int pb_is(const char*u){ void*p=objc_autoreleasePoolPush(); id s=cb_pb_get(); const char*g=s?((const char*(*)(id,SEL))objc_msgSend)(s,osel("UTF8String")):0;
    int ok=g && !strcmp(g,u); if(!ok) printf("  pasteboard has \"%s\", wanted \"%s\"\n",g?g:"(none)",u); objc_autoreleasePoolPop(p); return ok; }
static void* put_w(const char*ascii_or_utf8){   // a CF_UNICODETEXT block built the way the client does (UTF-16 + NUL)
    void*pool=objc_autoreleasePoolPush(); id s=nsstr(ascii_or_utf8); unsigned long n=((unsigned long(*)(id,SEL))objc_msgSend)(s,osel("length"));
    void*h=i_GlobalAlloc(0x42/*GHND*/,(n+1)*2); uint16_t*w=i_GlobalLock(h); struct { unsigned long loc,len; } rg={0,n};
    ((void(*)(id,SEL,void*,typeof(rg)))objc_msgSend)(s,osel("getCharacters:range:"),w,rg); w[n]=0; i_GlobalUnlock(h); objc_autoreleasePoolPop(pool); return h; }
static int other_ok;
static void* other(void*a){ (void)a; other_ok=i_OpenClipboard(0); return 0; }
int main(int argc,char**argv){
    if(argc>1 && !strcmp(argv[1],"off")){ CHECK(!i_OpenClipboard(0)); printf("%s\n",fails?"FAILED":"ok (PELOAD_NO_CLIPBOARD)"); return fails!=0; }
    // Global* blocks
    uint8_t*h=i_GlobalAlloc(0x42,100); CHECK(h && i_GlobalSize(h)==100); int z=1; for(int i=0;i<100;i++) if(h[i]) z=0; CHECK(z);
    CHECK(i_GlobalLock(h)==h && i_GlobalLock(h)==h && i_GlobalUnlock(h)==1 && i_GlobalUnlock(h)==0 && g_lasterr==0);
    CHECK(i_GlobalFree(h)==0); static uint8_t junk[64] __attribute__((aligned(16))); CHECK(i_GlobalFree(junk+16)==junk+16 && g_lasterr==6);
    CHECK(i_GlobalSize(0)==0 && !i_GlobalLock(0));
    // open / close / empty rules
    CHECK(!i_CloseClipboard() && g_lasterr==1418); CHECK(!i_EmptyClipboard() && g_lasterr==1418);
    CHECK(i_OpenClipboard(0)==1 && i_OpenClipboard(0)==1);   // the owner may open it again
    pthread_t t; pthread_create(&t,0,other,0); pthread_join(t,0); CHECK(other_ok==0);
    // copy: a player ID with a CRLF and non-ASCII text
    uint32_t seq0=i_GetClipboardSequenceNumber();
    CHECK(i_EmptyClipboard()); void*b=put_w("0123456789abcdef\r\nplayer \xc3\xa9\xe2\x9c\x93"); CHECK(i_SetClipboardData(13,b)==b); CHECK(i_CloseClipboard());
    CHECK(pb_is("0123456789abcdef\nplayer \xc3\xa9\xe2\x9c\x93")); CHECK(i_GetClipboardSequenceNumber()!=seq0);
    // paste: another app's text
    pb_write("server 203.0.113.7:49178 \xc3\xbc"); CHECK(i_IsClipboardFormatAvailable(13) && i_IsClipboardFormatAvailable(1)); CHECK(!i_IsClipboardFormatAvailable(2));
    CHECK(!i_GetClipboardData(13) && g_lasterr==1418);   // not open
    CHECK(i_OpenClipboard(0));
    uint16_t*w=i_GetClipboardData(13); CHECK(w && i_GlobalSize(w)==27*2); CHECK(w && w[0]=='s' && w[24]==' ' && w[25]==0xfc && w[26]==0);
    char*a=i_GetClipboardData(1); CHECK(a && !strcmp(a,"server 203.0.113.7:49178 \xc3\xbc"));
    uint32_t html=i_RegisterClipboardFormatW((const uint16_t*)u"HTML Format"); CHECK(html>=0xC000 && html==i_RegisterClipboardFormatW((const uint16_t*)u"html format"));
    CHECK(i_EnumClipboardFormats(0)==13 && i_EnumClipboardFormats(13)==1 && i_EnumClipboardFormats(1)==7 && i_EnumClipboardFormats(7)==0);
    CHECK(i_CloseClipboard()); CHECK(pb_is("server 203.0.113.7:49178 \xc3\xbc"));   // reading leaves the pasteboard as it was
    // a private format lives in this process until the next EmptyClipboard; setting only it empties the pasteboard
    CHECK(i_OpenClipboard(0) && i_EmptyClipboard()); void*hb=i_GlobalAlloc(0x42,8); CHECK(i_SetClipboardData(html,hb)==hb); CHECK(i_GetClipboardData(html)==hb);
    CHECK(i_CountClipboardFormats()==1 && i_EnumClipboardFormats(0)==html); CHECK(i_CloseClipboard());
    CHECK(i_IsClipboardFormatAvailable(html) && !i_IsClipboardFormatAvailable(13));
    CHECK(i_OpenClipboard(0) && i_EmptyClipboard() && i_CloseClipboard()); CHECK(!i_IsClipboardFormatAvailable(html));
    void*p=objc_autoreleasePoolPush(); ((void(*)(id,SEL))objc_msgSend)(cb_pb(),osel("releaseGlobally")); objc_autoreleasePoolPop(p);
    printf("%s\n",fails?"FAILED":"ok (clipboard)"); return fails!=0; }
EOF
} > "$T/t.c"
clang -arch x86_64 -O1 -Wno-deprecated-declarations -framework AppKit -o "$T/t" "$T/t.c"
NAME="reclaimer-cliptest-$$"
PELOAD_PASTEBOARD="$NAME" arch -x86_64 "$T/t"
PELOAD_PASTEBOARD="$NAME" PELOAD_NO_CLIPBOARD=1 arch -x86_64 "$T/t" off
