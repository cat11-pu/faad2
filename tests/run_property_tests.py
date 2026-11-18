#!/usr/bin/env python3
"""Malformed-frame property test and reference-decoder comparison for FAAD2."""

import argparse
import array
import hashlib
import json
import math
import os
import platform
import random
import shutil
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path


KINDS = ("truncated", "bad_header", "duplicate_config", "oversized")
SEED_FILTER = (
    "aevalsrc="
    "0.20*sin(2*PI*440*t)+0.10*sin(2*PI*997*t)+0.05*sin(2*PI*3000*t)"
    "|"
    "0.18*sin(2*PI*523.25*t)+0.08*sin(2*PI*1318.5*t)"
    ":s=48000:d=2"
)


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def run(cmd, *, env=None, timeout=120):
    return subprocess.run(
        [str(part) for part in cmd],
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=timeout,
    )


def parse_adts(blob):
    frames = []
    pos = 0
    while pos < len(blob):
        if pos + 7 > len(blob):
            raise ValueError(f"truncated ADTS header at byte {pos}")
        if blob[pos] != 0xFF or (blob[pos + 1] & 0xF6) != 0xF0:
            raise ValueError(f"bad ADTS sync at byte {pos}")
        header_len = 7 if (blob[pos + 1] & 1) else 9
        frame_len = ((blob[pos + 3] & 0x03) << 11) | (blob[pos + 4] << 3)
        frame_len |= blob[pos + 5] >> 5
        if frame_len < header_len:
            raise ValueError(f"invalid ADTS frame length {frame_len} at byte {pos}")
        end = pos + frame_len
        if end > len(blob):
            raise ValueError(f"ADTS frame at byte {pos} exceeds input")
        frames.append(blob[pos:end])
        pos = end
    return frames


def adts_header_len(frame):
    return 7 if (frame[1] & 1) else 9


def adts_frame_length(frame):
    return (
        ((frame[3] & 0x03) << 11)
        | (frame[4] << 3)
        | (frame[5] >> 5)
    )


def set_adts_frame_length(frame, length):
    if not 0 <= length <= 0x1FFF:
        raise ValueError(f"ADTS frame length out of range: {length}")
    frame[3] = (frame[3] & 0xFC) | ((length >> 11) & 0x03)
    frame[4] = (length >> 3) & 0xFF
    frame[5] = (frame[5] & 0x1F) | ((length & 0x07) << 5)


def generate_seed(ffmpeg, work_dir):
    source = work_dir / "seed_source.aac"
    cmd = [
        ffmpeg,
        "-hide_banner",
        "-loglevel",
        "error",
        "-y",
        "-f",
        "lavfi",
        "-i",
        SEED_FILTER,
        "-c:a",
        "aac",
        "-profile:a",
        "aac_low",
        "-b:a",
        "128k",
        "-f",
        "adts",
        source,
    ]
    proc = run(cmd)
    if proc.returncode != 0:
        raise RuntimeError(
            "ffmpeg seed generation failed:\n"
            + proc.stderr.decode("utf-8", "replace")
        )
    return source.read_bytes()


def prepare_valid_seed(blob, valid_count):
    frames = parse_adts(blob)
    if len(frames) < valid_count:
        raise ValueError(
            f"need {valid_count} valid ADTS frames, encoder produced {len(frames)}"
        )
    frames = frames[:valid_count]

    profile = frames[0][2] >> 6
    sf_index = (frames[0][2] >> 2) & 0x0F
    channel_config = ((frames[0][2] & 1) << 2) | (frames[0][3] >> 6)
    if (profile, sf_index, channel_config) != (1, 3, 2):
        raise ValueError(
            "expected AAC-LC, 48 kHz, stereo; got "
            f"profile={profile}, sf_index={sf_index}, channels={channel_config}"
        )
    return frames


def make_cases(seed_frames, rng):
    cases = []
    for case_id in range(len(seed_frames) * len(KINDS)):
        kind = KINDS[case_id % len(KINDS)]
        group = case_id // len(KINDS)
        source_index = group % len(seed_frames)
        source = seed_frames[source_index]
        size = len(source)
        header_len = adts_header_len(source)
        mutation_offset = 0
        variant = group % 5
        declared_size = size

        if kind == "truncated":
            if group == 0:
                cut = 1
            elif group == 1:
                cut = header_len - 1
            elif group == 2:
                cut = header_len
            else:
                cut = 1 + rng.randrange(size - 1)
            data = source[:cut]
            mutation_offset = cut
            declared_size = size

        elif kind == "bad_header":
            data = bytearray(source)
            if variant == 0:
                data[0] = 0x00
                mutation_offset = 0
            elif variant == 1:
                data[1] = (data[1] & 0xF8) | 0x06
                mutation_offset = 1
            elif variant == 2:
                data[2] = (data[2] & 0xC3) | (15 << 2)
                mutation_offset = 2
            elif variant == 3:
                data[2] = (data[2] & 0x3F) | (3 << 6)
                mutation_offset = 2
            else:
                data[2] &= 0xFE
                data[3] &= 0x3F
                mutation_offset = 2
            declared_size = adts_frame_length(data)

        elif kind == "duplicate_config":
            header = source[:header_len]
            duplicated = header + header + source[header_len:]
            data = bytearray(duplicated)
            mutation_offset = header_len
            if variant & 1:
                set_adts_frame_length(data, len(data))
            declared_size = adts_frame_length(data)

        else:
            if variant == 0:
                data = bytearray(source)
                set_adts_frame_length(data, 0x1FFF)
                declared_size = 0x1FFF
            elif variant == 1:
                data = bytearray(0x1FFF)
                for i in range(len(data)):
                    data[i] = (case_id * 37 + i * 11) & 0xFF
                data[:size] = source
                set_adts_frame_length(data, 0x1FFF)
                declared_size = 0x1FFF
            elif variant == 2:
                keep = min(size, 64)
                data = bytearray(source[:keep])
                set_adts_frame_length(data, 0x0FFF)
                declared_size = 0x0FFF
            elif variant == 3:
                data = bytearray(source)
                data.extend(((case_id * 29 + i * 7) & 0xFF) for i in range(65536))
                set_adts_frame_length(data, 0x1FFF)
                declared_size = 0x1FFF
            else:
                data = bytearray(source)
                set_adts_frame_length(data, 0x1FFF)
                data.extend(b"\xFF" * 768)
                declared_size = 0x1FFF
            mutation_offset = 3

        cases.append(
            {
                "case": case_id,
                "kind": kind,
                "variant": variant,
                "source_frame": source_index,
                "source_size": size,
                "size": len(data),
                "declared_size": declared_size,
                "mutation_offset": mutation_offset,
                "data": bytes(data),
            }
        )
    return cases


def classify_failure(returncode, stderr):
    text = stderr.decode("utf-8", "replace")
    if "LeakSanitizer" in text:
        return "leak"
    if "AddressSanitizer" in text:
        return "out_of_bounds"
    if "runtime error:" in text:
        return "undefined_behavior"
    if returncode < 0:
        return "crash"
    return "probe_failure"


def sanitizer_env():
    env = os.environ.copy()
    env["ASAN_OPTIONS"] = (
        "detect_leaks=1:halt_on_error=1:abort_on_error=0:"
        "allocator_may_return_null=1"
    )
    env["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
    env["LSAN_OPTIONS"] = "exitcode=23"
    return env


def parse_status_line(line):
    fields = {"status": line.split()[0].split("=", 1)[1]}
    for part in line.split():
        if "=" in part:
            key, value = part.split("=", 1)
            if key != "STATUS":
                fields[key] = value
    return fields


def parse_probe_line(stdout):
    for line in stdout.decode("utf-8", "replace").splitlines():
        if line.startswith("STATUS="):
            return parse_status_line(line)
    return {}


def run_malformed(cases, probe, work_dir):
    case_dir = work_dir / "cases"
    log_dir = work_dir / "logs"
    counter_dir = work_dir / "counterexamples"
    case_dir.mkdir(parents=True, exist_ok=True)
    log_dir.mkdir(parents=True, exist_ok=True)
    counter_dir.mkdir(parents=True, exist_ok=True)

    env = sanitizer_env()

    results = []
    failures = []
    for case in cases:
        path = case_dir / f"case_{case['case']:03d}_{case['kind']}.aac"
        path.write_bytes(case["data"])

        started = time.monotonic()
        proc = run(
            [probe, path, f"{case['case']:03d}"],
            env=env,
            timeout=30,
        )
        elapsed_ms = (time.monotonic() - started) * 1000.0
        status = parse_probe_line(proc.stdout)

        record = {
            "case": case["case"],
            "kind": case["kind"],
            "variant": case["variant"],
            "source_frame": case["source_frame"],
            "size": case["size"],
            "declared_size": case["declared_size"],
            "mutation_offset": case["mutation_offset"],
            "path": str(path.relative_to(work_dir)),
            "returncode": proc.returncode,
            "status": status.get("status", "missing"),
            "elapsed_ms": round(elapsed_ms, 3),
        }

        if proc.returncode != 0:
            category = classify_failure(proc.returncode, proc.stderr)
            log_path = log_dir / f"case_{case['case']:03d}.log"
            log_path.write_bytes(proc.stdout + b"\n--- stderr ---\n" + proc.stderr)
            counter_path = (
                counter_dir
                / f"case_{case['case']:03d}_{case['kind']}_off{case['mutation_offset']}.aac"
            )
            shutil.copyfile(path, counter_path)
            record.update(
                {
                    "failure": category,
                    "failure_offset": case["mutation_offset"],
                    "log": str(log_path.relative_to(work_dir)),
                    "counterexample": str(counter_path.relative_to(work_dir)),
                    "stderr_first_line": proc.stderr.decode(
                        "utf-8", "replace"
                    ).splitlines()[:1],
                }
            )
            failures.append(record)
        elif status.get("status") == "fail":
            record.update(
                {
                    "failure": status.get("reason", "probe_failure"),
                    "failure_offset": int(status.get("offset", "0")),
                    "forced_consumed": int(status.get("consumed", "0")),
                    "remaining": int(status.get("remaining", "0")),
                }
            )
            counter_path = (
                counter_dir
                / f"case_{case['case']:03d}_{case['kind']}_off{status.get('offset', '0')}.aac"
            )
            shutil.copyfile(path, counter_path)
            record["counterexample"] = str(counter_path.relative_to(work_dir))
            failures.append(record)
        elif status.get("status") == "ok":
            record.update(
                {
                    "decoded": int(status.get("decoded", "0")),
                    "calls": int(status.get("calls", "0")),
                    "errors": int(status.get("errors", "0")),
                }
            )
        results.append(record)

    return results, failures


def run_batch_replay(results, probe, work_dir):
    """Replay all cases in one process, with a fresh decoder per case."""
    paths = [work_dir / item["path"] for item in results]
    started = time.monotonic()
    proc = run([probe, "--batch", *paths], env=sanitizer_env(), timeout=120)
    elapsed_ms = (time.monotonic() - started) * 1000.0
    stdout = proc.stdout.decode("utf-8", "replace")
    lines = [line for line in stdout.splitlines() if line.startswith("STATUS=")]
    ok_lines = [line for line in lines if line.startswith("STATUS=ok ")]
    rejected_lines = [
        line for line in lines if line.startswith("STATUS=init_rejected ")
    ]
    fail_lines = [line for line in lines if line.startswith("STATUS=fail ")]
    record = {
        "cases": len(results),
        "completed": len(ok_lines) + len(rejected_lines),
        "decoded": len(ok_lines),
        "init_rejected": len(rejected_lines),
        "returncode": proc.returncode,
        "status": "ok" if proc.returncode == 0 else "fail",
        "elapsed_ms": round(elapsed_ms, 3),
    }
    if proc.returncode != 0:
        category = classify_failure(proc.returncode, proc.stderr)
        log_dir = work_dir / "logs"
        log_dir.mkdir(parents=True, exist_ok=True)
        log_path = log_dir / "batch_replay.log"
        log_path.write_bytes(proc.stdout + b"\n--- stderr ---\n" + proc.stderr)
        fields = parse_status_line(fail_lines[-1]) if fail_lines else {}
        failure_path = fields.get("case")
        counterexample = None
        if failure_path:
            source = Path(failure_path)
            if not source.exists():
                source = work_dir / "cases" / failure_path
            if source.exists():
                counter_dir = work_dir / "counterexamples"
                counter_dir.mkdir(parents=True, exist_ok=True)
                counter_path = counter_dir / source.name
                shutil.copyfile(source, counter_path)
                counterexample = str(counter_path.relative_to(work_dir))
        record.update(
            {
                "failure": category,
                "failure_offset": int(fields.get("offset", "-1")),
                "log": str(log_path.relative_to(work_dir)),
                "counterexample": counterexample,
            }
        )
    elif fail_lines:
        fields = parse_probe_line(proc.stdout)
        record.update(
            {
                "failure": fields.get("reason", "probe_failure"),
                "failure_offset": int(fields.get("offset", "0")),
                "forced_consumed": int(fields.get("consumed", "0")),
                "remaining": int(fields.get("remaining", "0")),
            }
        )
    return record


def compare_reference(faad_path, ref_path, tolerance, valid_frames, work_dir):
    faad_bytes = faad_path.read_bytes()
    ref_bytes = ref_path.read_bytes()
    if len(faad_bytes) % 4 or len(ref_bytes) % 4:
        raise ValueError("reference dump is not float32 aligned")

    faad = array.array("f")
    faad.frombytes(faad_bytes)
    ref = array.array("f")
    ref.frombytes(ref_bytes)
    if sys.byteorder != "little":
        faad.byteswap()
        ref.byteswap()

    channels = 2
    samples_per_channel = 1024
    scalars_per_frame = samples_per_channel * channels
    expected_scalars = len(valid_frames) * scalars_per_frame
    count_ok = len(faad) == len(ref) == expected_scalars

    max_abs = 0.0
    max_index = -1
    sum_sq = 0.0
    mismatches = 0
    first_bad = None
    compared = min(len(faad), len(ref))
    per_frame_max = [0.0] * len(valid_frames)

    for index in range(compared):
        actual = faad[index]
        expected = ref[index]
        finite = math.isfinite(actual) and math.isfinite(expected)
        diff = abs(actual - expected) if finite else float("inf")
        if diff > max_abs:
            max_abs = diff
            max_index = index
        if finite:
            sum_sq += (actual - expected) * (actual - expected)
        frame_index = index // scalars_per_frame
        if frame_index < len(per_frame_max) and diff > per_frame_max[frame_index]:
            per_frame_max[frame_index] = diff
        if diff > tolerance:
            mismatches += 1
            if first_bad is None:
                first_bad = {
                    "sample_index": index,
                    "byte_offset": index * 4,
                    "frame_index": frame_index,
                    "sample_in_frame": (index // channels) % samples_per_channel,
                    "channel": index % channels,
                    "expected": expected,
                    "actual": actual,
                    "abs_error": diff,
                }

    counterexample = None
    if mismatches or not count_ok:
        counter_dir = work_dir / "counterexamples"
        counter_dir.mkdir(parents=True, exist_ok=True)
        frame_index = 0
        if max_index >= 0:
            frame_index = min(max_index // scalars_per_frame, len(valid_frames) - 1)
        path = counter_dir / f"valid_frame_{frame_index:02d}.aac"
        path.write_bytes(valid_frames[frame_index])
        counterexample = {
            "path": str(path.relative_to(work_dir)),
            "frame_index": frame_index,
            "first_bad_sample": first_bad,
            "max_abs_error": max_abs,
            "max_error_sample_index": max_index,
        }

    return {
        "valid_frames": len(valid_frames),
        "channels": channels,
        "samples_per_channel_per_frame": samples_per_channel,
        "scalars_expected": expected_scalars,
        "scalars_faad": len(faad),
        "scalars_reference": len(ref),
        "count_ok": count_ok,
        "tolerance_abs": tolerance,
        "mismatches": mismatches,
        "max_abs_error": max_abs,
        "max_error_sample_index": max_index,
        "max_error_frame_index": (
            max_index // scalars_per_frame if max_index >= 0 else -1
        ),
        "rms_error": math.sqrt(sum_sq / compared) if compared else 0.0,
        "per_frame_max_abs_error": per_frame_max,
        "first_bad_sample": first_bad,
        "counterexample": counterexample,
    }


def summarize_by_kind(results):
    summary = {}
    for kind in KINDS:
        selected = [item for item in results if item["kind"] == kind]
        summary[kind] = {
            "generated": len(selected),
            "init_rejected": sum(
                1 for item in selected if item["status"] == "init_rejected"
            ),
            "decoded": sum(1 for item in selected if item["status"] == "ok"),
            "requested_failures": sum(
                1 for item in selected if item["status"] == "fail"
            ),
            "sanitizer_failures": sum(
                1 for item in selected if "failure" in item
            ),
            "mutation_offset_min": min(
                (item["mutation_offset"] for item in selected), default=None
            ),
            "mutation_offset_max": max(
                (item["mutation_offset"] for item in selected), default=None
            ),
        }
    return summary


def write_reports(
    work_dir, malformed, batch_replay, valid_ref, cases, seed_sha, ffmpeg, args
):
    summary = {
        "generated_at_utc": datetime.now(timezone.utc).isoformat(),
        "platform": platform.platform(),
        "seed_sha256": seed_sha,
        "seed_frames": len(cases) // len(KINDS),
        "malformed": {
            "planned": len(cases),
            "completed": len(malformed),
            "failures": sum(1 for item in malformed if "failure" in item),
            "by_kind": summarize_by_kind(malformed),
        },
        "batch_replay": batch_replay,
        "valid_reference": valid_ref,
        "counterexamples": [
            item for item in malformed if "counterexample" in item
        ],
        "tools": {
            "ffmpeg": ffmpeg,
            "probe": args.probe,
            "dump": args.dump,
        },
    }
    if valid_ref.get("counterexample"):
        summary["counterexamples"].append(valid_ref["counterexample"])
    if "failure" in batch_replay:
        summary["counterexamples"].append(batch_replay)

    (work_dir / "summary.json").write_text(
        json.dumps(summary, indent=2, ensure_ascii=False) + "\n"
    )
    (work_dir / "manifest.json").write_text(
        json.dumps(
            {
                "seed_sha256": seed_sha,
                "cases": [{k: v for k, v in case.items() if k != "data"} for case in cases],
            },
            indent=2,
            ensure_ascii=False,
        )
        + "\n"
    )

    failures = summary["malformed"]["failures"]
    lines = [
        "# FAAD2 property test summary",
        "",
        f"- Generated: `{summary['generated_at_utc']}`",
        f"- Seed SHA-256: `{seed_sha}`",
        f"- Malformed frames: `{len(malformed)}` "
        f"({', '.join(f'{k}={v}' for k, v in summary['malformed']['by_kind'].items())})",
        f"- Malformed failures: `{failures}`",
        f"- Alternating batch replay: `{batch_replay['completed']}/"
        f"{batch_replay['cases']}` cases completed in one process, "
        f"`{batch_replay['init_rejected']}` init-rejected, "
        f"status `{batch_replay['status']}`",
        f"- Valid frames compared: `{valid_ref['valid_frames']}`",
        f"- Reference scalars: `{valid_ref['scalars_expected']}`",
        f"- Max absolute sample error: `{valid_ref['max_abs_error']:.9g}` "
        f"(tolerance `{valid_ref['tolerance_abs']:.9g}`)",
        f"- Sample mismatches: `{valid_ref['mismatches']}`",
        "",
        "## Malformed frame results",
        "",
        "| kind | generated | init rejected | decoded | failures |",
        "| --- | ---: | ---: | ---: | ---: |",
    ]
    for kind, row in summary["malformed"]["by_kind"].items():
        lines.append(
            f"| {kind} | {row['generated']} | {row['init_rejected']} | "
            f"{row['decoded']} | {row['sanitizer_failures']} |"
        )

    lines.extend(
        [
            "",
            "Each case was run in its own process with "
            "`ASAN_OPTIONS=detect_leaks=1` and `UBSAN_OPTIONS=halt_on_error=1`.",
            "`mutation_offset` is the byte offset of the corruption in the "
            "case file; probe failures also report the decode `offset` where "
            "`bytesconsumed` exceeded the remaining input.",
            "",
            "## Reference comparison",
            "",
            f"- FAAD2 scalars: `{valid_ref['scalars_faad']}`",
            f"- FFmpeg scalars: `{valid_ref['scalars_reference']}`",
            f"- Count match: `{valid_ref['count_ok']}`",
            f"- RMS error: `{valid_ref['rms_error']:.9g}`",
            f"- First mismatch: `{valid_ref['first_bad_sample']}`",
            "",
            "## Counterexamples",
            "",
        ]
    )
    if summary["counterexamples"]:
        for item in summary["counterexamples"]:
            path = item.get("path") or item.get("counterexample")
            if path:
                lines.append(f"- `{path}`")
    else:
        lines.append("None. No crash, out-of-bounds access, leak, or "
                     "over-tolerance sample mismatch was observed.")
    lines.append("")
    (work_dir / "summary.md").write_text("\n".join(lines))
    return summary


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--probe", required=True)
    parser.add_argument("--dump", required=True)
    parser.add_argument("--work-dir", required=True)
    parser.add_argument("--seed", help="existing ADTS seed file")
    parser.add_argument("--ffmpeg", default=shutil.which("ffmpeg") or "ffmpeg")
    parser.add_argument("--valid-frames", type=int, default=50)
    parser.add_argument("--tolerance", type=float, default=1e-3)
    args = parser.parse_args()

    work_dir = Path(args.work_dir).resolve()
    work_dir.mkdir(parents=True, exist_ok=True)
    for name in ("cases", "logs", "counterexamples"):
        stale = work_dir / name
        if stale.exists():
            shutil.rmtree(stale)

    if args.seed:
        seed_blob = Path(args.seed).read_bytes()
    else:
        seed_blob = generate_seed(args.ffmpeg, work_dir)
    valid_frames = prepare_valid_seed(seed_blob, args.valid_frames)
    valid_blob = b"".join(valid_frames)
    (work_dir / "valid50.aac").write_bytes(valid_blob)

    rng = random.Random(0xFAAD2)
    cases = make_cases(valid_frames, rng)
    print(
        f"generated {len(cases)} malformed cases "
        f"({', '.join(f'{kind}={len(valid_frames)}' for kind in KINDS)})",
        flush=True,
    )

    malformed, failures = run_malformed(cases, args.probe, work_dir)
    print(
        f"malformed: {len(malformed)} run, {len(failures)} failure(s)",
        flush=True,
    )
    batch_replay = run_batch_replay(malformed, args.probe, work_dir)
    print(
        f"batch replay: {batch_replay['completed']}/{batch_replay['cases']} cases, "
        f"status={batch_replay['status']}",
        flush=True,
    )

    valid_ref = {}
    ref_path = work_dir / "ffmpeg_ref.f32"
    faad_path = work_dir / "faad.f32"
    cmd = [
        args.ffmpeg,
        "-hide_banner",
        "-loglevel",
        "error",
        "-y",
        "-i",
        work_dir / "valid50.aac",
        "-f",
        "f32le",
        "-acodec",
        "pcm_f32le",
        ref_path,
    ]
    proc = run(cmd)
    if proc.returncode != 0:
        raise RuntimeError(
            "ffmpeg reference decode failed:\n"
            + proc.stderr.decode("utf-8", "replace")
        )
    proc = run([args.dump, work_dir / "valid50.aac", faad_path])
    if proc.returncode != 0:
        raise RuntimeError(
            "FAAD2 reference dump failed:\n"
            + proc.stderr.decode("utf-8", "replace")
        )
    valid_ref = compare_reference(
        faad_path, ref_path, args.tolerance, valid_frames, work_dir
    )
    print(
        "reference: "
        f"scalars={valid_ref['scalars_faad']}, "
        f"max_abs={valid_ref['max_abs_error']:.9g}, "
        f"mismatches={valid_ref['mismatches']}",
        flush=True,
    )

    summary = write_reports(
        work_dir,
        malformed,
        batch_replay,
        valid_ref,
        cases,
        sha256(seed_blob),
        args.ffmpeg,
        args,
    )
    print(f"summary: {work_dir / 'summary.md'}", flush=True)

    bad = bool(failures) or "failure" in batch_replay or bool(
        valid_ref
        and (not valid_ref["count_ok"] or valid_ref["mismatches"] > 0)
    )
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
