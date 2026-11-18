#!/usr/bin/env bash
# Reproducible SBR / coupling benchmark for the faad2 floating-point build.
#
# Builds two libraries from the current working tree:
#   * baseline  : original code compiled with the legacy -ffloat-store
#   * optimized : current code compiled with the production flags (no float-store)
# Then decodes each HE-AAC stream N times (best-of-N) and compares PCM output
# byte-for-byte (32-bit float, i.e. true sample-for-sample equivalence).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ITERS="${ITERS:-50}"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

DEFS="-DAPPLY_DRC -DHAVE_INTTYPES_H=1 -DHAVE_LRINTF=1 -DHAVE_MEMCPY=1 \
 -DHAVE_STRINGS_H=1 -DHAVE_STRING_H=1 -DHAVE_SYS_STAT_H=1 -DHAVE_SYS_TYPES_H=1 \
 -DPACKAGE_VERSION=\"2.11.4\""

build_lib() {
    local dir="$1"; shift
    mkdir -p "$dir"
    cp -r "$ROOT/libfaad/." "$dir/"
    (cd "$dir" && gcc -O3 -DNDEBUG "$@" $DEFS \
        -I"$ROOT/build/include" -I. $(ls *.c | grep -v drm_dec) -c)
    (cd "$dir" && ar rcs libbench.a *.o)
}

# Baseline = pristine HEAD sources + the historical flag.
mkdir -p "$WORK/base_src"
git -C "$ROOT" archive HEAD libfaad | tar -x -C "$WORK/base_src" --strip-components=1
build_lib "$WORK/base" -ffloat-store

# Optimized = current working-tree sources + current production flags.
build_lib "$WORK/opt"

gcc -O2 -I"$ROOT/include" "$ROOT/bench/bench_dec.c" "$WORK/base/libbench.a" -lm -o "$WORK/bench_base"
gcc -O2 -I"$ROOT/include" "$ROOT/bench/bench_dec.c" "$WORK/opt/libbench.a"  -lm -o "$WORK/bench_opt"

printf "%-8s | %-10s | %-10s | %-9s | %-s\n" rate base_ms opt_ms reduction sample
printf '%s\n' "---------+------------+------------+-----------+------------"
for sr in 22050 32000 44100; do
    stream="$ROOT/bench/he_aac_${sr}.aac"
    "$WORK/bench_base" "$stream" "$ITERS" "$WORK/base.pcm"   >/dev/null
    "$WORK/bench_opt"  "$stream" "$ITERS" "$WORK/opt.pcm"    >/dev/null
    b=$("$WORK/bench_base" "$stream" "$ITERS" | tail -1 | awk '{print $6}' | cut -d= -f2)
    o=$("$WORK/bench_opt"  "$stream" "$ITERS" | tail -1 | awk '{print $6}' | cut -d= -f2)
    if cmp -s "$WORK/base.pcm" "$WORK/opt.pcm"; then exact=BIT_EXACT; else exact=DIFF; fi
    python3 -c "b=$b;o=$o;print(f'{\"$sr\":<8} | {b:10.3f} | {o:10.3f} | {(1-o/b)*100:7.1f}%  | $exact')"
done
