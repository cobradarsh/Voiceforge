// VoiceForge DSP engine.
//
// Phase-vocoder pitch shifter (4x overlap STFT) using peak-locked shifting
// (Laroche & Dolson): every spectral peak is moved together with the bins
// around it and their phases are rotated as one unit, which avoids the warbly
// "phasiness" of shifting each bin independently.
//
// A cepstral spectral envelope separates the voice's pitch from its formants
// (the "size" of the vocal tract) so they can be moved independently. Shifting
// pitch alone gives the classic chipmunk/monster sound; shifting pitch and
// formants by different amounts is what makes a voice read as a different speaker.
#include "VFDSP.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define VF_PI 3.14159265358979323846f
#define VF_OSAMP 4

struct VFEngine {
    double sr;
    int N, half, hop, latency, rover, cepCut;
    int log2N;

    float *win;
    float *inFifo, *outFifo, *outAccum;
    float *re, *im, *cre, *cim;
    float *lastPhase, *anaMag, *anaPhase, *outRe, *outIm, *env;
    float *prevSynPhase, *curSynPhase;   // synthesis phase of each analysis peak (prev/current frame)
    unsigned char *prevIsPeak, *curIsPeak;
    int *peaks;
    float *twr, *twi;
    int *bitrev;

    VFParams cur, pending;
    int dirty;
    int active;

    // low-shelf biquad
    float b0, b1, b2, a1, a2, z1, z2;
    float outGain;
    float levelGain;   // smoothed loudness compensation
};

static const struct { const char *name; VFParams p; } kPresets[VF_PRESET_COUNT] = {
    [VF_PRESET_NORMAL]    = { "Normal (Off)",  { 1.00f, 1.00f,  0.0f, 0.0f } },
    [VF_PRESET_DARK_LORD] = { "Dark Lord",     { 0.62f, 0.75f,  3.0f, 1.0f } },
    [VF_PRESET_TENOR]     = { "Tenor",         { 0.80f, 0.80f,  0.0f, 0.0f } },
    [VF_PRESET_NARRATOR]  = { "Deep Narrator", { 0.82f, 0.90f,  5.0f, 1.0f } },
    [VF_PRESET_FEMALE]    = { "Female",        { 1.70f, 1.18f, -2.0f, 0.0f } },
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

// ---------------------------------------------------------------- FFT

static void fft(VFEngine *e, float *re, float *im, int inverse) {
    int N = e->N;
    for (int i = 0; i < N; i++) {
        int j = e->bitrev[i];
        if (j > i) {
            float t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    for (int size = 2; size <= N; size <<= 1) {
        int h = size >> 1, step = N / size;
        for (int i = 0; i < N; i += size) {
            for (int j = 0; j < h; j++) {
                float wr = e->twr[j * step];
                float wi = inverse ? e->twi[j * step] : -e->twi[j * step];
                int a = i + j, b = a + h;
                float tr = re[b] * wr - im[b] * wi;
                float ti = re[b] * wi + im[b] * wr;
                re[b] = re[a] - tr; im[b] = im[a] - ti;
                re[a] += tr;        im[a] += ti;
            }
        }
    }
}

// ---------------------------------------------------------------- setup

static void update_filter(VFEngine *e) {
    float A = powf(10.0f, e->cur.bassDb / 40.0f);
    float w0 = 2.0f * VF_PI * 180.0f / (float)e->sr;
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
    // ~32 ms analysis window (largest power of two that fits), 256..2048 samples
    int N = 256;
    while (N * 2 <= sampleRate * 0.032 && N < 2048) N *= 2;
    e->N = N; e->half = N / 2; e->hop = N / VF_OSAMP; e->latency = N - e->hop;
    for (e->log2N = 0; (1 << e->log2N) < N; e->log2N++) {}
    // cepstral lifter: keep quefrencies below ~1.6 ms (above the period of any speaking pitch)
    e->cepCut = (int)(sampleRate * 0.0016);
    if (e->cepCut < 8) e->cepCut = 8;
    if (e->cepCut > e->half - 1) e->cepCut = e->half - 1;

    size_t hn = (size_t)e->half + 1;
    float **bufsN[] = { &e->win, &e->inFifo, &e->outFifo, &e->re, &e->im, &e->cre, &e->cim, &e->twr, &e->twi };
    for (size_t i = 0; i < sizeof bufsN / sizeof *bufsN; i++)
        if (!(*bufsN[i] = calloc(N, sizeof(float)))) goto fail;
    if (!(e->outAccum = calloc(2 * N, sizeof(float)))) goto fail;
    float **bufsH[] = { &e->lastPhase, &e->anaMag, &e->anaPhase, &e->outRe, &e->outIm, &e->env,
                        &e->prevSynPhase, &e->curSynPhase };
    for (size_t i = 0; i < sizeof bufsH / sizeof *bufsH; i++)
        if (!(*bufsH[i] = calloc(hn, sizeof(float)))) goto fail;
    if (!(e->prevIsPeak = calloc(hn, 1)) || !(e->curIsPeak = calloc(hn, 1))) goto fail;
    if (!(e->peaks = calloc(hn, sizeof(int)))) goto fail;
    if (!(e->bitrev = calloc(N, sizeof(int)))) goto fail;

    for (int i = 0; i < N; i++) {
        e->win[i] = 0.5f - 0.5f * cosf(2.0f * VF_PI * i / N);
        e->twr[i] = cosf(2.0f * VF_PI * i / N);
        e->twi[i] = sinf(2.0f * VF_PI * i / N);
        int r = 0;
        for (int b = 0; b < e->log2N; b++) if (i & (1 << b)) r |= 1 << (e->log2N - 1 - b);
        e->bitrev[i] = r;
    }
    e->cur = e->pending = kPresets[VF_PRESET_NORMAL].p;
    update_filter(e);
    vf_reset(e);
    return e;
fail:
    vf_destroy(e);
    return NULL;
}

void vf_destroy(VFEngine *e) {
    if (!e) return;
    float *all[] = { e->win, e->inFifo, e->outFifo, e->outAccum, e->re, e->im, e->cre, e->cim,
                     e->lastPhase, e->anaMag, e->anaPhase, e->outRe, e->outIm, e->env,
                     e->prevSynPhase, e->curSynPhase, e->twr, e->twi };
    for (size_t i = 0; i < sizeof all / sizeof *all; i++) free(all[i]);
    free(e->prevIsPeak);
    free(e->curIsPeak);
    free(e->peaks);
    free(e->bitrev);
    free(e);
}

double vf_sample_rate(const VFEngine *e) { return e->sr; }
int vf_latency_samples(const VFEngine *e) { return e->latency; }

void vf_set_params(VFEngine *e, VFParams p) {
    p.pitch = clampf(p.pitch, 0.25f, 4.0f);
    p.formant = clampf(p.formant, 0.25f, 4.0f);
    e->pending = p;
    e->dirty = 1;
}

void vf_reset(VFEngine *e) {
    int N = e->N;
    memset(e->inFifo, 0, N * sizeof(float));
    memset(e->outFifo, 0, N * sizeof(float));
    memset(e->outAccum, 0, 2 * N * sizeof(float));
    memset(e->lastPhase, 0, (e->half + 1) * sizeof(float));
    memset(e->prevSynPhase, 0, (e->half + 1) * sizeof(float));
    memset(e->prevIsPeak, 0, e->half + 1);
    e->levelGain = 1.0f;
    e->rover = e->latency;
    e->z1 = e->z2 = 0;
}

// ---------------------------------------------------------------- processing

// Smoothed spectral envelope (linear magnitude) from anaMag via real cepstrum.
static void compute_envelope(VFEngine *e) {
    int N = e->N, half = e->half, cut = e->cepCut;
    for (int k = 0; k <= half; k++) {
        e->cre[k] = logf(e->anaMag[k] + 1e-9f);
        e->cim[k] = 0;
    }
    for (int k = half + 1; k < N; k++) { e->cre[k] = e->cre[N - k]; e->cim[k] = 0; }
    fft(e, e->cre, e->cim, 1);
    // lifter (cepstrum of a real even sequence is real and even)
    for (int n = 0; n < N; n++) {
        float w = (n < cut || n > N - cut) ? 1.0f / N : 0.0f;
        e->cre[n] *= w;
        e->cim[n] = 0;
    }
    fft(e, e->cre, e->cim, 0);
    for (int k = 0; k <= half; k++) e->env[k] = expf(e->cre[k]);
}

static float env_at(const VFEngine *e, float pos) {
    if (pos <= 0) return e->env[0];
    if (pos >= e->half) return e->env[e->half];
    int i = (int)pos;
    float f = pos - i;
    return e->env[i] * (1 - f) + e->env[i + 1] * f;
}

static void process_frame(VFEngine *e) {
    int N = e->N, half = e->half;
    float expct = 2.0f * VF_PI * e->hop / N;
    float pitch = e->cur.pitch, formant = e->cur.formant;
    int separate = fabsf(pitch - formant) > 1e-3f;

    // zero-phase windowing: rotate the frame so its centre is at index 0, which makes
    // bin phases independent of the bin index (peaks can then move between bins freely)
    for (int k = 0; k < N; k++) {
        int n = (k + half) & (N - 1);
        e->re[k] = e->inFifo[n] * e->win[n];
        e->im[k] = 0;
    }
    fft(e, e->re, e->im, 0);

    float maxMag = 0;
    for (int k = 0; k <= half; k++) {
        float re = e->re[k], im = e->im[k];
        e->anaMag[k] = sqrtf(re * re + im * im);
        e->anaPhase[k] = atan2f(im, re);
        if (e->anaMag[k] > maxMag) maxMag = e->anaMag[k];
    }

    if (separate) compute_envelope(e);

    // find spectral peaks
    int np = 0;
    float floorMag = maxMag * 1e-4f;
    memset(e->curIsPeak, 0, half + 1);
    for (int k = 1; k < half; k++) {
        float m = e->anaMag[k];
        if (m > floorMag && m > e->anaMag[k - 1] && m >= e->anaMag[k + 1]) {
            e->peaks[np++] = k;
            e->curIsPeak[k] = 1;
        }
    }

    memset(e->outRe, 0, (half + 1) * sizeof(float));
    memset(e->outIm, 0, (half + 1) * sizeof(float));
    int regionStart = 0;
    for (int p = 0; p < np; p++) {
        int kp = e->peaks[p];
        // region of influence: up to the magnitude minimum before the next peak
        int regionEnd = half;
        if (p + 1 < np) {
            regionEnd = kp;
            for (int k = kp + 1; k < e->peaks[p + 1]; k++)
                if (e->anaMag[k] < e->anaMag[regionEnd]) regionEnd = k;
        }

        // true frequency of the peak (in bins) from the phase advance since the last frame
        float d = e->anaPhase[kp] - e->lastPhase[kp] - kp * expct;
        d -= 2.0f * VF_PI * floorf(d / (2.0f * VF_PI) + 0.5f);
        float fTrue = kp + VF_OSAMP * d / (2.0f * VF_PI);
        float fNew = fTrue * pitch;
        int shift = (int)floorf(fNew + 0.5f) - kp;

        // continue the synthesis phase of the matching peak from the previous frame
        int match = -1;
        for (int o = 0; o <= 2 && match < 0; o++) {
            if (kp - o >= 0 && e->prevIsPeak[kp - o]) match = kp - o;
            else if (kp + o <= half && e->prevIsPeak[kp + o]) match = kp + o;
        }
        float synPhase = match >= 0 ? e->prevSynPhase[match] + fNew * expct
                                    : e->anaPhase[kp] * pitch;
        synPhase = fmodf(synPhase, 2.0f * VF_PI);
        e->curSynPhase[kp] = synPhase;
        float rot = synPhase - e->anaPhase[kp];

        for (int k = regionStart; k <= regionEnd; k++) {
            int j = k + shift;
            if (j < 0 || j > half) continue;
            float m = e->anaMag[k];
            if (separate) m *= env_at(e, j / formant) / (e->env[k] + 1e-9f);
            float ph = e->anaPhase[k] + rot;
            e->outRe[j] += m * cosf(ph);
            e->outIm[j] += m * sinf(ph);
        }
        regionStart = regionEnd + 1;
    }
    memcpy(e->lastPhase, e->anaPhase, (half + 1) * sizeof(float));
    memcpy(e->prevSynPhase, e->curSynPhase, (half + 1) * sizeof(float));
    memcpy(e->prevIsPeak, e->curIsPeak, half + 1);

    // keep loudness equal to the input: shifting changes how many harmonics fall
    // under the formants, so match frame energy with a smoothed gain
    float eIn = 0, eOut = 0;
    for (int k = 0; k <= half; k++) {
        eIn += e->anaMag[k] * e->anaMag[k];
        eOut += e->outRe[k] * e->outRe[k] + e->outIm[k] * e->outIm[k];
    }
    if (eOut > 1e-12f && eIn > 1e-12f) {
        float target = clampf(sqrtf(eIn / eOut), 0.25f, 4.0f);
        e->levelGain += (target - e->levelGain) * 0.15f;
    }

    // synthesis
    for (int k = 0; k <= half; k++) { e->re[k] = e->outRe[k] * e->levelGain; e->im[k] = e->outIm[k] * e->levelGain; }
    e->im[0] = e->im[half] = 0;
    for (int k = half + 1; k < N; k++) { e->re[k] = e->re[N - k]; e->im[k] = -e->im[N - k]; }
    fft(e, e->re, e->im, 1);

    // overlap-add: periodic Hann analysis*synthesis at 4x overlap sums to 1.5
    float scale = 1.0f / (N * 1.5f);
    for (int k = 0; k < N; k++) e->outAccum[k] += e->re[(k + half) & (N - 1)] * e->win[k] * scale;
    memcpy(e->outFifo, e->outAccum, e->hop * sizeof(float));
    memmove(e->outAccum, e->outAccum + e->hop, N * sizeof(float));
    memset(e->outAccum + N, 0, e->hop * sizeof(float));
    memmove(e->inFifo, e->inFifo + e->hop, e->latency * sizeof(float));
}

static inline float soft_limit(float x) {
    float a = fabsf(x);
    if (a <= 0.8f) return x;
    float y = 0.8f + 0.2f * tanhf((a - 0.8f) / 0.2f);
    return x < 0 ? -y : y;
}

void vf_process(VFEngine *e, float *buf, int n) {
    for (int i = 0; i < n; i++) {
        if (e->rover == e->latency && e->dirty) {
            int wasActive = e->active;
            e->cur = e->pending;
            e->dirty = 0;
            e->active = !is_identity(e->cur);
            update_filter(e);
            if (e->active && !wasActive) vf_reset(e);
        }
        if (!e->active) continue;

        e->inFifo[e->rover] = buf[i];
        float y = e->outFifo[e->rover - e->latency];
        if (++e->rover >= e->N) {
            e->rover = e->latency;
            process_frame(e);
        }
        // low shelf (transposed direct form II)
        float s = e->b0 * y + e->z1;
        e->z1 = e->b1 * y - e->a1 * s + e->z2;
        e->z2 = e->b2 * y - e->a2 * s;
        buf[i] = soft_limit(s * e->outGain);
    }
}
