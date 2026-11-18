#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "neaacdec.h"

static unsigned char *read_file(const char *path, size_t *size)
{
    FILE *file = fopen(path, "rb");
    unsigned char *data;
    long length;

    if (!file)
        return NULL;
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return NULL;
    }
    length = ftell(file);
    if (length < 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }

    data = (unsigned char *)malloc((size_t)length ? (size_t)length : 1);
    if (!data) {
        fclose(file);
        return NULL;
    }
    if (length > 0 && fread(data, 1, (size_t)length, file) != (size_t)length) {
        free(data);
        fclose(file);
        return NULL;
    }
    fclose(file);
    *size = (size_t)length;
    return data;
}

int main(int argc, char **argv)
{
    unsigned char *data;
    size_t size = 0;
    size_t offset;
    NeAACDecHandle decoder;
    NeAACDecConfigurationPtr config;
    unsigned long samplerate = 0;
    unsigned char channels = 0;
    long init_consumed;
    unsigned long total_samples = 0;
    unsigned frames = 0;
    FILE *out;

    if (argc != 3) {
        fprintf(stderr, "usage: %s INPUT.aac OUTPUT.f32\n", argv[0]);
        return 64;
    }

    data = read_file(argv[1], &size);
    if (!data) {
        fprintf(stderr, "unable to read %s\n", argv[1]);
        return 4;
    }
    out = fopen(argv[2], "wb");
    if (!out) {
        free(data);
        fprintf(stderr, "unable to open %s\n", argv[2]);
        return 4;
    }

    decoder = NeAACDecOpen();
    if (!decoder) {
        fclose(out);
        free(data);
        return 3;
    }
    config = NeAACDecGetCurrentConfiguration(decoder);
    if (!config) {
        NeAACDecClose(decoder);
        fclose(out);
        free(data);
        return 3;
    }
    config->defObjectType = LC;
    config->defSampleRate = 48000;
    config->outputFormat = FAAD_FMT_FLOAT;
    config->downMatrix = 0;
    config->useOldADTSFormat = 0;
    config->dontUpSampleImplicitSBR = 1;
    if (!NeAACDecSetConfiguration(decoder, config)) {
        NeAACDecClose(decoder);
        fclose(out);
        free(data);
        return 3;
    }

    init_consumed = NeAACDecInit(decoder, data, (unsigned long)size,
                                &samplerate, &channels);
    if (init_consumed < 0 || (size_t)init_consumed > size) {
        fprintf(stderr, "init failed: %ld\n", init_consumed);
        NeAACDecClose(decoder);
        fclose(out);
        free(data);
        return 3;
    }

    /* Mirror the CLI so the encoder-delay frame is included in the dump. */
    NeAACDecPostSeekReset(decoder, 1);

    offset = (size_t)init_consumed;
    while (offset < size && frames < 10000) {
        NeAACDecFrameInfo info;
        void *pcm;

        memset(&info, 0, sizeof(info));
        pcm = NeAACDecDecode(decoder, &info, data + offset,
                            (unsigned long)(size - offset));
        frames++;

        if ((size_t)info.bytesconsumed > size - offset) {
            fprintf(stderr,
                    "bytesconsumed overflow at input offset %zu: %lu > %zu\n",
                    offset, info.bytesconsumed, size - offset);
            NeAACDecClose(decoder);
            fclose(out);
            free(data);
            return 3;
        }
        if (info.error) {
            fprintf(stderr, "decode error %u at input offset %zu\n",
                    info.error, offset);
            NeAACDecClose(decoder);
            fclose(out);
            free(data);
            return 3;
        }
        if (pcm && info.samples > 0) {
            size_t nbytes = (size_t)info.samples * sizeof(float);
            if (fwrite(pcm, 1, nbytes, out) != nbytes) {
                NeAACDecClose(decoder);
                fclose(out);
                free(data);
                return 4;
            }
            total_samples += info.samples;
        }
        if (info.bytesconsumed == 0)
            break;
        offset += (size_t)info.bytesconsumed;
    }

    fclose(out);
    NeAACDecClose(decoder);
    free(data);
    fprintf(stderr,
            "decoded %u frames, %lu scalar samples, %zu/%zu input bytes\n",
            frames, total_samples, offset, size);
    return 0;
}
