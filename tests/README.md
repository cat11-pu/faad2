# Audio property tests

This directory contains a regression harness for malformed ADTS/AAC input and
an independent reference-decoder comparison.

## Build and run

Build with ASan, UBSan, and LeakSanitizer:

```sh
cmake -S . -B build-asan \
  -DCMAKE_BUILD_TYPE=Debug \
  -DFAAD_BUILD_CLI=OFF \
  -DFAAD_BUILD_TESTS=ON \
  -DBUILD_SHARED_LIBS=OFF \
  -DCMAKE_C_FLAGS='-O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=all' \
  -DCMAKE_EXE_LINKER_FLAGS='-fsanitize=address,undefined'
cmake --build build-asan --target decode_probe decode_dump -j
python3 tests/run_property_tests.py \
  --probe build-asan/tests/decode_probe \
  --dump build-asan/tests/decode_dump \
  --work-dir artifacts/property
```

The same test is registered with CTest:

```sh
ctest --test-dir build-asan -R faad2_property_audio --output-on-failure
```

`tests/run_property_tests.py` creates its seed with the system `ffmpeg` unless
`--seed FILE.aac` is supplied.

## Malformed corpus

The runner generates 200 ADTS frames by cycling through four categories, so
each category has exactly 50 cases:

| Category | Mutation |
| --- | --- |
| `truncated` | Cut at a chosen byte offset, including 1 byte, a short header, a bare header, and random payload cuts. |
| `bad_header` | Corrupt sync, layer, sample-rate index, or profile fields. |
| `duplicate_config` | Insert a second ADTS configuration header before the payload, with and without rewriting `aac_frame_length`. |
| `oversized` | Declare 4095/8191 bytes, pad to 8191 bytes, or append up to 64 KiB while keeping a small declared length. |

Every case is stored in `artifacts/property/cases/`. The manifest records the
source frame, mutation variant, declared size, actual size, and
`mutation_offset` (the byte offset of the corruption).

Each case runs in its own process with:

```text
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
```

The 200 cases are also replayed in one process in alternating order, with a
fresh decoder handle per case, so LeakSanitizer also checks the complete
alloc/free lifecycle.

The probe verifies that every `bytesconsumed` value is within the supplied
buffer. If a violation is detected it prints:

```text
STATUS=fail case=... reason=... offset=<decode offset> consumed=... remaining=...
```

Sanitizer failures, probe failures, and the matching case file are copied to
`artifacts/property/counterexamples/`; full output is written to
`artifacts/property/logs/`.

## Reference comparison

The runner extracts 50 valid AAC-LC, 48 kHz, stereo ADTS frames and decodes
them with two independent decoders:

- FAAD2 through `decode_dump`, output as interleaved float32.
- FFmpeg through `pcm_f32le`.

All 102400 scalar samples (50 frames x 1024 samples x 2 channels) are compared
sample by sample. The default absolute tolerance is `1e-3`; this is required
because FAAD2 and FFmpeg use different transform/filter rounding. A mismatch
reports the scalar sample index, byte offset, frame index, channel, expected
value, and actual value.

## Current result

With seed SHA-256
`dbf927bc2aecd5f360d8145ccab149c7d807ef2bc2af953f8cd12e5f5551c5d6`:

- 200/200 malformed cases completed; 15 bad-header cases were rejected during
  initialization, the rest exercised decode.
- 0 crashes, 0 sanitizer reports, 0 leaks.
- 200/200 cases completed in the single-process alternating replay.
- 102400/102400 scalar samples compared; 0 mismatches.
- Maximum absolute FFmpeg-vs-FAAD2 sample error: `8.28504562e-4`.
- No failing counterexamples. The 200 non-failing corpus files remain under
  `artifacts/property/cases/` for regression use.

Generated reports:

- `artifacts/property/summary.md`
- `artifacts/property/summary.json`
- `artifacts/property/manifest.json`
