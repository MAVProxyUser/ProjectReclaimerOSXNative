# Clip recording (stats page "Record", backlog 42)

Record what the game window shows, with the game's sound, as an `.mp4` -- or only the sound, as an `.m4a` -- for a
length set on the stats page. Code: `shim/src/rec.m` (Objective-C, compiled beside `peload.c` by `build.sh`; C interface
`shim/src/rec.h`), hooks in `peload.c` (present paths), `xaudio.c` and `wasapi.c` (audio output callbacks), controls and
routes in `stats.c`, the page row in `stats_page.html`. Offline test: `bash shim/tools/rec-test.sh`.

## Using it

The stats page (http://127.0.0.1:8765/) has a **Record** row under the overlay buttons: the length in seconds (1-600,
default 30, remembered by the browser), **Video + audio** or **Audio only**, **Record** / **Stop**. While it runs a
red `REC 0:12 / 0:30` counts up; it stops by itself after the length, Stop ends it early (the file keeps what was
recorded). **Recordings in ~/Movies/Reclaimer** lists the clips (newest first, length, size, date) with a download link
and a **play** link (opens in a browser tab).

Files: `~/Movies/Reclaimer/<map>_<YYYYmmdd-HHMMSS>.mp4` (or `.m4a`), `<map>` = the map being played (`mainmenu` in the
menu, `game` when unknown). The folder is made on first use; nothing is written anywhere else.

## Formats

| | |
|---|---|
| Video | H.264 High profile (`PELOAD_REC_HEVC=1`: HEVC), hardware encoder (VideoToolbox), no B-frames, BT.709 tagged |
| Size | the game's picture in the window (letterboxed rect, window pixels), scaled down to fit 1920x1080 if larger, even |
| Frame rate | up to 60 fps (`PELOAD_REC_FPS=<1..120>`), each frame stamped with its real present time (variable rate) |
| Bit rate | ~0.1 bit per pixel per frame: 1080p60 = 12.4 Mbit/s (~93 MB a minute); `PELOAD_REC_KBPS=<n>` overrides |
| Sound | AAC-LC, 48 kHz stereo, 192 kbit/s |
| Container | MPEG-4, written as fragments every 2 s (`PELOAD_REC_NOFRAG=1`: one movie box at the end) |
| Audio only | `.m4a` (AAC in MPEG-4), same sound |

The window has no Retina backing, so its drawable is its size in points: a 1280x720 window records 1280x720; make the
window bigger or full screen for a sharper clip (the game itself renders at its own 1920x1080).

## What is in it

- Picture: exactly what the window shows -- the game frame, the client's layered windows (loading screen, its screens
  while the game is not presenting) and the overlay (hitboxes, labels, zones) -- copied after they are drawn.
- Sound: the game's XAudio2 mix (`xaudio.c`, taken before `PELOAD_AUDIO_MUTE`) plus every WASAPI render stream of the
  client (`wasapi.c`: menu music, other players' voice chat), each converted to 48 kHz stereo float (8/16/24/32-bit int
  or float, any rate, mono to 7.1) and mixed.
- Not in it: your own microphone; the GDI-composite picture shown before the D3D11->GL renderer starts; anything while
  the present thread is not presenting (window hidden with `PELOAD_HIDE_WINDOW`, minimised, fully covered, display
  asleep -- the picture holds its last frame; a video recording that never got a frame gets one black frame so the
  file stays valid). With `PELOAD_HIDE_WINDOW` the page offers Audio only.

## How it works

**Video.** `peload.c` calls `rec_video_frame(0, rect)` from the three present paths (GL helper `pq_present_wait_glh`,
in-process `pq_present_wait`, the pre-queue `d3dgl_blit_fullscreen_i`) right after `lay_draw(); hb_draw_overlay();`,
inside the window context's lock, only while `g_rec_video` is set (`REC_FRAME()`; `rec_pic_rect` = the same letterbox
maths as the present paths). On the present thread `rec.m`:

1. polls the fences of earlier blits (`glClientWaitSync(.., 0)`, never waits) and hands each finished one to the writer
   queue; flushes the `CVOpenGLTextureCache` (without that the cache keeps every surface and the pool ran dry after 8
   frames);
2. skips the present if the 60 fps grid isn't due (frames up to 4 ms early count), or if 3 blits are still on the GPU,
   or 6 frames wait for the encoder (dropped, counted);
3. takes a `CVPixelBuffer` from an IOSurface-backed BGRA pool (at most 8 buffers), maps it as a `GL_TEXTURE_RECTANGLE`
   through `CVOpenGLTextureCache`, and `glBlitFramebuffer`s the window's picture rect (back buffer, read framebuffer 0)
   into it -- scaled, and flipped so the window's top row is the buffer's first row -- then detaches it, restores the
   read/draw framebuffers and the scissor test, drops a fence and flushes. No pixel crosses the CPU.

The writer queue (`g_rq`, one serial GCD queue for every `AVAssetWriter` call) appends the buffer through an
`AVAssetWriterInputPixelBufferAdaptor` (`expectsMediaDataInRealTime`), or drops it if the input isn't ready.

**Sound.** `xa_aq_cb` / `wa_aq_cb` call `rec_audio_push(key, pcm, frames, format)` while `g_rec_audio` is set. Each
source's samples go into one 4 s ring laid out on the recording's timeline: a source's first block lands at "now -
start", the following blocks right after it (continuous), re-anchored only when it drifts more than 125 ms from the
clock (a stalled or restarted stream). Other rates are resampled linearly with the phase carried between blocks. The
lock (`os_unfair_lock`) is held for the few microseconds of one block. A 20 ms timer on the writer queue takes the ring
up to 200 ms behind "now" (every source has written that part), zeroes it, and appends it as one `CMSampleBuffer`
(float32 stereo 48 kHz) to the AAC input -- silence where nothing played, so the sound spans the whole clip. An encoder
that isn't ready keeps its samples in the ring for the next tick.

**Timeline.** Video and sound share one clock (`CLOCK_MONOTONIC`, 0 = the start): a frame is stamped with the moment it
was drawn, a sound block with the moment its callback ran. The offline test records a white frame and a 2 kHz burst
made at the same instant and finds them 2 ms apart in the file. In the game, the output latency of the audio queue
(~20-30 ms) is not compensated.

**Start / stop.** `rec_start` (stats HTTP thread, `dispatch_sync` onto the queue; 0.2-0.6 s for the encoder to start)
refuses when a recording runs, the length is outside 1-600 s (clamped), there is no window for video, or the disk has
less than the clip's size + 256 MB free. The timer stops it at the length; `rec_stop` at once. Stopping: the present
thread hands on its last frames (waits up to 50 ms per blit, once), reports, and clears `g_rec_video` itself; the
sound is taken up to the stop time; then `markAsFinished`, `endSessionAtSourceTime(stop)`, `finishWriting` (async).
If the present thread doesn't answer within 2 s (window hidden), the file is finished without it.

**Fragments.** The `.mp4` is written as movie fragments every 2 s, so a game that crashes or is killed mid-clip leaves a
file that plays up to its last fragment (the test kills the recorder 4.5 s into a 10 s clip: 4.0 s playable). With
B-frames, finishing a fragmented file failed in about 1 of 8 test runs (AVFoundation -11800 / OSStatus -16341, the file
cut at its last fragment); without them 0 of 60 -- hence `AVVideoAllowFrameReorderingKey = NO`.

## Cost

Off: two flag tests (`g_rec_video` per present, `g_rec_audio` per audio callback); the 1.5 MB sound ring is untouched
BSS. AVFoundation, CoreMedia and CoreVideo are linked, but Cocoa already loads all three at startup (same 514 images
with or without them), so linking adds nothing there either. Recording: one scaled blit per recorded frame on the GPU,
a fence poll and a mutex per present, the encoder in VideoToolbox's own process, a few microseconds per audio callback.
Not measured in a live game yet.

## Endpoints

- `POST /control?do=rec&secs=<1-600>&mode=av|audio` (header `X-Reclaimer-Control: 1`): start. A refusal answers 400;
  the reason is the snapshot's `rec.last_error`.
- `POST /control?do=recstop`: stop now, keep the file.
- `GET /recordings.json`: `[{"file","size","duration","time","audio_only"}]`, newest first; the clip being written is
  left out. `duration` comes from the file's own boxes (`moov/mvhd`, or `mvex/mehd` when fragmented).
- `GET /recordings/<file>`: the clip (`video/mp4` / `audio/mp4`, `Content-Disposition: inline`), with Range requests
  (browsers seek with them when playing it in a tab). Names are checked: `[A-Za-z0-9_.-]`, `.mp4`/`.m4a`, no `..`, only
  files directly in `~/Movies/Reclaimer`.
- Snapshot (`/stats.json`, `/events`): `"rec":{"state":"idle|recording|finishing","mode":"av|audio","secs","elapsed",
  "left","file","width","height","frames","dropped","video_ok","dir","last_file","last_error"}`.

With `PELOAD_STATS_ADDR` set to a LAN address, anyone who can reach the page can start recordings and download clips,
as with the other controls (`PELOAD_STATS_READONLY=1` turns the controls off; downloads stay available).

## Diagnostics

- `[rec]` lines in the log: start (size, codec, bit rate), saved (length, frames, dropped, MB) or FAILED with the
  AVFoundation error, domain and code, and the underlying error.
- `PELOAD_REC_DEBUG=1`: why frames were dropped (GPU never finished, context change, GPU behind, encoder queue behind,
  no texture cache, pool empty, no texture, encoder not ready, append failed) and dropped sound.

## Test (offline, no game)

`bash shim/tools/rec-test.sh` builds `shim/tools/rec-test.m` with `rec.m` for x86_64, runs it under Rosetta into a
scratch `$HOME` (never your own Movies folder), and checks with ffprobe / ffmpeg (Homebrew): H.264 1920x1080 from a
2560x1440 "window", AAC 48 kHz stereo, lengths, 60 fps frame counts, the picture upright (top red, bottom blue), both
test tones (48 kHz float stereo and 44.1 kHz int16 mono resampled) and no noise at 3 kHz, picture/sound sync, the
folder list's lengths, a 12 s clip (ring wraps, pool recycling), audio only, a recording without any frame, an early
stop, and a recorder killed mid-clip. The window there is an FBO (read framebuffer = the FBO, not 0): blitting from
the real window's back buffer needs a live check.

## Not done

- **Instant replay** (keep the last N seconds, save on a click): it needs the encoder running all the time -- a cost
  while "off" -- and AVAssetWriter can't drop old samples, so it would take a VTCompressionSession, a ring of encoded
  samples and our own MPEG-4 muxing. Left for later.
- The microphone (your own voice) is not mixed in.
