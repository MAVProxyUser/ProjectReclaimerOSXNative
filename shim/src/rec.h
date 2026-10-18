// rec.h -- clip recording (rec.m): the C interface peload.c (present thread, audio callbacks, stats server) and
// shim/tools/rec-test.m use. Design, formats and limits: shim/docs/recording.md.
#ifndef RECLAIMER_REC_H
#define RECLAIMER_REC_H
#include <stddef.h>
#include <stdint.h>
// Off = these two flags only: the present paths call rec_video_frame while g_rec_video is set (a video recording, or
// the end of one), the audio output callbacks call rec_audio_push while g_rec_audio is set.
extern volatile int g_rec_video, g_rec_audio;
// Start a recording (any thread). audio_only: 0 = picture + sound -> .mp4 (H.264 + AAC), 1 = sound -> .m4a (AAC).
// secs: its length (1..600 s; it stops by itself). name: the file name's first part (the map; sanitised).
// vw x vh: the size of the game's picture in the window (video; 0 = no window to record). Writes
// ~/Movies/Reclaimer/<name>_<YYYYmmdd-HHMMSS>.mp4|.m4a. Returns 1, or 0 with a reason in err.
int  rec_start(int audio_only, double secs, const char* name, int vw, int vh, char* err, size_t errn);
void rec_stop(void);                       // finish early (any thread); the file is kept
// Present thread, its GL context current, right after the window's frame (+ layered windows + overlay) is drawn and
// before the flush: copies rect x,y,w,h of framebuffer srcfbo (0 = the window) into the next video frame.
void rec_video_frame(unsigned srcfbo, int x, int y, int w, int h);
// An audio output callback: frames of interleaved PCM (bits 8/16/24/32 int, or 32 float; blk = bytes per frame) at
// rate Hz from source key (one key per stream). Mixed into the recording's 48 kHz stereo sound; never blocks long.
void rec_audio_push(const void* key, const void* data, uint32_t frames, int ch, int rate, int bits, int blk, int isfloat);
// {"state":"idle|recording|finishing", ...} for the stats snapshot (video_ok: a window to record exists)
int  rec_status_json(char* buf, size_t n, int video_ok);
char* rec_list_json(size_t* len);          // GET /recordings.json: the folder's clips, newest first (malloc'd)
int  rec_file_path(const char* name, char* path, size_t n);   // a listed clip's name -> its path (0: not one)
#endif
