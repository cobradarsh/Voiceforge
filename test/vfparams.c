// Desktop test: process a 16-bit PCM WAV with explicit parameters.
//   vfparams in.wav out.wav pitch formant [bassDb] [gainDb]
#include "../VFDSP.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc < 5) { fprintf(stderr, "usage: vfparams in.wav out.wav pitch formant [bassDb] [gainDb]\n"); return 1; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc(size);
    if (fread(d, 1, size, f) != (size_t)size) { fclose(f); return 1; }
    fclose(f);
    // minimal RIFF walk: find fmt and data
    int rate = 0, ch = 1;
    long p = 12, dataOff = 0, dataLen = 0;
    while (p + 8 <= size) {
        uint32_t len = d[p + 4] | d[p + 5] << 8 | d[p + 6] << 16 | (uint32_t)d[p + 7] << 24;
        if (!memcmp(d + p, "fmt ", 4)) { ch = d[p + 10] | d[p + 11] << 8; rate = d[p + 12] | d[p + 13] << 8 | d[p + 14] << 16 | d[p + 15] << 24; }
        if (!memcmp(d + p, "data", 4)) { dataOff = p + 8; dataLen = len; break; }
        p += 8 + len + (len & 1);
    }
    int n = (int)(dataLen / (2 * ch));
    float *x = malloc(n * sizeof(float));
    for (int i = 0; i < n; i++) x[i] = ((int16_t)(d[dataOff + 2 * ch * i] | d[dataOff + 2 * ch * i + 1] << 8)) / 32768.0f;

    VFParams prm = { (float)atof(argv[3]), (float)atof(argv[4]), argc > 5 ? (float)atof(argv[5]) : 0, argc > 6 ? (float)atof(argv[6]) : 0 };
    VFEngine *e = vf_create(rate);
    vf_set_params(e, prm);
    for (int i = 0; i < n; i += 240) vf_process(e, x + i, n - i < 240 ? n - i : 240);
    vf_destroy(e);

    FILE *o = fopen(argv[2], "wb");
    uint8_t h[44] = "RIFF\0\0\0\0WAVEfmt \x10\0\0\0\x01\0\x01\0\0\0\0\0\0\0\0\0\x02\0\x10\0data";
    uint32_t v;
    v = 36 + n * 2; memcpy(h + 4, &v, 4);
    v = rate; memcpy(h + 24, &v, 4);
    v = rate * 2; memcpy(h + 28, &v, 4);
    v = n * 2; memcpy(h + 40, &v, 4);
    fwrite(h, 1, 44, o);
    for (int i = 0; i < n; i++) {
        float s = x[i] * 32767.0f;
        int16_t q = (int16_t)(s > 32767 ? 32767 : s < -32768 ? -32768 : s);
        fwrite(&q, 2, 1, o);
    }
    fclose(o);
    return 0;
}
