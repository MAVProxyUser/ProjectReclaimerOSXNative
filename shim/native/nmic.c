// The app's microphone path (2026-11-08; the user: "add a soundboard in which I can select a wav and play it into the mic input
// path into game audio"). Included by nhook.c after its audio interposers (one compilation unit).
//
// NHOOK_AUDIO_TRACE=1: every CoreAudio device / AudioUnit call the app makes (property selectors as four-character codes,
// scope, element, the result) -- how the app looks for and opens a microphone. NHOOK_MUTE_OUTPUT=1: the app's sound output is
// zeroed (test clients that must look for a microphone without playing their sound on this Mac's speakers).
#include <mach/mach_time.h>
#include <dirent.h>
static int g_atr = -1;
static int atr(void) { if (g_atr < 0) g_atr = getenv("NHOOK_AUDIO_TRACE") != 0; return g_atr; }
static const char* fcc(uint32_t v, char* b) { for (int i = 0; i < 4; i++) { char c = (char)(v >> (24 - 8 * i)); b[i] = c >= 32 && c < 127 ? c : '.'; } b[4] = 0; return b; }

// ---- the soundboard microphone (2026-11-08) ------------------------------------------------------------------------------
// The app looks for a microphone once it is in an online session: AudioObjectGetPropertyData(system, kAudioHardwarePropertyDefault
// InputDevice) -- 0 on a Mac without one (a Mac Studio: "[voice] no capture (no microphone): listening only"), and it never sends
// voice. With no real input device, nhook answers with a VIRTUAL one (VMIC_DEV, "Reclaimer soundboard", 48 kHz mono float) and
// answers its property queries itself; an AUHAL unit the app points at it (kAudioOutputUnitProperty_CurrentDevice) is taken over:
// its settings are kept, Start runs a 10 ms clock thread that calls the app's input callback, and the app's AudioUnitRender(bus 1)
// gets the soundboard's samples (silence when nothing plays). The app treats it as its microphone: its Talk mode decides when
// it transmits (Hold / Toggle to talk with the talk key; Sensitivity sends by itself while a clip plays). With a REAL microphone
// the soundboard is mixed into its samples (my_AudioUnitRender, bus 1). NHOOK_VMIC=0 = no virtual microphone (the old "listening
// only"); NHOOK_VMIC=force = the virtual one even beside a real microphone.
#define VMIC_DEV    0x52434c4du   // 'RCLM'
#define VMIC_STREAM 0x52434c53u   // 'RCLS'
#define VMIC_RATE   48000.0
static volatile int g_vmic_given;          // the app was handed the virtual microphone
static volatile double g_vmic_t;           // the last time the app pulled samples from it
static int vmic_mode(void) { static int m = -1; if (m < 0) { const char* e = getenv("NHOOK_VMIC"); m = e && !strcmp(e, "0") ? 0 : e && !strcmp(e, "force") ? 2 : 1; } return m; }
static AudioStreamBasicDescription vmic_asbd(void) {
    AudioStreamBasicDescription d; memset(&d, 0, sizeof d); d.mSampleRate = VMIC_RATE; d.mFormatID = kAudioFormatLinearPCM;
    d.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked; d.mBytesPerPacket = 4; d.mFramesPerPacket = 1; d.mBytesPerFrame = 4;
    d.mChannelsPerFrame = 1; d.mBitsPerChannel = 32; return d; }
// a property of the virtual device / stream: its size (out == 0) or its value; kAudioHardwareUnknownPropertyError otherwise
static OSStatus vmic_prop(AudioObjectID o, const AudioObjectPropertyAddress* a, UInt32* sz, void* out) {
    int dev = o == VMIC_DEV, inp = a->mScope != kAudioObjectPropertyScopeOutput; uint8_t v[64]; UInt32 n = 0; CFStringRef s = 0;
    switch (a->mSelector) {
    case kAudioObjectPropertyName: s = CFSTR("Reclaimer soundboard"); break;
    case kAudioObjectPropertyManufacturer: s = CFSTR("ReclaimerMac"); break;
    case kAudioDevicePropertyDeviceUID: s = CFSTR("reclaimer-soundboard"); break;
    case kAudioDevicePropertyModelUID: s = CFSTR("reclaimer-soundboard-model"); break;
    case kAudioObjectPropertyClass: *(UInt32*)v = dev ? kAudioDeviceClassID : kAudioStreamClassID; n = 4; break;
    case kAudioObjectPropertyBaseClass: *(UInt32*)v = kAudioObjectClassID; n = 4; break;
    case kAudioObjectPropertyOwner: *(UInt32*)v = dev ? kAudioObjectSystemObject : VMIC_DEV; n = 4; break;
    case kAudioDevicePropertyTransportType: *(UInt32*)v = kAudioDeviceTransportTypeVirtual; n = 4; break;
    case kAudioDevicePropertyDeviceIsAlive: case kAudioDevicePropertyDeviceCanBeDefaultDevice: *(UInt32*)v = 1; n = 4; break;
    case kAudioDevicePropertyDeviceIsRunningSomewhere: case kAudioDevicePropertyIsHidden: case kAudioDevicePropertyMute:
    case kAudioDevicePropertyLatency: case kAudioDevicePropertySafetyOffset: case kAudioDevicePropertyClockDomain: *(UInt32*)v = 0; n = 4; break;
    case kAudioDevicePropertyHogMode: *(pid_t*)v = -1; n = sizeof(pid_t); break;
    case kAudioDevicePropertyNominalSampleRate: *(Float64*)v = VMIC_RATE; n = 8; break;
    case kAudioDevicePropertyAvailableNominalSampleRates: { AudioValueRange r = {VMIC_RATE, VMIC_RATE}; memcpy(v, &r, sizeof r); n = sizeof r; break; }
    case kAudioDevicePropertyBufferFrameSize: *(UInt32*)v = 480; n = 4; break;
    case kAudioDevicePropertyBufferFrameSizeRange: { AudioValueRange r = {64, 4096}; memcpy(v, &r, sizeof r); n = sizeof r; break; }
    case kAudioDevicePropertyPreferredChannelsForStereo: ((UInt32*)v)[0] = 1; ((UInt32*)v)[1] = 1; n = 8; break;
    case kAudioDevicePropertyStreamConfiguration: { AudioBufferList* l = (AudioBufferList*)v; memset(v, 0, sizeof(AudioBufferList));
        if (inp) { l->mNumberBuffers = 1; l->mBuffers[0].mNumberChannels = 1; } n = inp ? (UInt32)sizeof(AudioBufferList) : (UInt32)offsetof(AudioBufferList, mBuffers); break; }
    case kAudioDevicePropertyStreams: if (inp && dev) { *(UInt32*)v = VMIC_STREAM; n = 4; } else n = 0; break;
    case kAudioStreamPropertyDirection: *(UInt32*)v = 1; n = 4; break;
    case kAudioStreamPropertyStartingChannel: *(UInt32*)v = 1; n = 4; break;
    case kAudioStreamPropertyTerminalType: *(UInt32*)v = kAudioStreamTerminalTypeMicrophone; n = 4; break;
    case kAudioStreamPropertyVirtualFormat: case kAudioStreamPropertyPhysicalFormat: { AudioStreamBasicDescription d = vmic_asbd(); memcpy(v, &d, sizeof d); n = sizeof d; break; }
    case kAudioStreamPropertyAvailableVirtualFormats: case kAudioStreamPropertyAvailablePhysicalFormats: {
        AudioStreamRangedDescription r; memset(&r, 0, sizeof r); r.mFormat = vmic_asbd(); r.mSampleRateRange.mMinimum = r.mSampleRateRange.mMaximum = VMIC_RATE; memcpy(v, &r, sizeof r); n = sizeof r; break; }
    default: return kAudioHardwareUnknownPropertyError; }
    if (s) n = sizeof(CFStringRef);
    if (!out) { if (sz) *sz = n; return noErr; }
    if (!sz || *sz < n) return kAudioHardwareBadPropertySizeError;
    if (s) *(CFStringRef*)out = (CFStringRef)CFRetain(s); else memcpy(out, v, n);   // (a CFString the caller releases)
    *sz = n; return noErr; }
static int vmic_no_real_input(void) {   // no real default input device (asked of the real HAL)
    AudioObjectPropertyAddress a = {kAudioHardwarePropertyDefaultInputDevice, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    AudioDeviceID d = 0; UInt32 sz = sizeof d; return AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, 0, &sz, &d) || d == kAudioObjectUnknown; }

// the AUHAL units the app pointed at the virtual microphone (emulated: the real unit stays unconfigured)
typedef struct { AudioUnit au; AURenderCallbackStruct cb; AudioStreamBasicDescription fmt; int have_fmt; volatile int run; pthread_t th; } VmicUnit;
static VmicUnit g_vu[8]; static int g_nvu; static pthread_mutex_t g_vu_mx = PTHREAD_MUTEX_INITIALIZER;
static VmicUnit* vmic_unit(AudioUnit au) { for (int i = 0; i < g_nvu; i++) if (g_vu[i].au == au) return &g_vu[i]; return 0; }
// a unit's client format on its input bus (the app's own setting; for a real microphone too): what AudioUnitRender(bus 1) fills
typedef struct { AudioUnit au; AudioStreamBasicDescription f; } MicFmt; static MicFmt g_mfmt[8]; static int g_nmfmt;
static const AudioStreamBasicDescription* mic_fmt(AudioUnit au) { for (int i = 0; i < g_nmfmt; i++) if (g_mfmt[i].au == au) return &g_mfmt[i].f; return 0; }

// ---- the soundboard: a clip decoded to 48 kHz mono float (ExtAudioFile: WAV, AIFF, CAF, MP3, M4A ...), played into the microphone
static float* g_sb_clip; static size_t g_sb_len; static volatile size_t g_sb_pos; static volatile int g_sb_play; static float g_sb_vol = 1.0f;
static char g_sb_name[160]; static pthread_mutex_t g_sb_mx = PTHREAD_MUTEX_INITIALIZER; static volatile unsigned long g_sb_plays;
static void sb_dir(char* out, size_t n) { const char* d = getenv("NHOOK_SOUNDBOARD_DIR"); const char* h = getenv("HOME");
    if (d && *d) snprintf(out, n, "%s", d); else snprintf(out, n, "%s/Library/Application Support/ReclaimerMac/soundboard", h ? h : "/tmp"); mkdir(out, 0755); }
static int sb_name_ok(const char* s) { size_t L = strlen(s); if (!L || L > 120 || s[0] == '.') return 0;
    for (size_t i = 0; i < L; i++) { unsigned char c = (unsigned char)s[i]; if (c < 32 || c == '/' || c == '\\' || c == ':') return 0; } return !strstr(s, ".."); }
static int sb_ext_ok(const char* s) { const char* e = strrchr(s, '.'); if (!e) return 0; static const char* const ok[] = {".wav", ".wave", ".aif", ".aiff", ".caf", ".mp3", ".m4a", ".aac", ".flac"};
    for (size_t i = 0; i < sizeof ok / sizeof ok[0]; i++) if (!strcasecmp(e, ok[i])) return 1; return 0; }
static float* sb_decode(const char* path, size_t* len) {   // the whole file as 48 kHz mono float (max 10 minutes)
    CFURLRef u = CFURLCreateFromFileSystemRepresentation(0, (const UInt8*)path, (CFIndex)strlen(path), false); ExtAudioFileRef f = 0;
    OSStatus r = u ? ExtAudioFileOpenURL(u, &f) : -1; if (u) CFRelease(u); if (r || !f) return 0;
    AudioStreamBasicDescription c = vmic_asbd(); if (ExtAudioFileSetProperty(f, kExtAudioFileProperty_ClientDataFormat, sizeof c, &c)) { ExtAudioFileDispose(f); return 0; }
    size_t cap = 48000 * 10, n = 0, max = 48000 * 600; float* buf = malloc(cap * 4);
    while (buf) { if (n + 4096 > cap) { if (cap >= max) break; cap *= 2; float* nb = realloc(buf, cap * 4); if (!nb) break; buf = nb; }
        AudioBufferList l; l.mNumberBuffers = 1; l.mBuffers[0].mNumberChannels = 1; l.mBuffers[0].mDataByteSize = 4096 * 4; l.mBuffers[0].mData = buf + n;
        UInt32 fr = 4096; if (ExtAudioFileRead(f, &fr, &l) || !fr) break; n += fr; }
    ExtAudioFileDispose(f); if (!n) { free(buf); return 0; } *len = n; return buf; }
static int sb_play(const char* name) {
    if (!sb_name_ok(name) || !sb_ext_ok(name)) return 0; char d[1024], p[1200]; sb_dir(d, sizeof d); snprintf(p, sizeof p, "%s/%s", d, name);
    size_t n = 0; float* c = sb_decode(p, &n); if (!c) { nlog("[nhook] [soundboard] %s: not a sound file this Mac can read", name); return 0; }
    pthread_mutex_lock(&g_sb_mx); float* old = g_sb_clip; g_sb_clip = c; g_sb_len = n; g_sb_pos = 0; snprintf(g_sb_name, sizeof g_sb_name, "%s", name); g_sb_play = 1; g_sb_plays++;
    pthread_mutex_unlock(&g_sb_mx); free(old);
    nlog("[nhook] [soundboard] playing %s (%.1f s) into the %s microphone", name, n / VMIC_RATE, g_vmic_given ? "virtual" : "app's"); return 1; }
static void sb_stop(void) { g_sb_play = 0; }
// the next `frames` samples at `rate` into out (added when mix, else written); 0 = nothing playing (out untouched when mix)
static int sb_take(float* out, UInt32 frames, double rate, int mix) {
    if (!g_sb_play) { if (!mix) memset(out, 0, frames * 4); return 0; }
    pthread_mutex_lock(&g_sb_mx); const float* c = g_sb_clip; size_t L = g_sb_len; double step = VMIC_RATE / (rate > 1000 ? rate : VMIC_RATE), pos = (double)g_sb_pos; float vol = g_sb_vol;
    for (UInt32 i = 0; i < frames; i++) { size_t k = (size_t)pos; float s = c && k < L ? c[k] * vol : 0; if (mix) { float m = out[i] + s; out[i] = m > 1 ? 1 : m < -1 ? -1 : m; } else out[i] = s; pos += step; }
    g_sb_pos = (size_t)pos; if (!c || g_sb_pos >= L) g_sb_play = 0; pthread_mutex_unlock(&g_sb_mx); return 1; }
// fill / mix an AudioBufferList in the app's client format (float or 16 / 32-bit int; interleaved or not; any channel count)
static void sb_into(AudioBufferList* io, UInt32 frames, const AudioStreamBasicDescription* f, int mix) {
    if (!io || !io->mNumberBuffers || frames > 16384) return; static __thread float tmp[16384]; double rate = f ? f->mSampleRate : VMIC_RATE;
    int playing = sb_take(tmp, frames, rate, 0); if (mix && !playing) return;
    int isf = !f || (f->mFormatFlags & kAudioFormatFlagIsFloat), bits = f ? (int)f->mBitsPerChannel : 32, nonint = f && (f->mFormatFlags & kAudioFormatFlagIsNonInterleaved);
    int ch = nonint ? 1 : (f ? (int)f->mChannelsPerFrame : 1); if (ch < 1) ch = 1;
    for (UInt32 b = 0; b < io->mNumberBuffers; b++) { void* d = io->mBuffers[b].mData; if (!d) continue; UInt32 cap = io->mBuffers[b].mDataByteSize;
        for (UInt32 i = 0; i < frames; i++) for (int c = 0; c < ch; c++) { size_t k = (size_t)i * ch + c; float s = tmp[i];
            if (isf && bits == 32) { if ((k + 1) * 4 > cap) break; float* p = (float*)d + k; float m = mix ? *p + s : s; *p = m > 1 ? 1 : m < -1 ? -1 : m; }
            else if (!isf && bits == 16) { if ((k + 1) * 2 > cap) break; int16_t* p = (int16_t*)d + k; float m = (mix ? *p / 32768.0f : 0) + s; *p = (int16_t)(m > 1 ? 32767 : m < -1 ? -32768 : m * 32767); }
            else if (!isf && bits == 32) { if ((k + 1) * 4 > cap) break; int32_t* p = (int32_t*)d + k; double m = (mix ? *p / 2147483648.0 : 0) + s; *p = (int32_t)(m > 1 ? 2147483647.0 : m < -1 ? -2147483648.0 : m * 2147483647.0); } } } }
// the clock of an emulated unit: the app's input callback every 10 ms, like a device's IO thread
static void* vmic_clock(void* p) {
    VmicUnit* u = p; pthread_setname_np("nhook-vmic"); AudioTimeStamp ts; memset(&ts, 0, sizeof ts); ts.mFlags = kAudioTimeStampSampleTimeValid | kAudioTimeStampHostTimeValid;
    const UInt32 fr = 480; mach_timebase_info_data_t tb; mach_timebase_info(&tb); uint64_t per = (uint64_t)(10e6 * tb.denom / tb.numer), next = mach_absolute_time();
    while (u->run) { next += per; mach_wait_until(next); if (!u->run) break; ts.mHostTime = mach_absolute_time();
        AudioUnitRenderActionFlags fl = 0; uint64_t t0 = hc_now(); if (u->cb.inputProc) u->cb.inputProc(u->cb.inputProcRefCon, &fl, &ts, 1, fr, 0); sm_add(SM_VMIC, t0); ts.mSampleTime += fr;
        if (mach_absolute_time() > next + per * 20) next = mach_absolute_time(); }   // (a stall: no catching up)
    return 0; }
// my_AudioUnitSetProperty / GetProperty / Initialize / Start / Stop / Render / Dispose: 1 = an emulated unit's call, *r its result
static int vmic_set(AudioUnit au, AudioUnitPropertyID id, AudioUnitScope sc, AudioUnitElement el, const void* data, UInt32 sz, OSStatus* r) {
    if (id == kAudioUnitProperty_StreamFormat && sc == kAudioUnitScope_Output && el == 1 && data && sz >= sizeof(AudioStreamBasicDescription)) {   // a capture unit's client format
        pthread_mutex_lock(&g_vu_mx); int i = 0; for (; i < g_nmfmt; i++) if (g_mfmt[i].au == au) break; if (i < 8) { g_mfmt[i].au = au; memcpy(&g_mfmt[i].f, data, sizeof g_mfmt[i].f); if (i == g_nmfmt) g_nmfmt++; } pthread_mutex_unlock(&g_vu_mx); }
    if (id == kAudioOutputUnitProperty_CurrentDevice && data && sz >= 4 && *(const AudioDeviceID*)data == VMIC_DEV) {
        pthread_mutex_lock(&g_vu_mx); VmicUnit* u = vmic_unit(au); if (!u && g_nvu < 8) { u = &g_vu[g_nvu++]; memset(u, 0, sizeof *u); u->au = au; } pthread_mutex_unlock(&g_vu_mx);
        nlog("[nhook] [vmic] the app opened the soundboard microphone (unit %p)", (void*)au); *r = u ? noErr : kAudioUnitErr_InvalidProperty; return 1; }
    VmicUnit* u = vmic_unit(au); if (!u) return 0;
    if (id == kAudioOutputUnitProperty_SetInputCallback && data && sz >= sizeof(AURenderCallbackStruct)) memcpy(&u->cb, data, sizeof u->cb);
    if (id == kAudioUnitProperty_StreamFormat && data && sz >= sizeof(AudioStreamBasicDescription) && sc == kAudioUnitScope_Output) { memcpy(&u->fmt, data, sizeof u->fmt); u->have_fmt = 1; }
    if (atr()) nlog("[nhook] [vmic] set %u scope %u el %u (%u bytes)", (unsigned)id, (unsigned)sc, (unsigned)el, (unsigned)sz);
    *r = noErr; return 1; }
static int vmic_get(AudioUnit au, AudioUnitPropertyID id, AudioUnitScope sc, AudioUnitElement el, void* data, UInt32* sz, OSStatus* r) {
    VmicUnit* u = vmic_unit(au); if (!u) return 0; uint8_t v[64]; UInt32 n = 0;
    switch (id) {
    case kAudioUnitProperty_StreamFormat: { AudioStreamBasicDescription d = sc == kAudioUnitScope_Output && u->have_fmt ? u->fmt : vmic_asbd(); memcpy(v, &d, sizeof d); n = sizeof d; break; }
    case kAudioOutputUnitProperty_CurrentDevice: *(AudioDeviceID*)v = VMIC_DEV; n = 4; break;
    case kAudioOutputUnitProperty_HasIO: case kAudioOutputUnitProperty_EnableIO: *(UInt32*)v = sc == kAudioUnitScope_Input && el == 1 ? 1 : 0; n = 4; break;
    case kAudioUnitProperty_MaximumFramesPerSlice: *(UInt32*)v = 4096; n = 4; break;
    case kAudioUnitProperty_SampleRate: *(Float64*)v = VMIC_RATE; n = 8; break;
    case kAudioUnitProperty_Latency: *(Float64*)v = 0; n = 8; break;
    case kAudioDevicePropertyBufferFrameSize: *(UInt32*)v = 480; n = 4; break;
    case kAudioOutputUnitProperty_IsRunning: *(UInt32*)v = u->run ? 1 : 0; n = 4; break;
    default:   // a device property asked through the unit (cpal: kAudioDevicePropertyBufferFrameSizeRange 'fsz#'): the device's answer
        if (id >= 0x20202020u) { AudioObjectPropertyAddress pa = {id, kAudioObjectPropertyScopeInput, kAudioObjectPropertyElementMain}; UInt32 k = sizeof v;
            if (!vmic_prop(VMIC_DEV, &pa, &k, v)) { n = k; break; } }
        if (atr()) nlog("[nhook] [vmic] get %u scope %u el %u: not known", (unsigned)id, (unsigned)sc, (unsigned)el); *r = kAudioUnitErr_InvalidProperty; return 1; }
    if (!sz || !data || *sz < n) { *r = kAudioUnitErr_InvalidPropertyValue; return 1; } memcpy(data, v, n); *sz = n; *r = noErr; return 1; }
static int vmic_start(AudioUnit au, int start, OSStatus* r) {
    VmicUnit* u = vmic_unit(au); if (!u) return 0;
    if (start && !u->run) { u->run = 1; pthread_attr_t at; pthread_attr_init(&at); pthread_attr_set_qos_class_np(&at, QOS_CLASS_USER_INTERACTIVE, 0);
        if (pthread_create(&u->th, &at, vmic_clock, u)) u->run = 0; pthread_attr_destroy(&at); nlog("[nhook] [vmic] capture started (unit %p)", (void*)au); }
    if (!start && u->run) { u->run = 0; pthread_join(u->th, 0); nlog("[nhook] [vmic] capture stopped (unit %p)", (void*)au); }
    *r = noErr; return 1; }
static int vmic_render(AudioUnit au, UInt32 bus, UInt32 frames, AudioBufferList* io, OSStatus* r) {
    VmicUnit* u = vmic_unit(au); if (!u || bus != 1) return 0; g_vmic_t = mono_s(); { HK_ONCE(h, "soundboard microphone"); hk_fire(h); }
    sb_into(io, frames, u->have_fmt ? &u->fmt : 0, 0); *r = noErr; return 1; }

// the interposers (the trace ones above, extended)
static OSStatus vmic_GetPropertyData(AudioObjectID o, const AudioObjectPropertyAddress* a, UInt32 qs, const void* q, UInt32* sz, void* out, int* handled) {
    *handled = 0; if (!a || !vmic_mode()) return 0;
    if (o == VMIC_DEV || o == VMIC_STREAM) { *handled = 1; return vmic_prop(o, a, sz, out); }
    if (o == kAudioObjectSystemObject && a->mSelector == kAudioHardwarePropertyDefaultInputDevice && out && sz && *sz >= 4) {
        if (vmic_mode() == 2 || vmic_no_real_input()) { *handled = 1; *(AudioDeviceID*)out = VMIC_DEV; *sz = 4;
            if (!g_vmic_given) { g_vmic_given = 1; nlog("[nhook] [vmic] no microphone on this Mac: the app gets the soundboard microphone (NHOOK_VMIC=0 = none)"); }
            { int h = hk_named("soundboard microphone"); if (g_hk[h].state != HK_OK) hk_state(h, HK_OK, 0, "the app's default input is the virtual soundboard microphone"); }
            return noErr; } }
    (void)qs; (void)q; return 0; }

static OSStatus my_AudioObjectGetPropertyData(AudioObjectID o, const AudioObjectPropertyAddress* a, UInt32 qs, const void* q, UInt32* sz, void* out) {
    UInt32 in = sz ? *sz : 0; int h = 0; OSStatus r = vmic_GetPropertyData(o, a, qs, q, sz, out, &h); if (!h) r = AudioObjectGetPropertyData(o, a, qs, q, sz, out);
    if (atr() && a) { char s[5], c[5]; uint32_t v = out && sz && *sz >= 4 ? *(uint32_t*)out : 0;
        nlog("[nhook] [audio] GetPropertyData obj %u %s scope %s el %u size %u -> %d (size %u, first word 0x%x)%s", (unsigned)o, fcc(a->mSelector, s), fcc(a->mScope, c), (unsigned)a->mElement, (unsigned)in, (int)r, sz ? (unsigned)*sz : 0, v, h ? " [vmic]" : ""); }
    return r; }
static OSStatus my_AudioObjectGetPropertyDataSize(AudioObjectID o, const AudioObjectPropertyAddress* a, UInt32 qs, const void* q, UInt32* sz) {
    int h = vmic_mode() && a && (o == VMIC_DEV || o == VMIC_STREAM); OSStatus r = h ? vmic_prop(o, a, sz, 0) : AudioObjectGetPropertyDataSize(o, a, qs, q, sz);
    if (atr() && a) { char s[5], c[5]; nlog("[nhook] [audio] GetPropertyDataSize obj %u %s scope %s el %u -> %d (size %u)%s", (unsigned)o, fcc(a->mSelector, s), fcc(a->mScope, c), (unsigned)a->mElement, (int)r, sz ? (unsigned)*sz : 0, h ? " [vmic]" : ""); }
    return r; }
static OSStatus my_AudioObjectSetPropertyData(AudioObjectID o, const AudioObjectPropertyAddress* a, UInt32 qs, const void* q, UInt32 sz, const void* d) {
    if (vmic_mode() && (o == VMIC_DEV || o == VMIC_STREAM)) { if (atr() && a) { char s[5]; nlog("[nhook] [vmic] device set %s (%u bytes): accepted", fcc(a->mSelector, s), (unsigned)sz); } return noErr; }
    return AudioObjectSetPropertyData(o, a, qs, q, sz, d); }
static OSStatus my_AudioObjectAddPropertyListener(AudioObjectID o, const AudioObjectPropertyAddress* a, AudioObjectPropertyListenerProc p, void* d) {
    OSStatus r = vmic_mode() && (o == VMIC_DEV || o == VMIC_STREAM) ? noErr : AudioObjectAddPropertyListener(o, a, p, d);
    if (atr() && a) { char s[5], c[5]; nlog("[nhook] [audio] AddPropertyListener obj %u %s scope %s -> %d", (unsigned)o, fcc(a->mSelector, s), fcc(a->mScope, c), (int)r); }
    return r; }
static OSStatus my_AudioObjectRemovePropertyListener(AudioObjectID o, const AudioObjectPropertyAddress* a, AudioObjectPropertyListenerProc p, void* d) {
    return vmic_mode() && (o == VMIC_DEV || o == VMIC_STREAM) ? noErr : AudioObjectRemovePropertyListener(o, a, p, d); }
static OSStatus my_AudioComponentInstanceNew(AudioComponent c, AudioComponentInstance* out) {
    OSStatus r = AudioComponentInstanceNew(c, out);
    if (atr()) { AudioComponentDescription d; memset(&d, 0, sizeof d); AudioComponentGetDescription(c, &d); char t[5], s[5];
        nlog("[nhook] [audio] AudioComponentInstanceNew %s/%s -> %d unit %p", fcc(d.componentType, t), fcc(d.componentSubType, s), (int)r, out ? (void*)*out : 0); }
    return r; }
static OSStatus my_AudioComponentInstanceDispose(AudioComponentInstance au) {
    OSStatus r0; vmic_start(au, 0, &r0);   // (an emulated unit: its clock stops first)
    pthread_mutex_lock(&g_vu_mx); for (int i = 0; i < g_nvu; i++) if (g_vu[i].au == au) { g_vu[i] = g_vu[--g_nvu]; break; }
    for (int i = 0; i < g_nmfmt; i++) if (g_mfmt[i].au == au) { g_mfmt[i] = g_mfmt[--g_nmfmt]; break; } pthread_mutex_unlock(&g_vu_mx);
    return AudioComponentInstanceDispose(au); }
static OSStatus my_AudioUnitGetProperty(AudioUnit au, AudioUnitPropertyID id, AudioUnitScope sc, AudioUnitElement el, void* data, UInt32* sz) {
    OSStatus r; if (vmic_get(au, id, sc, el, data, sz, &r)) return r; r = AudioUnitGetProperty(au, id, sc, el, data, sz);
    if (atr()) nlog("[nhook] [audio] AudioUnitGetProperty %p id %u scope %u el %u -> %d", (void*)au, (unsigned)id, (unsigned)sc, (unsigned)el, (int)r);
    return r; }
static OSStatus my_AudioUnitInitialize(AudioUnit au) { if (vmic_unit(au)) return noErr; OSStatus r = AudioUnitInitialize(au); if (atr()) nlog("[nhook] [audio] AudioUnitInitialize %p -> %d", (void*)au, (int)r); return r; }
static OSStatus my_AudioUnitUninitialize(AudioUnit au) { if (vmic_unit(au)) return noErr; return AudioUnitUninitialize(au); }
static OSStatus my_AudioOutputUnitStart(AudioUnit au) { OSStatus r; if (vmic_start(au, 1, &r)) return r; r = AudioOutputUnitStart(au); if (atr()) nlog("[nhook] [audio] AudioOutputUnitStart %p -> %d", (void*)au, (int)r); return r; }
static OSStatus my_AudioOutputUnitStop(AudioUnit au) { OSStatus r; if (vmic_start(au, 0, &r)) return r; r = AudioOutputUnitStop(au); if (atr()) nlog("[nhook] [audio] AudioOutputUnitStop %p -> %d", (void*)au, (int)r); return r; }
INTERPOSE(my_AudioObjectGetPropertyData, AudioObjectGetPropertyData);
INTERPOSE(my_AudioObjectGetPropertyDataSize, AudioObjectGetPropertyDataSize);
INTERPOSE(my_AudioObjectSetPropertyData, AudioObjectSetPropertyData);
INTERPOSE(my_AudioObjectAddPropertyListener, AudioObjectAddPropertyListener);
INTERPOSE(my_AudioObjectRemovePropertyListener, AudioObjectRemovePropertyListener);
INTERPOSE(my_AudioComponentInstanceNew, AudioComponentInstanceNew);
INTERPOSE(my_AudioComponentInstanceDispose, AudioComponentInstanceDispose);
INTERPOSE(my_AudioUnitGetProperty, AudioUnitGetProperty);
INTERPOSE(my_AudioUnitInitialize, AudioUnitInitialize);
INTERPOSE(my_AudioUnitUninitialize, AudioUnitUninitialize);
INTERPOSE(my_AudioOutputUnitStart, AudioOutputUnitStart);
INTERPOSE(my_AudioOutputUnitStop, AudioOutputUnitStop);

// ---- the page (nstats.c: the snapshot's "soundboard", GET /soundboard.json, POST /control?do=sb, POST /soundboard/upload) -----
static void sb_urldecode(const char* in, char* out, size_t n) { size_t k = 0;
    for (size_t i = 0; in[i] && in[i] != '&' && k + 1 < n; i++) { if (in[i] == '%' && isxdigit((unsigned char)in[i + 1]) && isxdigit((unsigned char)in[i + 2])) { char h[3] = {in[i + 1], in[i + 2], 0}; out[k++] = (char)strtol(h, 0, 16); i += 2; }
        else out[k++] = in[i] == '+' ? ' ' : in[i]; } out[k] = 0; }
static void sb_json(StBuf* s) {   // the snapshot's state
    int real = !g_vmic_given && g_mic_t && mono_s() - g_mic_t < 3, virt = g_vmic_given; double now = mono_s();
    sb_put(s, ",\"soundboard\":{\"mic\":\"%s\",\"capturing\":%s,\"playing\":", virt ? "virtual" : real ? "real" : "none", (virt ? now - g_vmic_t < 1.5 : real) ? "true" : "false");
    if (g_sb_play) sb_str(s, g_sb_name); else sb_put(s, "null");
    sb_put(s, ",\"pos\":%.1f,\"len\":%.1f,\"vol\":%.2f,\"plays\":%lu,\"vmic\":%s}", g_sb_pos / VMIC_RATE, g_sb_len / VMIC_RATE, g_sb_vol, g_sb_plays, vmic_mode() ? "true" : "false"); }
static void sb_files_json(StBuf* s) {
    char d[1024]; sb_dir(d, sizeof d); DIR* dd = opendir(d); sb_put(s, "{\"folder\":\"ReclaimerMac/soundboard\",\"files\":["); int n = 0;
    struct dirent* e; while (dd && (e = readdir(dd))) { if (!sb_name_ok(e->d_name) || !sb_ext_ok(e->d_name)) continue; char p[1300]; struct stat st; snprintf(p, sizeof p, "%s/%s", d, e->d_name);
        if (stat(p, &st) || !S_ISREG(st.st_mode)) continue; sb_put(s, "%s{\"name\":", n++ ? "," : ""); sb_str(s, e->d_name); sb_put(s, ",\"bytes\":%lld}", (long long)st.st_size); }
    if (dd) closedir(dd); sb_put(s, "]}"); }
static int sb_control(const char* qs) {   // do=sb&play=<name> | stop=1 | vol=<0..2>
    const char* v;
    if ((v = strstr(qs, "play="))) { char nm[200]; sb_urldecode(v + 5, nm, sizeof nm); return sb_play(nm); }
    if (strstr(qs, "stop=")) { sb_stop(); return 1; }
    if ((v = strstr(qs, "vol="))) { float x = strtof(v + 4, 0); if (!(x >= 0 && x <= 2)) return 0; g_sb_vol = x; return 1; }
    return 0; }
// POST /soundboard/upload?name=<file name> (the page's "add" button; the same guard as /control): the body is the file
static void sb_upload(int fd, const char* req, const char* body0, size_t have, const char* qs) {
    char nm[200] = "", cl[32] = "", d[1024], p[1300], tp[1310]; const char* v = strstr(qs, "name="); if (v) sb_urldecode(v + 5, nm, sizeof nm);
    long long len = st_hdr(req, "Content-Length", cl, sizeof cl) ? atoll(cl) : -1; const char* err = 0;
    if (!sb_name_ok(nm) || !sb_ext_ok(nm)) err = "not a sound file name (.wav .aiff .caf .mp3 .m4a .aac .flac)";
    else if (len <= 0 || len > 50ll * 1024 * 1024) err = "missing or too big (50 MB at most)";
    FILE* f = 0; if (!err) { sb_dir(d, sizeof d); snprintf(p, sizeof p, "%s/%s", d, nm); snprintf(tp, sizeof tp, "%s.part", p); f = fopen(tp, "wb"); if (!f) err = "cannot write the soundboard folder"; }
    long long got = 0;
    if (!err) { size_t k = have > (size_t)len ? (size_t)len : have; if (k) fwrite(body0, 1, k, f); got = (long long)k; char buf[65536];
        while (got < len) { ssize_t r = recv(fd, buf, (size_t)(len - got < (long long)sizeof buf ? len - got : (long long)sizeof buf), 0); if (r <= 0) break; fwrite(buf, 1, (size_t)r, f); got += r; }
        fclose(f); if (got != len) { unlink(tp); err = "the upload stopped early"; } else if (rename(tp, p)) { unlink(tp); err = "cannot save it"; } }
    char out[400]; int k = snprintf(out, sizeof out, err ? "{\"ok\":false,\"error\":\"%s\"}" : "{\"ok\":true,\"name\":\"%s\"}", err ? err : nm);
    if (!err) nlog("[nhook] [soundboard] added %s (%lld bytes)", nm, len);
    char h[300]; int hk = snprintf(h, sizeof h, "HTTP/1.1 %s\r\nContent-Type: application/json\r\nContent-Length: %d\r\nConnection: close\r\n\r\n", err ? "400 Bad Request" : "200 OK", k);
    st_send(fd, h, (size_t)hk); st_send(fd, out, (size_t)k); }
