#!/bin/bash
# Offline test of the object_get_origin guard (backlog 60; FIXES.md section 17), no game: extracts the REAL guard code from
# shim/src/peload.c (originguard_one / originguard_install), runs it on halo3's own object_get_origin bytes (read from your
# MCC halo3.dll) with a stand-in object table, and checks:
#   the unguarded function and the guarded one give the same origin for a live object; an empty object slot and an empty
#   parent slot give (0,0,0) instead of a fault; r11 survives the call (halo3+0x57ab3 keeps the object handle there across
#   it -- the first guard jumped through r11 and crashed the next call); other bytes at a site -> nothing installed.
#   bash shim/tools/originguard-test.sh [path to halo3.dll]
set -e
cd "$(dirname "$0")/.."
DLL="${1:-$HOME/Library/Application Support/ReclaimerMac/prefix/drive_c/Program Files/Steam/steamapps/common/Halo The Master Chief Collection/halo3/halo3.dll}"
[ -f "$DLL" ] || { echo "halo3.dll not found: $DLL"; exit 1; }
T=$(mktemp -d "${TMPDIR:-/tmp}/ogtest.XXXXXX"); trap 'rm -rf "$T"' EXIT
# object_get_origin = halo3+0x3451b4 .. 0x34523a
python3 - "$DLL" "$T/fn.bin" <<'EOF'
import sys,struct
d=open(sys.argv[1],'rb').read(); pe=struct.unpack_from('<I',d,0x3c)[0]
nsec=struct.unpack_from('<H',d,pe+6)[0]; opt=struct.unpack_from('<H',d,pe+20)[0]; sec=pe+24+opt
for i in range(nsec):
    vs,va,rs,rp=struct.unpack_from('<IIII',d,sec+40*i+8)
    if va<=0x3451b4<va+vs: off=rp+0x3451b4-va; open(sys.argv[2],'wb').write(d[off:off+0x86]); break
EOF
cut_block(){ awk -v s="$2" -v e="$3" 'index($0,s)==1{on=1} on && index($0,e)==1 && !(index($0,s)==1){exit} on{print}' "$1"; }
{
cat <<'EOF'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/mman.h>
#include <libkern/OSCacheControl.h>
static uint8_t*g_hookpool; static uint64_t g_hookoff;
EOF
cut_block src/peload.c '// ---- object_get_origin on a deleted object (backlog 60)' '// ---- '   # up to the next section
cat <<'EOF'
typedef uint64_t (__attribute__((ms_abi)) *thunk_t)(void*table,void*out);
static int fails;
static void check(const char*what,int ok){ printf("  %-58s %s\n",what,ok?"ok":"FAIL"); if(!ok) fails++; }
int main(int argc,char**argv){
    FILE*f=fopen(argv[1],"rb"); uint8_t fn[0x86]; if(!f||fread(fn,1,sizeof fn,f)!=sizeof fn){ puts("can't read the function bytes"); return 1; } fclose(f);
    // halo3's pages: 0x345000.. at a fake base; the guard's mprotect covers 0x3000 from the site's page
    uint8_t*img=mmap(0,0x4000,PROT_READ|PROT_WRITE|PROT_EXEC,MAP_PRIVATE|MAP_ANON,-1,0); uint8_t*base=img-0x345000;
    uint8_t*fnp=base+0x3451b4; memcpy(fnp,fn,sizeof fn);
    // the prologue up to 0x3451e3 reads the object table from TLS; a stand-in: r10 = out, r9 = our table, rax = index 0
    static const uint8_t pro[]={0x48,0x83,0xec,0x28, 0x4c,0x8b,0xd2, 0x4c,0x8b,0xc9, 0x31,0xc0};
    memset(fnp,0x90,0x3451e3-0x3451b4); memcpy(fnp,pro,sizeof pro);
    // thunk: mov r11,MAGIC; sub rsp,0x28; mov rax,fn; call rax; add rsp,0x28; mov rax,r11; ret  -> returns r11 after the call
    uint8_t*th=img+0x3000, *w=th; const uint64_t MAGIC=0x1122334455667788ull;
    *w++=0x49;*w++=0xBB; memcpy(w,&MAGIC,8); w+=8; *w++=0x48;*w++=0x83;*w++=0xEC;*w++=0x28;
    *w++=0x48;*w++=0xB8; uint64_t a=(uint64_t)fnp; memcpy(w,&a,8); w+=8; *w++=0xFF;*w++=0xD0;
    *w++=0x48;*w++=0x83;*w++=0xC4;*w++=0x28; *w++=0x4C;*w++=0x89;*w++=0xD8; *w++=0xC3;
    sys_icache_invalidate(img,0x4000);
    thunk_t call=(thunk_t)th;
    // table: 24-byte entries, the object pointer at +0x10; objects: parent index word at +0x10 (-1 = none), origin at +0x50
    static uint8_t table[4*24]; static uint8_t obj[0x200] __attribute__((aligned(16)));
    float org[3]={1.5f,-2.25f,3.0f}; memcpy(obj+0x50,org,12); *(int32_t*)(obj+0x10)=-1;
    float out[3];
    *(void**)(table+0x10)=obj; memset(out,0xAA,sizeof out); call(table,out);
    check("unguarded: live object -> its origin",!memcmp(out,org,12));
    // other bytes at a site: refused, nothing written
    uint8_t keep=fnp[0x3451e3-0x3451b4+1]; fnp[0x3451e3-0x3451b4+1]^=0xFF; uint8_t snap[0x86]; memcpy(snap,fnp,sizeof snap);
    originguard_one(base,0x3451e3,(const uint8_t[]){0x4d,0x8b,0x44,0xc1,0x10, 0x41,0x83,0x78,0x10,0xff, 0x49,0x8d,0x50,0x50},14,5,(const uint8_t[]){0x4d,0x85,0xc0},0x345232);
    check("other bytes at the site -> not installed",!memcmp(snap,fnp,sizeof snap)); fnp[0x3451e3-0x3451b4+1]=keep;
    originguard_install(base);
    check("both sites patched (jmp [rip+0])",fnp[0x2f]==0xFF&&fnp[0x30]==0x25&&fnp[0x5f]==0xFF&&fnp[0x60]==0x25);
    memset(out,0xAA,sizeof out); uint64_t r11=call(table,out);
    check("guarded: live object -> the same origin",!memcmp(out,org,12));
    check("guarded: r11 survives the call (live object)",r11==MAGIC);
    *(void**)(table+0x10)=0; memset(out,0xAA,sizeof out); r11=call(table,out);
    check("empty object slot -> (0,0,0), no fault",out[0]==0&&out[1]==0&&out[2]==0);
    check("guarded: r11 survives the call (empty slot)",r11==MAGIC);
    *(void**)(table+0x10)=obj; *(int32_t*)(obj+0x10)=1; obj[0x14]=0; *(void**)(table+24+0x10)=0;   // parent index 1, empty
    memset(out,0xAA,sizeof out); r11=call(table,out);
    check("empty parent slot -> (0,0,0), no fault",out[0]==0&&out[1]==0&&out[2]==0);
    check("guarded: r11 survives the call (empty parent)",r11==MAGIC);
    printf(fails?"%d FAILED\n":"all passed\n",fails); return fails!=0;
}
EOF
} > "$T/t.c"
clang -arch x86_64 -O1 -w -o "$T/t" "$T/t.c"
arch -x86_64 "$T/t" "$T/fn.bin"
