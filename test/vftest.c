// Desktop test: runs every preset over a 16-bit PCM WAV and writes one WAV per preset.
//   vftest input.wav outdir
#include "../VFDSP.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int read_wav(const char *path, float **out, int *frames, int *rate) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    uint8_t h[12];
    if (fread(h, 1, 12, f) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) { fclose(f); return -1; }
    int channels = 0, bits = 0;
    for (;;) {
        uint8_t ch[8];
        if (fread(ch, 1, 8, f) != 8) { fclose(f); return -1; }
        uint32_t size = ch[4] | ch[5] << 8 | ch[6] << 16 | (uint32_t)ch[7] << 24;
        if (!memcmp(ch, "fmt ", 4)) {
            uint8_t fmt[40] = {0};
            fread(fmt, 1, size < 40 ? size : 40, f);
            if (size > 40) fseek(f, size - 40, SEEK_CUR);
            channels = fmt[2] | fmt[3] << 8;
            *rate = fmt[4] | fmt[5] << 8 | fmt[6] << 16 | fmt[7] << 24;
            bits = fmt[14] | fmt[15] << 8;
        } else if (!memcmp(ch, "data", 4)) {
            if (bits != 16 || channels < 1) { fclose(f); return -1; }
            int n = size / (2 * channels);
            int16_t *raw = malloc(size);
            n = (int)fread(raw, 2 * channels, n, f);
            *out = malloc(n * sizeof(float));
            for (int i = 0; i < n; i++) (*out)[i] = raw[i * channels] / 32768.0f;
            free(raw);
            *frames = n;
            fclose(f);
            return 0;
        } else {
            fseek(f, size + (size & 1), SEEK_CUR);
        }
    }
}

static void put32(FILE *f, uint32_t v) { uint8_t b[4] = { v, v >> 8, v >> 16, v >> 24 }; fwrite(b, 1, 4, f); }
static void put16(FILE *f, uint16_t v) { uint8_t b[2] = { v, v >> 8 }; fwrite(b, 1, 2, f); }

static void write_wav(const char *path, const float *x, int n, int rate) {
    FILE *f = fopen(path, "wb");
    fwrite("RIFF", 1, 4, f); put32(f, 36 + n * 2); fwrite("WAVEfmt ", 1, 8, f);
    put32(f, 16); put16(f, 1); put16(f, 1); put32(f, rate); put32(f, rate * 2); put16(f, 2); put16(f, 16);
    fwrite("data", 1, 4, f); put32(f, n * 2);
    for (int i = 0; i < n; i++) {
        float v = x[i] * 32767.0f;
        put16(f, (uint16_t)(int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v));
    }
    fclose(f);
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: vftest input.wav outdir\n"); return 1; }
    float *in; int n, rate;
    if (read_wav(argv[1], &in, &n, &rate)) { fprintf(stderr, "cannot read %s (need 16-bit PCM)\n", argv[1]); return 1; }
    float *buf = malloc(n * sizeof(float));
    for (int p = 0; p < VF_PRESET_COUNT; p++) {
        VFEngine *e = vf_create(rate);
        vf_set_params(e, vf_preset_params((VFPreset)p, 1.3f, 1.1f));
        memcpy(buf, in, n * sizeof(float));
        // feed in call-sized chunks of varying length, like a real render callback
        int sizes[] = { 160, 256, 480, 512, 1024, 37 };
        for (int i = 0, c = 0; i < n; c++) {
            int len = sizes[c % 6];
            if (len > n - i) len = n - i;
            vf_process(e, buf + i, len);
            i += len;
        }
        char path[1024];
        snprintf(path, sizeof path, "%s/%d_%s.wav", argv[2], p, vf_preset_name((VFPreset)p));
        for (char *s = strrchr(path, '/') + 1; *s; s++) if (*s == ' ' || *s == '(' || *s == ')') *s = '_';
        write_wav(path, buf, n, rate);
        printf("%-14s rate=%d latency=%d samples (%.1f ms) -> %s\n", vf_preset_name((VFPreset)p), rate,
               vf_latency_samples(e), 1000.0 * vf_latency_samples(e) / rate, path);
        vf_destroy(e);
    }
    return 0;
}
