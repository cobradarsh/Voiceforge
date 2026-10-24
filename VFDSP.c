// VoiceForge DSP engine.
//
// Streaming TD-PSOLA (time-domain pitch-synchronous overlap-add) for a single voice.
//
// The input is cut into two-period grains centred on its glottal pulses ("analysis marks",
// one pitch period apart, found with the YIN pitch detector and aligned to the waveform by
// cross-correlation). Grains are overlap-added at a new spacing ("synthesis marks", one
// *output* period apart), so the output repeats real pulses of the speaker's voice at the new
// pitch. That keeps the voice periodic and clean, unlike frequency-domain shifting.
//
// Formants (the size of the vocal tract) are moved independently by resampling each grain:
// a grain played back `formant` times faster has its spectral envelope stretched by `formant`.
// Unvoiced sounds (s, f, breaths) keep their timing and only get the formant change.
#include "VFDSP.h"
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define VF_PI 3.14159265358979323846
#define VF_RING 16384               // power of two; holds input history and pending output
#define VF_MASK (VF_RING - 1)
#define VF_FMIN 75.0                // lowest speaking pitch tracked (Hz)
#define VF_FMAX 500.0               // highest speaking pitch tracked (Hz)
#define VF_FORMANT_MIN 0.7f         // bounds the grain length, and so the latency
#define VF_FORMANT_MAX 2.5f
#define VF_YIN_THRESHOLD 0.18f

struct VFEngine {
    double sr;
    int Tmin, Tmax;                 // period search range (samples)
    int dec;                        // decimation used by the pitch detector
    int lookahead, latency, detectHop;
    double unvoicedT;               // grain spacing for unvoiced sound

    float *in, *acc, *wsum;         // rings indexed by absolute sample time
    float *scratch, *cmnd;          // pitch detector work buffers

    int64_t now;                    // input samples written since reset
    int64_t nextDetect;
    double synPos;                  // next synthesis mark (input time)
    double anaMark;                 // current analysis mark (input time)
    double T;                       // current period (samples)
    int voiced, voicedHold;

    VFParams cur, pending;
    int dirty, active;

    float b0, b1, b2, a1, a2, z1, z2;   // low shelf
    float outGain;
};

static const struct { const char *name; VFParams p; } kPresets[VF_PRESET_COUNT] = {
    [VF_PRESET_NORMAL]    = { "Normal (Off)",  { 1.00f, 1.00f,  0.0f, 0.0f } },
    [VF_PRESET_DARK_LORD] = { "Dark Lord",     { 0.62f, 0.75f,  3.0f, 1.0f } },
    [VF_PRESET_TENOR]     = { "Tenor",         { 0.80f, 0.80f,  0.0f, 0.0f } },
    [VF_PRESET_NARRATOR]  = { "Deep Narrator", { 0.82f, 0.90f,  5.0f, 1.0f } },
    [VF_PRESET_FEMALE]    = { "Female",        { 1.75f, 1.25f, -2.0f, 0.0f } },
    [VF_PRESET_SMURF]     = { "Smurf",         { 1.60f, 1.60f,  0.0f, 0.0f } },
    [VF_PRESET_CHIPMUNK]  = { "Chipmunk",      { 2.20f, 2.20f,  0.0f, 0.0f } },
    [VF_PRESET_CUSTOM]    = { "Custom",        { 1.00f, 1.00f,  0.0f, 0.0f } },
};

static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

VFParams vf_preset_params(VFPreset preset, float customPitch, float customFormant) {
    if ((int)preset < 0 || preset >= VF_PRESET_COUNT) preset = VF_PRESET_NORMAL;
    VFParams p = kPresets[preset].p;
    if (preset == VF_PRESET_CUSTOM) {
        p.pitch = clampf(customPitch, 0.5f, 2.5f);
        p.formant = clampf(customFormant, 0.5f, 2.5f);
    }
    return p;
}

const char *vf_preset_name(VFPreset preset) {
    if ((int)preset < 0 || preset >= VF_PRESET_COUNT) return "?";
    return kPresets[preset].name;
}

static int is_identity(VFParams p) {
    return fabsf(p.pitch - 1.0f) < 1e-3f && fabsf(p.formant - 1.0f) < 1e-3f &&
           fabsf(p.bassDb) < 1e-3f && fabsf(p.gainDb) < 1e-3f;
}

// ---------------------------------------------------------------- setup

static void update_filter(VFEngine *e) {
    float A = powf(10.0f, e->cur.bassDb / 40.0f);
    float w0 = 2.0f * (float)VF_PI * 180.0f / (float)e->sr;
    float cs = cosf(w0), alpha = sinf(w0) / 2.0f * sqrtf(2.0f);
    float sA = 2.0f * sqrtf(A) * alpha;
    float a0 = (A + 1) + (A - 1) * cs + sA;
    e->b0 = A * ((A + 1) - (A - 1) * cs + sA) / a0;
    e->b1 = 2 * A * ((A - 1) - (A + 1) * cs) / a0;
    e->b2 = A * ((A + 1) - (A - 1) * cs - sA) / a0;
    e->a1 = -2 * ((A - 1) + (A + 1) * cs) / a0;
    e->a2 = ((A + 1) + (A - 1) * cs - sA) / a0;
    e->outGain = powf(10.0f, e->cur.gainDb / 20.0f);
}

VFEngine *vf_create(double sampleRate) {
    if (sampleRate < 4000 || sampleRate > 192000) return NULL;
    VFEngine *e = calloc(1, sizeof(VFEngine));
    if (!e) return NULL;
    e->sr = sampleRate;
    e->Tmin = (int)floor(sampleRate / VF_FMAX);
    e->Tmax = (int)ceil(sampleRate / VF_FMIN);
    // the detector only needs speech pitch (< 500 Hz), so it runs at ~11-16 kHz whatever the rate
    e->dec = (int)(sampleRate / 11000);
    if (e->dec < 1) e->dec = 1;
    e->detectHop = (int)(sampleRate * 0.005);           // re-estimate the pitch every 5 ms
    e->unvoicedT = sampleRate * 0.008;                  // 8 ms grains for unvoiced sound
    // Synthesis marks trail the input by enough to read a full grain and refine its mark;
    // output trails them by the longest (formant-stretched) grain.
    e->lookahead = (int)ceil(1.75 * e->Tmax) + 2;
    e->latency = e->lookahead + (int)ceil(e->Tmax / VF_FORMANT_MIN) + 4;

    int detN = e->Tmax / e->dec * 2 + 2;
    e->in = calloc(VF_RING, sizeof(float));
    e->acc = calloc(VF_RING, sizeof(float));
    e->wsum = calloc(VF_RING, sizeof(float));
    e->scratch = calloc(detN, sizeof(float));
    e->cmnd = calloc(detN, sizeof(float));
    if (!e->in || !e->acc || !e->wsum || !e->scratch || !e->cmnd) {
        vf_destroy(e);
        return NULL;
    }
    e->cur = e->pending = kPresets[VF_PRESET_NORMAL].p;
    update_filter(e);
    vf_reset(e);
    return e;
}

void vf_destroy(VFEngine *e) {
    if (!e) return;
    free(e->in);
    free(e->acc);
    free(e->wsum);
    free(e->scratch);
    free(e->cmnd);
    free(e);
}

double vf_sample_rate(const VFEngine *e) { return e->sr; }
int vf_latency_samples(const VFEngine *e) { return e->latency; }

void vf_set_params(VFEngine *e, VFParams p) {
    p.pitch = clampf(p.pitch, 0.25f, 4.0f);
    p.formant = clampf(p.formant, VF_FORMANT_MIN, VF_FORMANT_MAX);
    e->pending = p;
    e->dirty = 1;
}

void vf_reset(VFEngine *e) {
    memset(e->in, 0, VF_RING * sizeof(float));
    memset(e->acc, 0, VF_RING * sizeof(float));
    memset(e->wsum, 0, VF_RING * sizeof(float));
    e->now = 0;
    e->nextDetect = 0;
    e->synPos = 0;
    e->anaMark = 0;
    e->T = e->unvoicedT;
    e->voiced = 0;
    e->voicedHold = 0;
    e->z1 = e->z2 = 0;
}

// ---------------------------------------------------------------- pitch detection (YIN)

// Returns the period in samples of the most recent audio, or 0 if it is not voiced.
static double detect_period(VFEngine *e) {
    int D = e->dec;
    int tmax = e->Tmax / D, tmin = e->Tmin / D, W = tmax;
    int N = W + tmax + 1;
    int64_t start = e->now - (int64_t)N * D;
    float energy = 0;
    for (int i = 0; i < N; i++) {
        float v = 0;
        for (int k = 0; k < D; k++) v += e->in[(start + (int64_t)i * D + k) & VF_MASK];
        v /= D;
        e->scratch[i] = v;
        energy += v * v;
    }
    if (energy / N < 1e-6f) return 0;   // silence (< -60 dBFS)

    float run = 0;
    e->cmnd[0] = 1;
    for (int tau = 1; tau <= tmax; tau++) {
        float sum = 0;
        for (int j = 0; j < W; j++) {
            float d = e->scratch[j] - e->scratch[j + tau];
            sum += d * d;
        }
        run += sum;
        e->cmnd[tau] = run > 0 ? sum * tau / run : 1;
    }
    int tau = -1;
    for (int t = tmin; t <= tmax; t++) {
        if (e->cmnd[t] < VF_YIN_THRESHOLD) {
            while (t + 1 <= tmax && e->cmnd[t + 1] < e->cmnd[t]) t++;
            tau = t;
            break;
        }
    }
    if (tau < 0) return 0;
    double frac = 0;
    if (tau > 1 && tau < tmax) {
        float a = e->cmnd[tau - 1], b = e->cmnd[tau], c = e->cmnd[tau + 1];
        float den = a - 2 * b + c;
        if (fabsf(den) > 1e-9f) frac = 0.5 * (a - c) / den;
    }
    return (tau + frac) * D;
}

static void update_pitch(VFEngine *e) {
    double T = detect_period(e);
    if (T > 0) {
        // avoid sudden octave jumps while a voiced segment continues
        if (e->voiced) {
            double r = T / e->T;
            if (r > 1.8 && r < 2.2) T *= 0.5;
            else if (r > 0.45 && r < 0.55) T *= 2.0;
        }
        e->T = e->voiced ? 0.6 * T + 0.4 * e->T : T;
        e->voiced = 1;
        e->voicedHold = 3;
    } else if (e->voicedHold > 0) {
        e->voicedHold--;                // bridge short detector dropouts inside a vowel
    } else {
        e->voiced = 0;
        e->T = e->unvoicedT;
    }
}

// ---------------------------------------------------------------- grains

static inline float in_at(const VFEngine *e, double p) {
    double fl = floor(p);
    int64_t i = (int64_t)fl;
    float f = (float)(p - fl);
    return e->in[i & VF_MASK] * (1 - f) + e->in[(i + 1) & VF_MASK] * f;
}

// Move an analysis mark one period on, aligned to the previous grain by cross-correlation.
static double next_mark(const VFEngine *e, double prev, double T) {
    double cand = prev + T;
    if (!e->voiced) return cand;
    int half = (int)(T * 0.5), range = (int)(T / 6);
    if (range < 1) return cand;
    int64_t p0 = (int64_t)llround(prev), c0 = (int64_t)llround(cand);
    double best = -1e30;
    int bestS = 0;
    for (int s = -range; s <= range; s++) {
        double sum = 0;
        for (int k = -half; k <= half; k++)
            sum += e->in[(p0 + k) & VF_MASK] * e->in[(c0 + s + k) & VF_MASK];
        if (sum > best) { best = sum; bestS = s; }
    }
    return (double)(c0 + bestS);
}

// Overlap-add one grain: source centred on analysis mark `a`, output centred on `c`.
static void place_grain(VFEngine *e, double a, double c, double T, float formant) {
    double h = T / formant;                 // output half-length
    int64_t n0 = (int64_t)ceil(c - h), n1 = (int64_t)floor(c + h);
    for (int64_t n = n0; n <= n1; n++) {
        double k = n - c;
        float w = (float)(0.5 + 0.5 * cos(VF_PI * k / h));
        e->acc[n & VF_MASK] += w * in_at(e, a + k * formant);
        e->wsum[n & VF_MASK] += w;
    }
}

// ---------------------------------------------------------------- processing

static inline float soft_limit(float x) {
    float a = fabsf(x);
    if (a <= 0.8f) return x;
    float y = 0.8f + 0.2f * tanhf((a - 0.8f) / 0.2f);
    return x < 0 ? -y : y;
}

static float process_sample(VFEngine *e, float x) {
    e->in[e->now & VF_MASK] = x;
    e->now++;
    if (e->now >= e->nextDetect) {
        update_pitch(e);
        e->nextDetect = e->now + e->detectHop;
    }

    float pitch = e->cur.pitch, formant = e->cur.formant;
    while (e->synPos + e->lookahead <= (double)e->now) {
        double T = e->T;
        // analysis mark nearest the synthesis mark (time is preserved)
        while (e->anaMark + T * 0.5 < e->synPos) e->anaMark = next_mark(e, e->anaMark, T);
        place_grain(e, e->anaMark, e->synPos, T, formant);
        e->synPos += e->voiced ? T / pitch : T;
    }

    int64_t r = e->now - 1 - e->latency;
    float *acc = &e->acc[r & VF_MASK], *ws = &e->wsum[r & VF_MASK];
    float y = *ws > 0.35f ? *acc / *ws : *acc / 0.35f;
    *acc = 0;
    *ws = 0;
    return y;
}

void vf_process(VFEngine *e, float *buf, int n) {
    for (int i = 0; i < n; i++) {
        if (e->dirty) {
            int wasActive = e->active;
            e->cur = e->pending;
            e->dirty = 0;
            e->active = !is_identity(e->cur);
            update_filter(e);
            if (e->active && !wasActive) vf_reset(e);
        }
        if (!e->active) continue;

        float y = process_sample(e, buf[i]);
        // low shelf (transposed direct form II)
        float s = e->b0 * y + e->z1;
        e->z1 = e->b1 * y - e->a1 * s + e->z2;
        e->z2 = e->b2 * y - e->a2 * s;
        buf[i] = soft_limit(s * e->outGain);
    }
}
