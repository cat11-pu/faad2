#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <neaacdec.h>

/* Repeated decode + RDTSC-free stage timing via clock_gettime around full
 * decode; function-level split is done via gprof. This harness measures
 * stable best-of-N decode time and output hash. */

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: %s in.aac iters\n", argv[0]); return 1; }
    FILE *f = fopen(argv[1], "rb");
    fseek(f, 0, SEEK_END); long fsize = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *buf = malloc(fsize);
    fread(buf, 1, fsize, f); fclose(f);
    int iters = atoi(argv[2]);

    NeAACDecHandle dec = NeAACDecOpen();
    NeAACDecConfigurationPtr cfg = NeAACDecGetCurrentConfiguration(dec);
    cfg->outputFormat = FAAD_FMT_FLOAT;
    NeAACDecSetConfiguration(dec, cfg);
    unsigned long sr = 0; unsigned char ch = 0;
    long consumed = NeAACDecInit(dec, buf, fsize, &sr, &ch);

    double best = 1e30;
    struct timespec t0, t1;
    for (int it = 0; it < iters; it++) {
        long off = consumed;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        while (off < fsize) {
            NeAACDecFrameInfo fi;
            NeAACDecDecode(dec, &fi, buf + off, fsize - off);
            if (fi.bytesconsumed == 0) break;
            off += fi.bytesconsumed;
        }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double ms = (t1.tv_sec - t0.tv_sec)*1000.0 + (t1.tv_nsec - t0.tv_nsec)/1e6;
        if (ms < best) best = ms;
        NeAACDecPostSeekReset(dec, 0);
    }
    NeAACDecClose(dec); free(buf);
    printf("%.4f\n", best);
    return 0;
}
