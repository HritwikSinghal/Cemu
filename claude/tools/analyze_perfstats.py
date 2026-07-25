#!/usr/bin/env python3
"""Analyze CEMU_PERFSTATS_CSV captures from the fork's bottleneck instrumentation.

Usage:
    python3 claude/tools/analyze_perfstats.py capture.csv [other.csv ...]

One file  -> steady-state summary: frametime distribution, GPU busy vs frametime,
             per-stage CPU timers, per-frame counters, and a slowest-5%-vs-fastest-50%
             delta table that shows what distinguishes a bad frame.
Two+ files -> additionally a like-for-like comparison bucketed by draws/frame, which is
             how the 4K-vs-1440p pixel/fixed cost split was derived (see claude/profiling.md).

Why bucket by draw count: separate captures are separate play sessions, so the scene
content differs frame to frame. Draws/frame is the available proxy for "same scene
complexity". Do NOT compare raw per-capture means across resolutions -- the light/heavy
scene mix differs and swamps the resolution effect.

Column names are read from the CSV header, so this keeps working when the
instrumentation grows new columns (see LattePerformanceMonitor.cpp writer).
"""
from __future__ import annotations

import csv
import statistics as st
import sys
from typing import Sequence

# Frames to drop from the start of every capture: shader/pipeline warm-up and
# first-touch texture uploads make them unrepresentative.
WARMUP_FRAMES = 120

# Stage timers, in the order they appear in the frame. Kept explicit rather than
# derived from the header so the summary reads in pipeline order.
STAGE_TIMERS = [
    "shaderUpdateUs", "fboUpdateUs", "textureUpdateUs", "textureHashUs",
    "textureUploadUs", "uniformUs", "indexUs", "bufferSyncUs", "pipelineUs",
    "descriptorSetsUs", "renderpassUs", "submitUs", "gpuWaitUs", "idleSpinUs",
    "guestFenceUs",
]

COUNTERS = [
    "drawsFirst", "drawsFast", "seqEndTexture", "seqEndContextReg",
    "indexCacheHit", "indexCacheMiss", "pipelineMiss", "descSetMiss",
    "asyncSkippedDraws", "submits", "submitsForced", "occlusionQueries",
    "textureReloads", "textureReloadSlices", "vsyncLateUs",
]

BYTE_COUNTERS = ["bytesUniform", "bytesTexture", "bytesIndex"]

Rows = list[dict[str, int]]


def load(path: str) -> Rows:
    with open(path, newline="") as fh:
        rows = [{k: int(v) for k, v in r.items() if v != ""} for r in csv.DictReader(fh)]
    if len(rows) <= WARMUP_FRAMES:
        raise SystemExit(f"{path}: only {len(rows)} rows, need more than {WARMUP_FRAMES} warm-up frames")
    return rows[WARMUP_FRAMES:]


def pct(vals: Sequence[int], p: float) -> int:
    ordered = sorted(vals)
    return ordered[min(int(len(ordered) * p / 100), len(ordered) - 1)]


def mean_of(rows: Rows, col: str) -> float:
    return st.mean(r[col] for r in rows) if rows else 0.0


def draws(row: dict[str, int]) -> int:
    return row["drawsFirst"] + row["drawsFast"]


def summarize(path: str, rows: Rows) -> None:
    n = len(rows)
    ft = [r["frameUs"] for r in rows]
    mft = st.mean(ft)
    print(f"\n{'=' * 78}\n== {path}  ({n} steady-state frames, warm-up {WARMUP_FRAMES} dropped)\n{'=' * 78}")

    print("-- frametime --")
    print(f"mean {mft:.0f}us ({1e6 / mft:.1f} fps)   median {st.median(ft):.0f}us ({1e6 / st.median(ft):.1f} fps)")
    print(f"p5 {pct(ft, 5)}  p95 {pct(ft, 95)}  p99 {pct(ft, 99)}  max {max(ft)}")
    for limit, label in [(17500, "misses 60fps"), (25000, ">25ms"), (33333, "misses 30fps")]:
        c = sum(1 for v in ft if v > limit)
        print(f"  frames {label:14s}: {c:5d} ({100 * c / n:5.1f}%)")

    gpu = [r["gpuBusyUs"] for r in rows]
    print("\n-- GPU busy (retired-cmdbuffer attribution; see caveats in claude/profiling.md) --")
    print(f"mean {st.mean(gpu):.0f}us  median {st.median(gpu):.0f}us  p95 {pct(gpu, 95)}us"
          f"   busy/frametime {100 * st.mean(gpu) / mft:.1f}%")

    print("\n-- per-stage CPU time (us/frame) --")
    print(f"{'stage':>18s} {'mean':>8s} {'median':>8s} {'p95':>8s} {'%frame':>7s}")
    for t in STAGE_TIMERS:
        if t not in rows[0]:
            continue
        v = [r[t] for r in rows]
        print(f"{t[:-2]:>18s} {st.mean(v):8.0f} {st.median(v):8.0f} {pct(v, 95):8d} {100 * st.mean(v) / mft:6.1f}%")
    tracked = sum(mean_of(rows, t) for t in STAGE_TIMERS if t in rows[0])
    print(f"{'SUM tracked':>18s} {tracked:8.0f} {'':17s} {100 * tracked / mft:6.1f}%")

    print("\n-- per-frame counters (mean / median / p95) --")
    for c in COUNTERS:
        if c not in rows[0]:
            continue
        v = [r[c] for r in rows]
        print(f"{c:>20s} {st.mean(v):10.1f} {st.median(v):8.0f} {pct(v, 95):8d}")

    print("\n-- upload volume (KB/frame, mean) --")
    for c in BYTE_COUNTERS:
        if c in rows[0]:
            print(f"{c:>20s} {mean_of(rows, c) / 1024:10.1f}")

    ordered = sorted(rows, key=lambda r: r["frameUs"])
    fast, slow = ordered[: n // 2], ordered[-max(1, n // 20):]
    print(f"\n-- slowest 5% (n={len(slow)}, mean {mean_of(slow, 'frameUs') / 1000:.1f}ms)"
          f" vs fastest 50% (mean {mean_of(fast, 'frameUs') / 1000:.1f}ms) --")
    print(f"{'metric':>20s} {'fast50':>12s} {'slow5':>12s} {'delta':>10s}")
    cols = [c for c in ["gpuBusyUs"] + STAGE_TIMERS + COUNTERS + BYTE_COUNTERS if c in rows[0]]
    deltas = [(mean_of(slow, c) - mean_of(fast, c), c, mean_of(fast, c), mean_of(slow, c)) for c in cols]
    for d, c, fm, sm in sorted(deltas, reverse=True):
        print(f"{c:>20s} {fm:12.0f} {sm:12.0f} {d:+10.0f}")


def compare(captures: list[tuple[str, Rows]], bucket: int = 500, min_frames: int = 30) -> None:
    """Like-for-like GPU cost comparison, bucketed by draws/frame."""
    print(f"\n{'=' * 78}\n== like-for-like comparison, bucketed by draws/frame\n{'=' * 78}")
    names = [f"{n.split('/')[-1]}" for n, _ in captures]
    header = f"{'draws/frame':>14s}"
    for nm in names:
        header += f" | {nm[:18]:>18s}"
    print(header)
    print(f"{'':>14s}" + " | ".join(f"{'n   busy   ft':>18s}" for _ in names))

    hi_draws = max(draws(r) for _, rows in captures for r in rows)
    for lo in range(0, hi_draws + bucket, bucket):
        cells, keep = [], True
        for _, rows in captures:
            sel = [r for r in rows if lo <= draws(r) < lo + bucket]
            if len(sel) < min_frames:
                keep = False
            cells.append((len(sel), mean_of(sel, "gpuBusyUs") / 1000, mean_of(sel, "frameUs") / 1000))
        if not keep:
            continue
        line = f"{lo:6d}-{lo + bucket:<7d}"
        for cnt, busy, ft in cells:
            line += f" | {cnt:5d} {busy:5.1f}ms {ft:5.1f}ms"
        print(line)

    print("\nRead this table, not the per-capture means: it holds scene complexity roughly")
    print("constant. To split pixel-scaled vs resolution-independent GPU cost, take one")
    print("well-populated bucket present in both captures and solve:")
    print("    busy_hi = P + F ;  busy_lo = P / pixel_ratio + F")
    print("A naive linear fit of busy vs draws is NOT reliable here (it produced a negative")
    print("slope for the 1440p capture) -- draw count tracks content, not cost.")


def main(argv: list[str]) -> int:
    if len(argv) < 2:
        print(__doc__)
        return 1
    captures = [(p, load(p)) for p in argv[1:]]
    for path, rows in captures:
        summarize(path, rows)
    if len(captures) > 1:
        compare(captures)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
