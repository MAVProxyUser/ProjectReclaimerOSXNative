// winid: the on-screen window ids of a process (for `screencapture -l<id>`: one window with its title bar, never the desktop).
//     clang -O2 -framework CoreGraphics -framework CoreFoundation -o shim/build/winid shim/tools/winid.c   (shim/tools/winshot.sh builds it)
//     winid <pid> [title substring]  ->  "<id> <width>x<height> <title>" per window, largest first
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <CoreGraphics/CoreGraphics.h>
#include <CoreFoundation/CoreFoundation.h>

int main(int argc, char **argv){
    if(argc<2){ fprintf(stderr,"usage: winid <pid> [title substring]\n"); return 2; }
    int pid=atoi(argv[1]); const char *want=argc>2?argv[2]:0;
    CFArrayRef a=CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly|kCGWindowListExcludeDesktopElements,kCGNullWindowID); if(!a) return 1;
    struct { long id; int w,h; char t[256]; } out[64]; int n=0;
    for(CFIndex i=0;i<CFArrayGetCount(a) && n<64;i++){ CFDictionaryRef d=CFArrayGetValueAtIndex(a,i); int p=0; long id=0, layer=1;
        CFNumberRef v=CFDictionaryGetValue(d,kCGWindowOwnerPID); if(!v || !CFNumberGetValue(v,kCFNumberIntType,&p) || p!=pid) continue;
        v=CFDictionaryGetValue(d,kCGWindowLayer); if(v) CFNumberGetValue(v,kCFNumberLongType,&layer); if(layer!=0) continue;   // normal windows only
        v=CFDictionaryGetValue(d,kCGWindowNumber); if(!v || !CFNumberGetValue(v,kCFNumberLongType,&id)) continue;
        CGRect r={{0,0},{0,0}}; CFDictionaryRef b=CFDictionaryGetValue(d,kCGWindowBounds); if(b) CGRectMakeWithDictionaryRepresentation(b,&r);
        char t[256]=""; CFStringRef s=CFDictionaryGetValue(d,kCGWindowName); if(s) CFStringGetCString(s,t,sizeof t,kCFStringEncodingUTF8);   // (empty without Screen Recording permission)
        if(want && !strstr(t,want)) continue;
        out[n].id=id; out[n].w=(int)r.size.width; out[n].h=(int)r.size.height; snprintf(out[n].t,sizeof out[n].t,"%s",t); n++; }
    CFRelease(a);
    for(int i=0;i<n;i++) for(int j=i+1;j<n;j++) if((long)out[j].w*out[j].h>(long)out[i].w*out[i].h){ __typeof__(out[0]) x=out[i]; out[i]=out[j]; out[j]=x; }
    for(int i=0;i<n;i++) printf("%ld %dx%d %s\n",out[i].id,out[i].w,out[i].h,out[i].t);
    return n?0:1; }
