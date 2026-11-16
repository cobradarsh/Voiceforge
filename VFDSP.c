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
//
// The pitch detector is tuned for call-microphone audio (noise, echo cancellation, band
// limiting), not studio speech: a loose YIN threshold with a best-candidate fallback, a low
// energy gate, and a long hold across short dropouts. vf_voiced_percent() reports how much
// of the speech it is actually tracking, so this can be checked on the device.
#include "VFDSP.h"
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define VF_PI 3.14159265358979323846
#define VF_RING 16384               // power of two; holds input history and pending output
#define VF_MASK (VF_RING - 1)
#define VF_FMIN 70.0                // lowest speaking pitch tracked (Hz)
#define VF_FMAX 500.0               // highest speaking pitch tracked (Hz)
#define VF_FORMANT_MIN 0.7f         // bounds the grain length, and so the latency
#define VF_FORMANT_MAX 2.5f
#define VF_YIN_THRESHOLD 0.35f      // first dip below this wins
#define VF_YIN_FALLBACK 0.55f       // else the deepest dip, if it is at least this clear
#define VF_SILENCE_RMS 3e-4f        // ~ -70 dBFS
#define VF_VOICED_HOLD 8            // detector hops (40 ms) bridged inside a vowel
#define VF_TREMOR_HZ 5.5f

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
    float tremorPhase;

    unsigned statSpeech, statVoiced;   // detector hops with speech / with a tracked pitch

    VFParams cur, pending;
    int dirty, active;

    struct { float b0, b1, b2, a1, a2, z1, z2; } lowShelf, midBell, airShelf;
    float outGain;
    uint32_t noise;                 // xorshift state for breath
    float breathHp, breathHpIn, breathEnv, breathPole, breathDecay;
};

static const struct { const char *name; VFParams p; } kPresets[VF_PRESET_COUNT] = {
    [VF_PRESET_NORMAL]        = { "Normal (Off)",  { 1.00f, 1.00f,  0.0f,  0.0f, 0.0f } },
    [VF_PRESET_DARK_LORD]     = { "Dark Lord",     { 0.62f, 0.75f,  3.0f,  1.0f, 0.0f } },
    [VF_PRESET_TENOR]         = { "Tenor",         { 0.80f, 0.84f,  0.0f,  0.0f, 0.0f } },
    [VF_PRESET_NARRATOR]      = { "Deep Narrator", { 0.82f, 0.90f,  5.0f,  1.0f, 0.0f } },
    [VF_PRESET_FEMALE]        = { "Female",        { 1.85f, 1.10f, -1.0f,  0.0f, 0.0f,  -3.5f, 3.0f, 0.05f } },
    [VF_PRESET_SMURF]         = { "Smurf",         { 1.60f, 1.45f,  0.0f,  0.0f, 0.0f } },
    [VF_PRESET_CHIPMUNK]      = { "Chipmunk",      { 2.20f, 1.80f,  0.0f,  0.0f, 0.0f } },
    [VF_PRESET_CUSTOM]        = { "Custom",        { 1.00f, 1.00f,  0.0f,  0.0f, 0.0f } },
    // Female variants: pitch and vocal-tract size fall with age; the senior voice adds a slow tremor.
    // Formant ratios kept modest (<= 1.2): larger values make the voice sound nasal/pinched.
    //                                                pitch  formant bass  gain tremor  mid    air  breath
    [VF_PRESET_FEMALE_YOUNG]  = { "Young Female",  { 2.05f, 1.14f, -2.0f,  0.0f, 0.0f,  -3.5f, 4.0f, 0.06f } },
    [VF_PRESET_FEMALE_MATURE] = { "Mature Female", { 1.65f, 1.06f,  0.0f,  0.0f, 0.0f,  -3.0f, 2.0f, 0.04f } },
    [VF_PRESET_FEMALE_SENIOR] = { "Senior Female", { 1.45f, 1.03f,  0.5f, -1.0f, 0.03f, -2.0f, 0.0f, 0.08f } },
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
           fabsf(p.bassDb) < 1e-3f && fabsf(p.gainDb) < 1e-3f && p.tremor <= 0 &&
           fabsf(p.midDb) < 1e-3f && fabsf(p.airDb) < 1e-3f && p.breath <= 0;
}

// ---------------------------------------------------------------- setup

typedef struct { float b0, b1, b2, a1, a2, z1, z2; } VFBiquad;

static void set_coeffs(VFBiquad *f, float b0, float b1, float b2, float a0, float a1, float a2) {
    f->b0 = b0 / a0; f->b1 = b1 / a0; f->b2 = b2 / a0; f->a1 = a1 / a0; f->a2 = a2 / a0;
}

static void design_low_shelf(VFBiquad *f, double sr, float hz, float dB) {
    float A = powf(10.0f, dB / 40.0f), w0 = 2.0f * (float)VF_PI * hz / (float)sr;
    float cs = cosf(w0), sA = 2.0f * sqrtf(A) * sinf(w0) / 2.0f * sqrtf(2.0f);
    set_coeffs(f, A * ((A + 1) - (A - 1) * cs + sA), 2 * A * ((A - 1) - (A + 1) * cs), A * ((A + 1) - (A - 1) * cs - sA),
               (A + 1) + (A - 1) * cs + sA, -2 * ((A - 1) + (A + 1) * cs), (A + 1) + (A - 1) * cs - sA);
}

static void design_high_shelf(VFBiquad *f, double sr, float hz, float dB) {
    float A = powf(10.0f, dB / 40.0f), w0 = 2.0f * (float)VF_PI * hz / (float)sr;
    float cs = cosf(w0), sA = 2.0f * sqrtf(A) * sinf(w0) / 2.0f * sqrtf(2.0f);
    set_coeffs(f, A * ((A + 1) + (A - 1) * cs + sA), -2 * A * ((A - 1) + (A + 1) * cs), A * ((A + 1) + (A - 1) * cs - sA),
               (A + 1) - (A - 1) * cs + sA, 2 * ((A - 1) - (A + 1) * cs), (A + 1) - (A - 1) * cs - sA);
}

static void design_peak(VFBiquad *f, double sr, float hz, float q, float dB) {
    float A = powf(10.0f, dB / 40.0f), w0 = 2.0f * (float)VF_PI * hz / (float)sr;
    float cs = cosf(w0), alpha = sinf(w0) / (2.0f * q);
    set_coeffs(f, 1 + alpha * A, -2 * cs, 1 - alpha * A, 1 + alpha / A, -2 * cs, 1 - alpha / A);
}

static inline float run_biquad(VFBiquad *f, float x) {
    float y = f->b0 * x + f->z1;
    f->z1 = f->b1 * x - f->a1 * y + f->z2;
    f->z2 = f->b2 * x - f->a2 * y;
    return y;
}

static void update_filter(VFEngine *e) {
    design_low_shelf((VFBiquad *)&e->lowShelf, e->sr, 180.0f, e->cur.bassDb);
    design_peak((VFBiquad *)&e->midBell, e->sr, 1800.0f, 0.9f, e->cur.midDb);     // the pinched/nasal band
    design_high_shelf((VFBiquad *)&e->airShelf, e->sr, 5000.0f, e->cur.airDb);
    e->outGain = powf(10.0f, e->cur.gainDb / 20.0f);
    e->breathPole = expf(-2.0f * (float)VF_PI * 2500.0f / (float)e->sr);      // breath noise is high-passed
    e->breathDecay = expf(-1.0f / (0.012f * (float)e->sr));                   // 12 ms level follower
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
    e->tremorPhase = 0;
    e->statSpeech = e->statVoiced = 0;
    e->lowShelf.z1 = e->lowShelf.z2 = e->midBell.z1 = e->midBell.z2 = e->airShelf.z1 = e->airShelf.z2 = 0;
    e->noise = 0x9E3779B9u;
    e->breathHp = e->breathHpIn = e->breathEnv = 0;
}

int vf_voiced_percent(VFEngine *e) {
    int pct = e->statSpeech ? (int)(100u * e->statVoiced / e->statSpeech) : 0;
    e->statSpeech = e->statVoiced = 0;
    return pct;
}

// ---------------------------------------------------------------- pitch detection (YIN)

// Returns the period in samples of the most recent audio, 0 if unvoiced, -1 if silent.
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
    if (energy / N < VF_SILENCE_RMS * VF_SILENCE_RMS) return -1;

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
    if (tau < 0) {
        // no clear dip: take the deepest one if it is still reasonably periodic
        int best = tmin;
        for (int t = tmin + 1; t <= tmax; t++)
            if (e->cmnd[t] < e->cmnd[best]) best = t;
        if (e->cmnd[best] < VF_YIN_FALLBACK) tau = best;
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
    if (T >= 0) e->statSpeech++;
    if (T > 0) {
        e->statVoiced++;
        // avoid sudden octave jumps while a voiced segment continues
        if (e->voiced) {
            double r = T / e->T;
            if (r > 1.8 && r < 2.2) T *= 0.5;
            else if (r > 0.45 && r < 0.55) T *= 2.0;
        }
        e->T = e->voiced ? 0.6 * T + 0.4 * e->T : T;
        e->voiced = 1;
        e->voicedHold = VF_VOICED_HOLD;
    } else if (e->voicedHold > 0) {
        e->voicedHold--;                // bridge short detector dropouts inside a vowel
        if (T == 0) e->statVoiced++;    // counted as tracked: the held period is used
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
    if (e->cur.tremor > 0) {
        e->tremorPhase += 2.0f * (float)VF_PI * VF_TREMOR_HZ / (float)e->sr;
        if (e->tremorPhase > 2.0f * (float)VF_PI) e->tremorPhase -= 2.0f * (float)VF_PI;
        pitch *= 1.0f + e->cur.tremor * sinf(e->tremorPhase);
    }
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
        y = run_biquad((VFBiquad *)&e->lowShelf, y);
        y = run_biquad((VFBiquad *)&e->midBell, y);
        y = run_biquad((VFBiquad *)&e->airShelf, y);
        if (e->cur.breath > 0) {
            // aspiration: high-passed noise following the voice's own level, so it sits under
            // the speech and vanishes in silence
            e->noise ^= e->noise << 13; e->noise ^= e->noise >> 17; e->noise ^= e->noise << 5;
            float n = (float)(int32_t)e->noise * (1.0f / 2147483648.0f);
            float hp = n - e->breathHpIn + e->breathPole * e->breathHp;
            e->breathHpIn = n; e->breathHp = hp;
            float a = fabsf(y);
            e->breathEnv = a > e->breathEnv ? a : e->breathEnv * e->breathDecay;
            y += e->cur.breath * e->breathEnv * hp * 3.0f;
        }
        buf[i] = soft_limit(y * e->outGain);
    }
}
