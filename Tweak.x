// VoiceForge — live voice changer for calls.
//
// One dylib, two roles:
//   SpringBoard          reads the Settings pane, watches calls (CallKit) and publishes a
//                        64-bit state word + a target word through notify(3).
//   audiomxd/mediaserverd hooks the public AudioToolbox render/process entry points and runs
//                        the VFDSP engine on the microphone stream while a call is active.
#import <Foundation/Foundation.h>
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

// state word
#define VF_ST_PRESET(s)   ((int)((s) & 0xff))
#define VF_ST_ENABLED     (1ULL << 8)
#define VF_ST_INCALL      (1ULL << 9)
#define VF_ST_DEBUG       (1ULL << 10)
#define VF_ST_ALWAYS      (1ULL << 11)
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

static void vf_springboard_init(void) {
    notify_register_check(VF_STATE_NAME, &sSBStateToken);
    notify_register_check(VF_TARGET_NAME, &sSBTargetToken);
    CFNotificationCenterAddObserver(CFNotificationCenterGetDarwinNotifyCenter(), NULL, vf_prefs_changed,
                                    CFSTR(VF_PREFS_CHANGED), NULL, CFNotificationSuspensionBehaviorCoalesce);
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

%ctor {
    const char *name = getprogname();
    if (!strcmp(name, "audiomxd") || !strcmp(name, "mediaserverd")) {
        vf_audio_init();
        %init(VFAudio);
    } else if ([NSBundle.mainBundle.bundleIdentifier isEqualToString:@"com.apple.springboard"]) {
        vf_springboard_init();
    }
}
