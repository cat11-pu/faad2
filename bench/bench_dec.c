#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <neaacdec.h>

static unsigned long fnv1a(const void *data, size_t len, unsigned long h)
{
    const unsigned char *p = data;
    for (size_t i = 0; i < len; i++) {
        h ^= p[i];
        h *= 1099511628211UL;
    }
    return h;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s input.aac iters [out.pcm]\n", argv[0]);
        return 1;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    int iters = atoi(argv[2]);
    FILE *of = argc > 3 ? fopen(argv[3], "wb") : NULL;

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *buf = malloc(fsize);
    if (fread(buf, 1, fsize, f) != (size_t)fsize) return 1;
    fclose(f);

    NeAACDecHandle dec = NeAACDecOpen();
    NeAACDecConfigurationPtr cfg = NeAACDecGetCurrentConfiguration(dec);
    cfg->outputFormat = FAAD_FMT_FLOAT;
    cfg->defSampleRate = 0;
    cfg->downMatrix = 0;
    NeAACDecSetConfiguration(dec, cfg);

    unsigned long sr = 0;
    unsigned char ch = 0;
    long consumed = NeAACDecInit(dec, buf, fsize, &sr, &ch);
    if (consumed < 0) { fprintf(stderr, "NeAACDecInit failed\n"); return 1; }

    double best = 1e30, total = 0;
    unsigned long hash = 1469598103934665603UL;
    long total_samples = 0;
    int frames = 0;
    for (int it = 0; it < iters; it++) {
        long off = consumed;
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        while (off < fsize) {
            NeAACDecFrameInfo fi;
            void *out = NeAACDecDecode(dec, &fi, buf + off, fsize - off);
            if (fi.bytesconsumed == 0) break;
            if (fi.error != 0) {
                fprintf(stderr, "decode error %d: %s\n", fi.error,
                        NeAACDecGetErrorMessage(fi.error));
                return 1;
            }
            if (of && it == 0)
                fwrite(out, fi.channels * 4, fi.samples, of);
            if (it == 0) {
                hash = fnv1a(out, (size_t)fi.samples * fi.channels * 4, hash);
                total_samples += (long)fi.samples * fi.channels;
                frames++;
            }
            off += fi.bytesconsumed;
        }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double ms = (t1.tv_sec - t0.tv_sec) * 1000.0 +
                    (t1.tv_nsec - t0.tv_nsec) / 1.0e6;
        if (ms < best) best = ms;
        total += ms;
        NeAACDecPostSeekReset(dec, 0);
    }
    if (of) fclose(of);
    NeAACDecClose(dec);
    free(buf);
    printf("%s out_sr=%lu ch=%u frames=%d samples=%ld best_ms=%.3f avg_ms=%.3f hash=%016lx\n",
           argv[1], sr, ch, frames, total_samples, best, total / iters, hash);
    return 0;
}
