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
    VF_PRESET_COUNT
} VFPreset;

typedef struct {
    float pitch;     // frequency ratio applied to the voice's pitch (1.0 = unchanged)
    float formant;   // frequency ratio applied to the vocal-tract envelope (1.0 = unchanged)
    float bassDb;    // low-shelf gain at ~180 Hz
    float gainDb;    // output gain before the soft limiter
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

#ifdef __cplusplus
}
#endif
