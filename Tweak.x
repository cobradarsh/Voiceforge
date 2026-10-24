// VoiceForge — live voice changer for calls.
//
// One dylib, three roles:
//   SpringBoard          reads the Settings pane, watches calls (CallKit) and publishes a
//                        64-bit state word + a target word through notify(3); saves voices
//                        picked from the in-call menu.
//   audiomxd/mediaserverd hooks the public AudioToolbox render/process entry points and runs
//                        the VFDSP engine on the microphone stream while a call is active.
//   InCallService        draws the in-call button and voice menu.
#import <UIKit/UIKit.h>
#import <AudioToolbox/AudioToolbox.h>
#import <CallKit/CallKit.h>
#import <notify.h>
#import <os/log.h>
#include <stdlib.h>
#include <string.h>
#include "VFDSP.h"

#define VF_PREFS_DOMAIN   CFSTR("com.voiceforge.prefs")
#define VF_PREFS_CHANGED  "com.voiceforge/prefs"
#define VF_STATE_NAME     "com.voiceforge.state"
#define VF_TARGET_NAME    "com.voiceforge.target"
#define VF_SELECT_NAME    "com.voiceforge.select"   // in-call menu -> SpringBoard

// state word
#define VF_ST_PRESET(s)   ((int)((s) & 0xff))
#define VF_ST_ENABLED     (1ULL << 8)
#define VF_ST_INCALL      (1ULL << 9)
#define VF_ST_DEBUG       (1ULL << 10)
#define VF_ST_ALWAYS      (1ULL << 11)
#define VF_ST_BUTTON      (1ULL << 12)   // show the in-call button
#define VF_ST_PITCH(s)    ((float)(((s) >> 16) & 0xff) / 100.0f)
#define VF_ST_FORMANT(s)  ((float)(((s) >> 24) & 0xff) / 100.0f)

// target word: [0..31] component subtype (0 = auto), [32..35] function (0 = any), [36..43] bus (0xff = any)
enum { VF_FN_ANY = 0, VF_FN_RENDER = 1, VF_FN_PROCESS = 2, VF_FN_PROCESS_MULTI = 3 };
#define VF_TG_SUBTYPE(t)  ((OSType)((t) & 0xffffffffULL))
#define VF_TG_FN(t)       ((int)(((t) >> 32) & 0xf))
#define VF_TG_BUS(t)      ((int)(((t) >> 36) & 0xff))

#define VFLog(fmt, ...) os_log_error(OS_LOG_DEFAULT, "VoiceForge: " fmt, ##__VA_ARGS__)

// =====================================================================================
// SpringBoard side
// =====================================================================================

static int sSBStateToken = -1, sSBTargetToken = -1;
static BOOL sInCall;

static id vf_pref(NSString *key) {
    return CFBridgingRelease(CFPreferencesCopyAppValue((__bridge CFStringRef)key, VF_PREFS_DOMAIN));
}

static double vf_pref_double(NSString *key, double def) {
    id v = vf_pref(key);
    return [v respondsToSelector:@selector(doubleValue)] ? [v doubleValue] : def;
}

static BOOL vf_pref_bool(NSString *key, BOOL def) {
    id v = vf_pref(key);
    return [v respondsToSelector:@selector(boolValue)] ? [v boolValue] : def;
}

static OSType vf_fourcc(NSString *s) {
    const char *c = s.UTF8String;
    if (!c || strlen(c) != 4) return 0;
    return ((OSType)c[0] << 24) | ((OSType)c[1] << 16) | ((OSType)c[2] << 8) | (OSType)c[3];
}

// "auto", or "<fn>:<subtype>[:<bus>]" e.g. "render:vpio:1", "process:vp4 ", "any:auvp:*"
static uint64_t vf_parse_target(NSString *spec) {
    NSArray<NSString *> *parts = [[spec stringByTrimmingCharactersInSet:NSCharacterSet.newlineCharacterSet]
                                  componentsSeparatedByString:@":"];
    if (parts.count < 2) return 0;
    NSDictionary *fns = @{ @"any": @(VF_FN_ANY), @"render": @(VF_FN_RENDER),
                           @"process": @(VF_FN_PROCESS), @"multi": @(VF_FN_PROCESS_MULTI) };
    NSNumber *fn = fns[parts[0].lowercaseString];
    OSType subtype = vf_fourcc(parts[1]);
    if (!fn || !subtype) return 0;
    uint64_t bus = 0xff;
    if (parts.count > 2 && ![parts[2] isEqualToString:@"*"]) bus = (uint64_t)(parts[2].intValue & 0xff);
    return (uint64_t)subtype | ((uint64_t)fn.intValue << 32) | (bus << 36);
}

static void vf_publish(void) {
    CFPreferencesAppSynchronize(VF_PREFS_DOMAIN);
    uint64_t s = 0;
    int preset = (int)vf_pref_double(@"preset", 0);
    if (preset < 0 || preset >= VF_PRESET_COUNT) preset = 0;
    s |= (uint64_t)preset;
    if (vf_pref_bool(@"enabled", YES)) s |= VF_ST_ENABLED;
    if (sInCall) s |= VF_ST_INCALL;
    if (vf_pref_bool(@"debugLog", NO)) s |= VF_ST_DEBUG;
    if (vf_pref_bool(@"alwaysOn", NO)) s |= VF_ST_ALWAYS;
    if (vf_pref_bool(@"inCallButton", YES)) s |= VF_ST_BUTTON;
    uint64_t pitch = (uint64_t)lround(fmin(fmax(vf_pref_double(@"customPitch", 1.3), 0.5), 2.5) * 100);
    uint64_t formant = (uint64_t)lround(fmin(fmax(vf_pref_double(@"customFormant", 1.1), 0.5), 2.5) * 100);
    s |= pitch << 16 | formant << 24;

    id spec = vf_pref(@"targetSpec");
    uint64_t target = [spec isKindOfClass:NSString.class] ? vf_parse_target(spec) : 0;

    notify_set_state(sSBTargetToken, target);
    notify_set_state(sSBStateToken, s);
    notify_post(VF_TARGET_NAME);
    notify_post(VF_STATE_NAME);
}

@interface VFCallWatcher : NSObject <CXCallObserverDelegate>
@property (nonatomic, strong) CXCallObserver *observer;
@end

@implementation VFCallWatcher
- (instancetype)init {
    if ((self = [super init])) {
        _observer = [CXCallObserver new];
        [_observer setDelegate:self queue:dispatch_get_main_queue()];
        [self update];
    }
    return self;
}
- (void)callObserver:(CXCallObserver *)observer callChanged:(CXCall *)call {
    [self update];
}
- (void)update {
    BOOL active = NO;
    for (CXCall *call in self.observer.calls)
        if (!call.hasEnded) active = YES;
    sInCall = active;
    vf_publish();
}
@end

static VFCallWatcher *sCallWatcher;

static void vf_prefs_changed(CFNotificationCenterRef c, void *o, CFStringRef n, const void *obj, CFDictionaryRef info) {
    vf_publish();
}

// selection word from the in-call menu: [0..7] preset, bit 8 valid, bit 9 has pitch,
// [16..23] custom pitch x100, [24..31] custom formant x100
#define VF_SEL_VALID      (1ULL << 8)
#define VF_SEL_HAS_PITCH  (1ULL << 9)
static int sSBSelectToken = -1;

static void vf_select_changed(CFNotificationCenterRef c, void *o, CFStringRef n, const void *obj, CFDictionaryRef info) {
    uint64_t sel = 0;
    if (notify_get_state(sSBSelectToken, &sel) != NOTIFY_STATUS_OK || !(sel & VF_SEL_VALID)) return;
    int preset = VF_ST_PRESET(sel);
    if (preset >= VF_PRESET_COUNT) return;
    CFPreferencesSetAppValue(CFSTR("preset"), (__bridge CFNumberRef)@(preset), VF_PREFS_DOMAIN);
    CFPreferencesSetAppValue(CFSTR("enabled"), kCFBooleanTrue, VF_PREFS_DOMAIN);
    if (sel & VF_SEL_HAS_PITCH) {
        CFPreferencesSetAppValue(CFSTR("customPitch"), (__bridge CFNumberRef)@(VF_ST_PITCH(sel)), VF_PREFS_DOMAIN);
        CFPreferencesSetAppValue(CFSTR("customFormant"), (__bridge CFNumberRef)@(VF_ST_FORMANT(sel)), VF_PREFS_DOMAIN);
    }
    CFPreferencesAppSynchronize(VF_PREFS_DOMAIN);
    vf_publish();
}

static void vf_springboard_init(void) {
    notify_register_check(VF_STATE_NAME, &sSBStateToken);
    notify_register_check(VF_TARGET_NAME, &sSBTargetToken);
    notify_register_check(VF_SELECT_NAME, &sSBSelectToken);
    CFNotificationCenterAddObserver(CFNotificationCenterGetDarwinNotifyCenter(), NULL, vf_prefs_changed,
                                    CFSTR(VF_PREFS_CHANGED), NULL, CFNotificationSuspensionBehaviorCoalesce);
    CFNotificationCenterAddObserver(CFNotificationCenterGetDarwinNotifyCenter(), NULL, vf_select_changed,
                                    CFSTR(VF_SELECT_NAME), NULL, CFNotificationSuspensionBehaviorDeliverImmediately);
    dispatch_async(dispatch_get_main_queue(), ^{
        sCallWatcher = [VFCallWatcher new];
    });
}

// =====================================================================================
// Audio server side
// =====================================================================================

#define VF_MAX_SLOTS 32
#define VF_CHUNK 4096

typedef struct {
    void *volatile unit;                 // claim key
    volatile int ready;                  // fields below are initialised
    int fn;
    UInt32 bus;
    AudioComponentDescription desc;
    AudioStreamBasicDescription fmt;
    int fmtValid;
    uint32_t fmtAge;
    uint32_t seenGen;                    // log / params generation last applied
    VFEngine *engine;
    float *scratch;
} VFSlot;

static VFSlot sSlots[VF_MAX_SLOTS];
static int sAudioStateToken = -1, sAudioTargetToken = -1;
static uint64_t sState, sTarget;
static volatile uint32_t sGen = 1;

static void vf_refresh_state(void) {
    int changed = 0;
    if (sAudioStateToken >= 0 && notify_check(sAudioStateToken, &changed) == NOTIFY_STATUS_OK && changed) {
        uint64_t s = 0;
        notify_get_state(sAudioStateToken, &s);
        sState = s;
        __atomic_add_fetch(&sGen, 1, __ATOMIC_RELAXED);
    }
    changed = 0;
    if (sAudioTargetToken >= 0 && notify_check(sAudioTargetToken, &changed) == NOTIFY_STATUS_OK && changed) {
        uint64_t t = 0;
        notify_get_state(sAudioTargetToken, &t);
        sTarget = t;
        __atomic_add_fetch(&sGen, 1, __ATOMIC_RELAXED);
    }
}

static const char *vf_fn_name(int fn) {
    switch (fn) {
        case VF_FN_RENDER: return "render";
        case VF_FN_PROCESS: return "process";
        case VF_FN_PROCESS_MULTI: return "multi";
        default: return "any";
    }
}

static void vf_fourcc_str(OSType v, char out[5]) {
    for (int i = 0; i < 4; i++) {
        char c = (char)((v >> (24 - 8 * i)) & 0xff);
        out[i] = (c >= 32 && c < 127) ? c : '?';
    }
    out[4] = 0;
}

static VFSlot *vf_slot(AudioUnit unit, int fn, UInt32 bus) {
    for (int i = 0; i < VF_MAX_SLOTS; i++) {
        VFSlot *s = &sSlots[i];
        if (s->unit == unit && __atomic_load_n(&s->ready, __ATOMIC_ACQUIRE) && s->fn == fn && s->bus == bus) return s;
    }
    for (int i = 0; i < VF_MAX_SLOTS; i++) {
        VFSlot *s = &sSlots[i];
        void *expected = NULL;
        if (!s->unit && __atomic_compare_exchange_n(&s->unit, &expected, (void *)unit, false,
                                                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            s->fn = fn;
            s->bus = bus;
            s->fmtValid = 0;
            s->fmtAge = 0;
            s->seenGen = 0;
            memset(&s->desc, 0, sizeof s->desc);
            AudioComponent comp = AudioComponentInstanceGetComponent(unit);
            if (comp) AudioComponentGetDescription(comp, &s->desc);
            __atomic_store_n(&s->ready, 1, __ATOMIC_RELEASE);
            return s;
        }
    }
    return NULL;
}

static void vf_refresh_format(VFSlot *s, AudioUnit unit) {
    if (s->fmtValid && ++s->fmtAge < 1000) return;
    s->fmtAge = 0;
    AudioStreamBasicDescription f;
    UInt32 size = sizeof f;
    AudioUnitElement element = s->fn == VF_FN_RENDER ? s->bus : 0;
    if (AudioUnitGetProperty(unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, element, &f, &size) == noErr) {
        s->fmt = f;
        s->fmtValid = 1;
    }
}

static BOOL vf_target_matches(const VFSlot *s) {
    uint64_t t = sTarget;
    OSType subtype = VF_TG_SUBTYPE(t);
    if (!subtype) {
        // auto: microphone side of the voice-processing / remote I/O units
        return s->fn == VF_FN_RENDER && s->bus == 1 && s->desc.componentType == kAudioUnitType_Output &&
               (s->desc.componentSubType == kAudioUnitSubType_VoiceProcessingIO ||
                s->desc.componentSubType == kAudioUnitSubType_RemoteIO);
    }
    if (s->desc.componentSubType != subtype) return NO;
    if (VF_TG_FN(t) != VF_FN_ANY && VF_TG_FN(t) != s->fn) return NO;
    if (VF_TG_BUS(t) != 0xff && (UInt32)VF_TG_BUS(t) != s->bus) return NO;
    return YES;
}

static void vf_log_slot(const VFSlot *s, UInt32 frames, BOOL match) {
    char type[5], sub[5], manu[5], fmtid[5];
    vf_fourcc_str(s->desc.componentType, type);
    vf_fourcc_str(s->desc.componentSubType, sub);
    vf_fourcc_str(s->desc.componentManufacturer, manu);
    vf_fourcc_str(s->fmt.mFormatID, fmtid);
    VFLog("seen fn=%{public}s unit=%p comp=%{public}s/%{public}s/%{public}s bus=%u frames=%u "
          "fmt=%{public}s %.0fHz ch=%u bits=%u flags=0x%x inCall=%d target=%{public}s",
          vf_fn_name(s->fn), s->unit, type, sub, manu, (unsigned)s->bus, (unsigned)frames, fmtid,
          s->fmt.mSampleRate, (unsigned)s->fmt.mChannelsPerFrame, (unsigned)s->fmt.mBitsPerChannel,
          (unsigned)s->fmt.mFormatFlags, (sState & VF_ST_INCALL) ? 1 : 0, match ? "MATCH" : "-");
}

static BOOL vf_prepare_engine(VFSlot *s) {
    double rate = s->fmt.mSampleRate;
    if (s->engine && vf_sample_rate(s->engine) != rate) {
        vf_destroy(s->engine);
        s->engine = NULL;
    }
    if (!s->engine) {
        s->engine = vf_create(rate);
        if (!s->engine) return NO;
        s->seenGen = 0;
    }
    if (!s->scratch && !(s->scratch = malloc(VF_CHUNK * sizeof(float)))) return NO;
    return YES;
}

static void vf_process_abl(VFSlot *s, AudioBufferList *abl, UInt32 frames) {
    const AudioStreamBasicDescription *f = &s->fmt;
    if (!abl || abl->mNumberBuffers == 0 || f->mFormatID != kAudioFormatLinearPCM) return;
    BOOL isFloat = (f->mFormatFlags & kAudioFormatFlagIsFloat) != 0;
    BOOL nonInterleaved = (f->mFormatFlags & kAudioFormatFlagIsNonInterleaved) != 0;
    UInt32 bits = f->mBitsPerChannel;
    if (!(isFloat ? bits == 32 : (bits == 16 || bits == 32))) return;
    UInt32 stride = nonInterleaved ? 1 : (f->mChannelsPerFrame ? f->mChannelsPerFrame : 1);
    UInt32 bps = bits / 8;
    AudioBuffer *b0 = &abl->mBuffers[0];
    if (!b0->mData) return;
    UInt32 avail = b0->mDataByteSize / (bps * stride);
    if (frames > avail) frames = avail;
    UInt32 fracBits = (f->mFormatFlags & kLinearPCMFormatFlagsSampleFractionMask) >> kLinearPCMFormatFlagsSampleFractionShift;
    float fullScale = isFloat ? 1.0f : (float)(fracBits ? (1u << fracBits) : (1u << (bits - 1)));

    for (UInt32 done = 0; done < frames;) {
        UInt32 n = frames - done < VF_CHUNK ? frames - done : VF_CHUNK;
        float *x = s->scratch;
        for (UInt32 i = 0; i < n; i++) {
            UInt32 idx = (done + i) * stride;
            x[i] = isFloat ? ((float *)b0->mData)[idx]
                 : bits == 16 ? ((SInt16 *)b0->mData)[idx] / fullScale
                 : ((SInt32 *)b0->mData)[idx] / fullScale;
        }
        vf_process(s->engine, x, (int)n);
        for (UInt32 bi = 0; bi < abl->mNumberBuffers; bi++) {
            AudioBuffer *b = &abl->mBuffers[bi];
            if (!b->mData || b->mDataByteSize / (bps * stride) < done + n) continue;
            for (UInt32 i = 0; i < n; i++) {
                for (UInt32 c = 0; c < stride; c++) {
                    UInt32 idx = (done + i) * stride + c;
                    if (isFloat) ((float *)b->mData)[idx] = x[i];
                    else if (bits == 16) ((SInt16 *)b->mData)[idx] = (SInt16)fmaxf(-32768.0f, fminf(32767.0f, x[i] * fullScale));
                    else ((SInt32 *)b->mData)[idx] = (SInt32)fmax(-2147483648.0, fmin(2147483647.0, (double)x[i] * fullScale));
                }
            }
            if (!nonInterleaved) break;
        }
        done += n;
    }
}

static void vf_handle(AudioUnit unit, int fn, UInt32 bus, UInt32 frames, AudioBufferList *abl) {
    if (!unit || !abl) return;
    vf_refresh_state();
    uint64_t st = sState;
    BOOL debug = (st & VF_ST_DEBUG) != 0;
    BOOL wantProcess = (st & VF_ST_ENABLED) && VF_ST_PRESET(st) != VF_PRESET_NORMAL &&
                       (st & (VF_ST_INCALL | VF_ST_ALWAYS));
    if (!debug && !wantProcess) return;

    VFSlot *s = vf_slot(unit, fn, bus);
    if (!s) return;
    vf_refresh_format(s, unit);
    if (!s->fmtValid) return;

    uint32_t gen = __atomic_load_n(&sGen, __ATOMIC_RELAXED);
    BOOL match = vf_target_matches(s);
    BOOL newGen = s->seenGen != gen;
    if (debug && newGen) vf_log_slot(s, frames, match);
    if (!match || !wantProcess) {
        s->seenGen = gen;
        return;
    }
    if (!vf_prepare_engine(s)) return;
    if (s->seenGen != gen) {
        vf_set_params(s->engine, vf_preset_params((VFPreset)VF_ST_PRESET(st), VF_ST_PITCH(st), VF_ST_FORMANT(st)));
        s->seenGen = gen;
    }
    vf_process_abl(s, abl, frames);
}

static void vf_release_unit(AudioComponentInstance unit) {
    for (int i = 0; i < VF_MAX_SLOTS; i++) {
        VFSlot *s = &sSlots[i];
        if (s->unit != unit) continue;
        VFEngine *e = s->engine;
        float *scratch = s->scratch;
        s->engine = NULL;
        s->scratch = NULL;
        __atomic_store_n(&s->ready, 0, __ATOMIC_RELEASE);
        __atomic_store_n(&s->unit, NULL, __ATOMIC_RELEASE);
        vf_destroy(e);
        free(scratch);
    }
}

%group VFAudio

%hookf(OSStatus, AudioUnitRender, AudioUnit unit, AudioUnitRenderActionFlags *flags, const AudioTimeStamp *ts, UInt32 bus, UInt32 frames, AudioBufferList *ioData) {
    OSStatus r = %orig;
    if (r == noErr) vf_handle(unit, VF_FN_RENDER, bus, frames, ioData);
    return r;
}

%hookf(OSStatus, AudioUnitProcess, AudioUnit unit, AudioUnitRenderActionFlags *flags, const AudioTimeStamp *ts, UInt32 frames, AudioBufferList *ioData) {
    OSStatus r = %orig;
    if (r == noErr) vf_handle(unit, VF_FN_PROCESS, 0, frames, ioData);
    return r;
}

%hookf(OSStatus, AudioUnitProcessMultiple, AudioUnit unit, AudioUnitRenderActionFlags *flags, const AudioTimeStamp *ts, UInt32 frames, UInt32 numInputs, const AudioBufferList **inputs, UInt32 numOutputs, AudioBufferList **outputs) {
    OSStatus r = %orig;
    if (r == noErr && numOutputs > 0 && outputs) vf_handle(unit, VF_FN_PROCESS_MULTI, 0, frames, outputs[0]);
    return r;
}

%hookf(OSStatus, AudioComponentInstanceDispose, AudioComponentInstance unit) {
    vf_release_unit(unit);
    return %orig;
}

%end

static void vf_audio_init(void) {
    notify_register_check(VF_STATE_NAME, &sAudioStateToken);
    notify_register_check(VF_TARGET_NAME, &sAudioTargetToken);
    notify_get_state(sAudioStateToken, &sState);
    notify_get_state(sAudioTargetToken, &sTarget);
    VFLog("loaded into %{public}s, state=0x%llx target=0x%llx", getprogname(), sState, sTarget);
}

// =====================================================================================
// In-call screen (InCallService): floating button + voice menu
// =====================================================================================

static int sICStateToken = -1, sICSelectToken = -1;

static const int kMenuPresets[] = { VF_PRESET_NORMAL, VF_PRESET_FEMALE, VF_PRESET_NARRATOR, VF_PRESET_DARK_LORD,
                                    VF_PRESET_TENOR, VF_PRESET_SMURF, VF_PRESET_CHIPMUNK, VF_PRESET_CUSTOM };
static NSString *const kMenuTitles[] = { @"Off", @"Female", @"Narrator", @"Dark Lord",
                                         @"Tenor", @"Smurf", @"Chipmunk", @"Custom" };
static const int kMenuCount = 8;
static const int kLowPitches[] = { 70, 80, 90 };
static const int kHighPitches[] = { 110, 120, 130, 140, 150, 160, 170, 180, 190, 200 };

static uint64_t vf_ic_state(void) {
    uint64_t s = 0;
    if (sICStateToken >= 0) notify_get_state(sICStateToken, &s);
    return s;
}

// Custom pitch from the menu: formants follow at a cube-root rate, which keeps the voice
// natural (pitch 1.7 -> formant 1.19, close to the Female preset; 0.8 -> 0.93).
static void vf_ic_select(int preset, int pitchX100) {
    uint64_t sel = (uint64_t)preset | VF_SEL_VALID;
    if (pitchX100 > 0) {
        float pitch = pitchX100 / 100.0f;
        uint64_t formant = (uint64_t)lroundf(powf(pitch, 0.33f) * 100);
        sel |= VF_SEL_HAS_PITCH | (uint64_t)pitchX100 << 16 | formant << 24;
    }
    notify_set_state(sICSelectToken, sel);
    notify_post(VF_SELECT_NAME);
}

@interface VFInCallUI : NSObject
+ (instancetype)shared;
- (void)attachTo:(UIView *)host;
- (void)stateChanged;
@end

@implementation VFInCallUI {
    __weak UIView *_host;
    UIButton *_button;
    UIControl *_backdrop;
    UIVisualEffectView *_panel;
    UILabel *_subtitle;
    NSMutableArray<UIButton *> *_presetButtons;
    NSMutableArray<UIButton *> *_pitchButtons;
    BOOL _customOpen;
    int _shownPreset;
    int _shownPitch;   // x100
    BOOL _enabled;
}

+ (instancetype)shared {
    static VFInCallUI *ui;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ ui = [VFInCallUI new]; });
    return ui;
}

- (instancetype)init {
    if ((self = [super init])) {
        _presetButtons = [NSMutableArray array];
        _pitchButtons = [NSMutableArray array];
        [self readState];
    }
    return self;
}

- (void)readState {
    uint64_t st = vf_ic_state();
    _shownPreset = VF_ST_PRESET(st);
    _shownPitch = (int)lroundf(VF_ST_PITCH(st) * 100);
    _enabled = (st & VF_ST_ENABLED) != 0;
}

- (void)stateChanged {
    [self readState];
    if (!(vf_ic_state() & VF_ST_BUTTON)) {
        [self detach];
        return;
    }
    [self refresh];
}

// ------------------------------------------------------------------ button

- (UIButton *)makeButton {
    UIButton *b = [UIButton buttonWithType:UIButtonTypeSystem];
    b.frame = CGRectMake(0, 0, 56, 56);
    b.layer.cornerRadius = 16;
    b.clipsToBounds = YES;
    UIVisualEffectView *bg = [[UIVisualEffectView alloc] initWithEffect:[UIBlurEffect effectWithStyle:UIBlurEffectStyleSystemThinMaterialDark]];
    bg.frame = b.bounds;
    bg.userInteractionEnabled = NO;
    bg.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    [b insertSubview:bg atIndex:0];
    UIImageSymbolConfiguration *cfg = [UIImageSymbolConfiguration configurationWithPointSize:24 weight:UIImageSymbolWeightSemibold];
    [b setImage:[UIImage systemImageNamed:@"waveform" withConfiguration:cfg] forState:UIControlStateNormal];
    [b addTarget:self action:@selector(toggleMenu) forControlEvents:UIControlEventTouchUpInside];
    [b addGestureRecognizer:[[UIPanGestureRecognizer alloc] initWithTarget:self action:@selector(drag:)]];
    b.accessibilityLabel = @"Voice changer";
    return b;
}

- (CGPoint)clamp:(CGPoint)p {
    UIView *host = _host;
    UIEdgeInsets s = host.safeAreaInsets;
    CGFloat r = 28 + 8;
    p.x = fmax(r, fmin(host.bounds.size.width - r, p.x));
    p.y = fmax(s.top + r, fmin(host.bounds.size.height - s.bottom - r, p.y));
    return p;
}

- (void)drag:(UIPanGestureRecognizer *)g {
    UIView *host = _host;
    if (!host) return;
    CGPoint t = [g translationInView:host];
    _button.center = CGPointMake(_button.center.x + t.x, _button.center.y + t.y);
    [g setTranslation:CGPointZero inView:host];
    if (g.state == UIGestureRecognizerStateEnded || g.state == UIGestureRecognizerStateCancelled) {
        _button.center = [self clamp:_button.center];
        [NSUserDefaults.standardUserDefaults setObject:NSStringFromCGPoint(_button.center) forKey:@"VoiceForgeButtonCenter"];
    }
}

- (void)attachTo:(UIView *)host {
    [self readState];
    if (!(vf_ic_state() & VF_ST_BUTTON)) {
        [self detach];
        return;
    }
    if (_host == host && _button.superview == host) {
        [host bringSubviewToFront:_button];
        [self refresh];
        return;
    }
    [self closeMenu];
    _host = host;
    if (!_button) _button = [self makeButton];
    [host addSubview:_button];
    NSString *saved = [NSUserDefaults.standardUserDefaults stringForKey:@"VoiceForgeButtonCenter"];
    CGPoint c = saved ? CGPointFromString(saved)
                      : CGPointMake(host.bounds.size.width - 36, host.safeAreaInsets.top + 115);
    _button.center = [self clamp:c];
    [self refresh];
}

- (void)detach {
    [self closeMenu];
    [_button removeFromSuperview];
    _host = nil;
}

// ------------------------------------------------------------------ menu

- (void)toggleMenu {
    if (_panel) [self closeMenu];
    else [self openMenu];
}

- (void)openMenu {
    UIView *host = _host;
    if (!host) return;
    [self readState];
    _customOpen = _shownPreset == VF_PRESET_CUSTOM;

    _backdrop = [[UIControl alloc] initWithFrame:host.bounds];
    _backdrop.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    _backdrop.backgroundColor = [UIColor colorWithWhite:0 alpha:0.25];
    [_backdrop addTarget:self action:@selector(closeMenu) forControlEvents:UIControlEventTouchUpInside];
    [host addSubview:_backdrop];

    _panel = [[UIVisualEffectView alloc] initWithEffect:[UIBlurEffect effectWithStyle:UIBlurEffectStyleSystemMaterialDark]];
    _panel.layer.cornerRadius = 24;
    _panel.clipsToBounds = YES;
    [host addSubview:_panel];
    [self buildPanel];

    _backdrop.alpha = 0;
    _panel.alpha = 0;
    _panel.transform = CGAffineTransformMakeScale(0.92, 0.92);
    [UIView animateWithDuration:0.22 animations:^{
        self->_backdrop.alpha = 1;
        self->_panel.alpha = 1;
        self->_panel.transform = CGAffineTransformIdentity;
    }];
}

- (void)closeMenu {
    UIView *panel = _panel, *backdrop = _backdrop;
    _panel = nil;
    _backdrop = nil;
    _subtitle = nil;
    [_presetButtons removeAllObjects];
    [_pitchButtons removeAllObjects];
    if (!panel) return;
    [UIView animateWithDuration:0.18 animations:^{
        panel.alpha = 0;
        backdrop.alpha = 0;
    } completion:^(BOOL finished) {
        [panel removeFromSuperview];
        [backdrop removeFromSuperview];
    }];
}

- (UILabel *)labelWithFrame:(CGRect)frame text:(NSString *)text size:(CGFloat)size alpha:(CGFloat)alpha {
    UILabel *l = [[UILabel alloc] initWithFrame:frame];
    l.text = text;
    l.font = [UIFont systemFontOfSize:size weight:UIFontWeightSemibold];
    l.textColor = [UIColor colorWithWhite:1 alpha:alpha];
    return l;
}

- (UIButton *)chipWithFrame:(CGRect)frame title:(NSString *)title tag:(NSInteger)tag action:(SEL)action {
    UIButton *b = [UIButton buttonWithType:UIButtonTypeCustom];
    b.frame = frame;
    b.tag = tag;
    b.layer.cornerRadius = frame.size.height / 2;
    b.titleLabel.font = [UIFont systemFontOfSize:14 weight:UIFontWeightSemibold];
    b.titleLabel.adjustsFontSizeToFitWidth = YES;
    b.titleLabel.minimumScaleFactor = 0.7;
    [b setTitle:title forState:UIControlStateNormal];
    [b addTarget:self action:action forControlEvents:UIControlEventTouchUpInside];
    return b;
}

- (void)layoutPitchRow:(const int *)values count:(int)n y:(CGFloat)y width:(CGFloat)W diameter:(CGFloat)d gap:(CGFloat)gap in:(UIView *)c {
    CGFloat rowW = n * d + (n - 1) * gap, x = (W - rowW) / 2;
    for (int i = 0; i < n; i++) {
        NSString *t = [NSString stringWithFormat:@"%.1f", values[i] / 100.0];
        UIButton *b = [self chipWithFrame:CGRectMake(x + i * (d + gap), y, d, d) title:t tag:values[i] action:@selector(pitchTapped:)];
        [c addSubview:b];
        [_pitchButtons addObject:b];
    }
}

- (void)buildPanel {
    UIView *host = _host;
    UIView *c = _panel.contentView;
    for (UIView *v in [c.subviews copy]) [v removeFromSuperview];
    [_presetButtons removeAllObjects];
    [_pitchButtons removeAllObjects];

    CGFloat W = fmin(host.bounds.size.width - 32, 420), pad = 16, gap = 8, y = 16;
    [c addSubview:[self labelWithFrame:CGRectMake(pad, y, W - 2 * pad, 18) text:@"Voice changer" size:14 alpha:0.6]];
    y += 20;
    _subtitle = [self labelWithFrame:CGRectMake(pad, y, W - 2 * pad, 22) text:@"" size:17 alpha:0.95];
    [c addSubview:_subtitle];
    y += 22 + 14;

    CGFloat cw = (W - 2 * pad - 3 * gap) / 4, ch = 42;
    for (int i = 0; i < kMenuCount; i++) {
        CGRect f = CGRectMake(pad + (i % 4) * (cw + gap), y + (i / 4) * (ch + gap), cw, ch);
        UIButton *b = [self chipWithFrame:f title:kMenuTitles[i] tag:kMenuPresets[i] action:@selector(presetTapped:)];
        [c addSubview:b];
        [_presetButtons addObject:b];
    }
    y += 2 * ch + gap;

    if (_customOpen) {
        y += 18;
        [c addSubview:[self labelWithFrame:CGRectMake(pad, y, W - 2 * pad, 16) text:@"Custom pitch" size:13 alpha:0.6]];
        y += 24;
        CGFloat d = fmin(52, (W - 2 * pad - 4 * gap) / 5);
        [self layoutPitchRow:kLowPitches count:3 y:y width:W diameter:d gap:gap in:c];
        y += d + 14;
        UIView *line = [[UIView alloc] initWithFrame:CGRectMake(pad + 12, y, W - 2 * pad - 24, 1)];
        line.backgroundColor = [UIColor colorWithWhite:1 alpha:0.5];
        [c addSubview:line];
        y += 15;
        [self layoutPitchRow:kHighPitches count:5 y:y width:W diameter:d gap:gap in:c];
        y += d + gap;
        [self layoutPitchRow:kHighPitches + 5 count:5 y:y width:W diameter:d gap:gap in:c];
        y += d;
    }
    y += pad + 4;

    CGFloat top = fmax(host.safeAreaInsets.top + 60, (host.bounds.size.height - y) / 2 - 40);
    _panel.frame = CGRectMake((host.bounds.size.width - W) / 2, top, W, y);
    [self refresh];
}

- (void)styleChip:(UIButton *)b selected:(BOOL)selected {
    b.backgroundColor = selected ? [UIColor colorWithWhite:1 alpha:0.92] : [UIColor colorWithWhite:1 alpha:0.12];
    [b setTitleColor:selected ? UIColor.blackColor : UIColor.whiteColor forState:UIControlStateNormal];
}

- (void)refresh {
    BOOL active = _enabled && _shownPreset != VF_PRESET_NORMAL;
    _button.tintColor = active ? UIColor.systemGreenColor : UIColor.whiteColor;
    for (UIButton *b in _presetButtons) [self styleChip:b selected:b.tag == _shownPreset];
    for (UIButton *b in _pitchButtons)
        [self styleChip:b selected:_shownPreset == VF_PRESET_CUSTOM && labs((long)b.tag - _shownPitch) <= 1];
    NSString *name = [NSString stringWithUTF8String:vf_preset_name((VFPreset)_shownPreset)];
    if (!_enabled) name = @"Off (disabled in Settings)";
    else if (_shownPreset == VF_PRESET_CUSTOM) name = [NSString stringWithFormat:@"Custom · pitch %.2f", _shownPitch / 100.0];
    _subtitle.text = name;
}

- (void)presetTapped:(UIButton *)b {
    int preset = (int)b.tag;
    _enabled = YES;
    _shownPreset = preset;
    if (preset == VF_PRESET_CUSTOM) {
        if (_shownPitch < 50) _shownPitch = 130;
        vf_ic_select(preset, _shownPitch);
        _customOpen = YES;
        [self buildPanel];
        return;
    }
    vf_ic_select(preset, 0);
    if (_customOpen) {
        _customOpen = NO;
        [self buildPanel];
    } else {
        [self refresh];
    }
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.35 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
        [self closeMenu];
    });
}

- (void)pitchTapped:(UIButton *)b {
    _enabled = YES;
    _shownPreset = VF_PRESET_CUSTOM;
    _shownPitch = (int)b.tag;
    vf_ic_select(VF_PRESET_CUSTOM, _shownPitch);
    [self refresh];
}

@end

static void vf_ic_state_changed(CFNotificationCenterRef c, void *o, CFStringRef n, const void *obj, CFDictionaryRef info) {
    dispatch_async(dispatch_get_main_queue(), ^{
        [[VFInCallUI shared] stateChanged];
    });
}

%group VFInCall

%hook UIViewController
- (void)viewDidAppear:(BOOL)animated {
    %orig;
    NSString *cls = NSStringFromClass(self.class);
    if (vf_ic_state() & VF_ST_DEBUG) VFLog("InCallService view controller appeared: %{public}@", cls);
    // PHAudioCallViewController is the voice-call screen (name matched loosely across iOS versions)
    if ([cls containsString:@"AudioCallViewController"]) [[VFInCallUI shared] attachTo:self.view];
}
%end

%end

static void vf_incall_init(void) {
    notify_register_check(VF_STATE_NAME, &sICStateToken);
    notify_register_check(VF_SELECT_NAME, &sICSelectToken);
    CFNotificationCenterAddObserver(CFNotificationCenterGetDarwinNotifyCenter(), NULL, vf_ic_state_changed,
                                    CFSTR(VF_STATE_NAME), NULL, CFNotificationSuspensionBehaviorDeliverImmediately);
}

%ctor {
    const char *name = getprogname();
    NSString *bundleID = NSBundle.mainBundle.bundleIdentifier;
    if (!strcmp(name, "audiomxd") || !strcmp(name, "mediaserverd")) {
        vf_audio_init();
        %init(VFAudio);
    } else if ([bundleID isEqualToString:@"com.apple.springboard"]) {
        vf_springboard_init();
    } else if ([bundleID isEqualToString:@"com.apple.InCallService"]) {
        vf_incall_init();
        %init(VFInCall);
    }
}
