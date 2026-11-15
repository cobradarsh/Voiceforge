// VoiceForge DSP engine: streaming pitch + formant shifter for mono float audio.
// Portable C, no platform dependencies, no allocation after vf_create().
#pragma once
#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    VF_PRESET_NORMAL = 0,
    VF_PRESET_DARK_LORD,
    VF_PRESET_TENOR,
    VF_PRESET_NARRATOR,     // deep, warm narrator voice
    VF_PRESET_FEMALE,
    VF_PRESET_SMURF,
    VF_PRESET_CHIPMUNK,
    VF_PRESET_CUSTOM,
    VF_PRESET_FEMALE_YOUNG,   // light, bright young woman
    VF_PRESET_FEMALE_MATURE,  // fuller middle-aged woman
    VF_PRESET_FEMALE_SENIOR,  // older woman: lower, softer, slight tremor
    VF_PRESET_COUNT
} VFPreset;

typedef struct {
    float pitch;     // frequency ratio applied to the voice's pitch (1.0 = unchanged)
    float formant;   // frequency ratio applied to the vocal-tract envelope (1.0 = unchanged)
    float bassDb;    // low-shelf gain at ~180 Hz
    float gainDb;    // output gain before the soft limiter
    float tremor;    // slow pitch wobble depth (0 = none, 0.03 = +-3 %), ~5.5 Hz
} VFParams;

typedef struct VFEngine VFEngine;

// Returns the parameter set for a preset. For VF_PRESET_CUSTOM, pitch/formant come from the arguments.
VFParams vf_preset_params(VFPreset preset, float customPitch, float customFormant);
const char *vf_preset_name(VFPreset preset);

VFEngine *vf_create(double sampleRate);
void vf_destroy(VFEngine *e);
double vf_sample_rate(const VFEngine *e);
int vf_latency_samples(const VFEngine *e);

// Parameter changes are picked up at the next frame boundary; safe to call from the audio thread.
void vf_set_params(VFEngine *e, VFParams p);
void vf_reset(VFEngine *e);

// In-place processing of n mono samples. Output is delayed by vf_latency_samples().
void vf_process(VFEngine *e, float *buf, int n);

// Percentage of recent non-silent audio in which a voice pitch was tracked (0-100), then resets
// the counters. Low values on real speech mean the pitch detector is not seeing the voice.
int vf_voiced_percent(VFEngine *e);

#ifdef __cplusplus
}
#endif
