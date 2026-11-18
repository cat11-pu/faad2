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

static const char *base_name(const char *path)
{
    const char *slash = strrchr(path, '/');
    const char *backslash = strrchr(path, '\\');
    const char *last = slash;
    if (backslash && (!last || backslash > last))
        last = backslash;
    return last ? last + 1 : path;
}

static int fail(const char *case_id, const char *reason, size_t offset,
                unsigned long consumed, size_t remaining, unsigned error)
{
    printf("STATUS=fail case=%s reason=%s offset=%zu consumed=%lu "
           "remaining=%zu error=%u\n",
           case_id, reason, offset, consumed, remaining, error);
    fflush(stdout);
    return 2;
}

static int probe_file(const char *path, const char *case_id)
{
    unsigned char *data;
    size_t size = 0;
    size_t offset;
    NeAACDecHandle decoder;
    NeAACDecConfigurationPtr config;
    unsigned long samplerate = 0;
    unsigned char channels = 0;
    long init_consumed;
    unsigned calls = 0;
    unsigned errors = 0;
    unsigned max_channels = 0;
    unsigned long max_samples = 0;
    uint64_t sink = 1469598103934665603ULL;

    data = read_file(path, &size);
    if (!data) {
        fprintf(stderr, "unable to read %s\n", path);
        return 4;
    }

    decoder = NeAACDecOpen();
    if (!decoder) {
        free(data);
        fprintf(stderr, "NeAACDecOpen failed\n");
        return 3;
    }

    config = NeAACDecGetCurrentConfiguration(decoder);
    if (!config) {
        NeAACDecClose(decoder);
        free(data);
        fprintf(stderr, "NeAACDecGetCurrentConfiguration failed\n");
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
        free(data);
        fprintf(stderr, "NeAACDecSetConfiguration failed\n");
        return 3;
    }

    init_consumed = NeAACDecInit(decoder, data, (unsigned long)size,
                                &samplerate, &channels);
    if (init_consumed < 0) {
        printf("STATUS=init_rejected case=%s size=%zu\n", case_id, size);
        fflush(stdout);
        NeAACDecClose(decoder);
        free(data);
        return 0;
    }
    if ((size_t)init_consumed > size) {
        NeAACDecClose(decoder);
        free(data);
        return fail(case_id, "init_consumed_overflow", 0,
                    (unsigned long)init_consumed, size, 0);
    }

    offset = (size_t)init_consumed;
    while (offset < size && calls < 4096) {
        NeAACDecFrameInfo info;
        void *pcm;

        memset(&info, 0, sizeof(info));
        pcm = NeAACDecDecode(decoder, &info, data + offset,
                            (unsigned long)(size - offset));
        calls++;

        if ((size_t)info.bytesconsumed > size - offset) {
            NeAACDecClose(decoder);
            free(data);
            return fail(case_id, "bytesconsumed_overflow", offset,
                        info.bytesconsumed, size - offset, info.error);
        }
        if (info.channels > 8) {
            NeAACDecClose(decoder);
            free(data);
            return fail(case_id, "channel_count_overflow", offset,
                        info.bytesconsumed, size - offset, info.error);
        }
        if (info.samples > 32768UL) {
            NeAACDecClose(decoder);
            free(data);
            return fail(case_id, "sample_count_overflow", offset,
                        info.bytesconsumed, size - offset, info.error);
        }

        if (info.error)
            errors++;
        if (info.channels > max_channels)
            max_channels = info.channels;
        if (info.samples > max_samples)
            max_samples = info.samples;

        /* Touch every sample the decoder claims to have produced. */
        if (pcm && info.samples > 0) {
            const unsigned char *bytes = (const unsigned char *)pcm;
            size_t i;
            size_t nbytes = (size_t)info.samples * sizeof(float);
            for (i = 0; i < nbytes; i++)
                sink = (sink ^ bytes[i]) * 1099511628211ULL;
        }

        if (info.bytesconsumed == 0)
            break;
        offset += (size_t)info.bytesconsumed;
    }

    printf("STATUS=ok case=%s size=%zu init=%ld decoded=%zu calls=%u "
           "errors=%u max_channels=%u max_samples=%lu sink=%016llx\n",
           case_id, size, init_consumed, offset, calls, errors,
           max_channels, max_samples, (unsigned long long)sink);
    fflush(stdout);
    NeAACDecClose(decoder);
    free(data);
    return 0;
}

int main(int argc, char **argv)
{
    int i;

    if (argc < 2) {
        fprintf(stderr, "usage: %s FILE [CASE_ID]\n", argv[0]);
        fprintf(stderr, "       %s --batch FILE...\n", argv[0]);
        return 64;
    }
    if (strcmp(argv[1], "--batch") == 0) {
        for (i = 2; i < argc; i++) {
            int result = probe_file(argv[i], base_name(argv[i]));
            if (result != 0)
                return result;
        }
        printf("STATUS=batch_ok cases=%d\n", argc - 2);
        fflush(stdout);
        return 0;
    }
    return probe_file(argv[1], argc > 2 ? argv[2] : "?");
}
