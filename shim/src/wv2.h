// wv2.h -- WebView2 on WKWebView (webview2.m): the C interface peload.c uses. Design: webview2.m's header comment.
#ifndef RECLAIMER_WV2_H
#define RECLAIMER_WV2_H
#include <stdint.h>
void* wv2_resolve(const char* name);      // a WebView2Loader.dll export -> our implementation (NULL: not one we have)
int   wv2_active(void);                    // a WebView2 window exists in this process
int   wv2_owns(void* hwnd);                // that HWND hosts one (its geometry is tracked; GetWindowRect reports it)
void  wv2_pump(void);                      // main thread: hand macOS events to AppKit (the page gets keys and the mouse)
void  wv2_hwnd_changed(void* hwnd, int show);   // after SetWindowPos / MoveWindow / ShowWindow (show: 1 / 0, -1 = unchanged)
void  wv2_hwnd_style(void* hwnd, uint32_t exstyle);
void  wv2_activate(void* hwnd);            // SetForegroundWindow on it (takes activation only from our own processes)  // SetWindowLongPtrW(GWL_EXSTYLE): WS_EX_TRANSPARENT = clicks go through
// peload.c provides:
int   pl_hwnd_geom(void* hwnd, int* x, int* y, int* w, int* h);   // our window's Win32 screen rect (0: not ours)
void  pl_win2unix(const uint16_t* w, char* out, int n);           // a Windows path -> the prefix's Unix path
#endif
