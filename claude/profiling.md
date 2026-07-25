# Cemu fork -- profiling reference

Durable how-to and measured-results reference for the Panther Lake perf campaign.
Live status and next actions stay in `claude/todo.md` / `claude/progress.md`; this file is
the stable "how do I measure, and what did we already measure" record.

Target hardware: Intel Panther Lake B390 iGPU (Xe3), Mesa ANV, Arch Linux, `xe` kernel driver.

---

## 1. Capturing with the fork's own instrumentation

The fork carries per-frame instrumentation (see root `CLAUDE.md` "Profiling instrumentation").
Two ways to turn it on; both set `g_lattePerfStatsEnabled`:

- **Overlay panel** (eyeball, no file): Options > General settings > Graphics > Overlay > Debug.
  Shows GPU busy, draw/sequence counters, per-stage CPU us, wait buckets.
- **CSV dump** (analysis): one row per frame, header included.

```bash
# NEVER capture to /tmp -- a reboot on 2026-07-26 wiped both baseline captures.
mkdir -p ~/Projects/Cemu-perf
CEMU_PERFSTATS_CSV=~/Projects/Cemu-perf/botw-$(date +%Y%m%d-%H%M)-4k.csv ./bin/Cemu_release
```

Capture hygiene, learned the hard way:

- Persistent path, resolution encoded in the filename.
- Play the **same route** in every capture of a comparison set -- cross-capture means are
  otherwise dominated by how much light-vs-heavy scenery you walked through.
- 2-3 minutes is plenty (the baselines were ~7000 and ~6700 frames).
- First ~120 frames are warm-up (shader compiles, first-touch texture uploads); the
  analysis script drops them.

Reading a capture:

```bash
python3 claude/tools/analyze_perfstats.py ~/Projects/Cemu-perf/botw-4k.csv
python3 claude/tools/analyze_perfstats.py .../botw-4k.csv .../botw-1440p.csv   # adds like-for-like table
```

Interpretation order (cheapest question first):

1. `frameUs` vs `gpuBusyUs` -- GPU-bound or CPU-bound?
2. If CPU-bound: per-stage timers and the wait buckets (`gpuWaitUs`, `idleSpinUs`).
3. `seqEndTexture` / `seqEndContextReg` -- how often draw sequences break (first-draw cost).
4. `indexCacheHit` / `indexCacheMiss`, `pipelineMiss`, `descSetMiss` -- cache health.

### Caveat on `gpuBusyUs` (important)

Timestamps bracket a **whole command buffer** (TOP/BOTTOM pair, read back at fence
retirement), so the number is "GPU wall time attributed to retired cmdbuffers", not pure
shader execution. Two consequences:

- GPU-side stalls inside a cmdbuffer -- notably occlusion queries using
  `VK_QUERY_RESULT_WAIT_BIT` -- count as "busy".
- Attribution smears across frame boundaries when a cmdbuffer spans them.

Weak evidence that query stalls are not dominant: heavy 4K frames with zero occlusion
queries showed 33.5ms busy at ~2989 draws vs 35.2ms at ~3927 draws for frames with
queries -- roughly draw-proportional. Confirm properly with `INTEL_MEASURE` (section 3).

---

## 2. Measured baseline results (captures taken 2026-07-25, analyzed 2026-07-26)

**The raw CSVs no longer exist** (they were written to `/tmp`; the 2026-07-26 reboot cleared
it). The aggregates below are the surviving record -- treat them as the baseline to beat, and
re-capture to a persistent path before any A/B claim.

Both captures: same BOTW play area, ~3 min, FPS++ enabled, VSync=2 (MAILBOX), AsyncCompile on.

### 2.1 Headline: heavy scenes are GPU-bound at both resolutions

| | 4K (7285 frames) | 1440p (6680 frames) |
|---|---|---|
| Mean frametime | 26.0ms (38.4 fps) | 19.8ms (50.5 fps) |
| Median frametime | 16.8ms (59.4 fps) | 20.3ms (49.4 fps) |
| Frames missing 60fps | 48.3% | 53.1% |
| Frames missing 30fps | 40.4% | 0.8% |
| Heavy-scene plateau | 37-39ms (~26 fps) | 20.6-24.7ms (~41-49 fps) |
| Heavy-scene GPU busy | 33-36ms | 17.8-22.8ms |
| Light-scene GPU busy | 6-9ms | 4.4-7ms |

Heavy-frame budget at 4K (n=2893 frames over 33.3ms): **38.5ms frametime = 35.1ms GPU busy
(91% utilization) + 3.4ms gap.** LatteThread `gpuWait` 25.6ms, `idleSpin` 0.8ms, and *all
fifteen* CPU stage timers together 6.9ms -- which is hidden under `gpuWait` anyway.

Consequence, and the reason the CPU-side plan was shelved: a perfect CPU-side fix recovers
at most the 3.4ms gap (~9%). Reaching 60fps needs GPU work to fall from ~35ms to <16.7ms.

### 2.2 Pixel-scaled vs resolution-independent GPU cost

Compared like-for-like via draw-count buckets (the per-capture means are not comparable):

| draws/frame | 4K GPU busy | 1440p GPU busy | ratio |
|---|---|---|---|
| 2500-3000 | 23.8ms | 11.6ms | 2.05 |
| 3000-3500 | 32.8ms | 18.3ms | 1.80 |
| 3500-4000 | 34.0ms | 19.3ms | 1.76 |
| 4000-4500 | 33.1ms | 18.9ms | 1.75 |

Heavy-scene aggregate: **33.4ms at 4K vs 18.9ms at 1440p = 1.77x for 2.25x fewer pixels.**
Solving `P + F = 33.4` and `P/2.25 + F = 18.9`:

- **P (pixel-scaled) ~= 26ms at 4K, ~11.6ms at 1440p**
- **F (resolution-independent: geometry, per-draw GPU overhead, query stalls) ~= 7.3ms**

Caveats: draw count is a cross-session content proxy; retired-attribution smearing applies;
and the 4K run may have sat at lower GPU clocks (power check still pending) which would
shift the split. Treat 26/7 as approximate.

Two conclusions that set campaign strategy:

- **Native 4K60 is out of reach.** Budget is ~15.5ms of GPU busy. Even zeroing F leaves 26ms
  of pixel work -- a ~3x pixel-cost cut. Not reachable without upscaling.
- **1440p60 needs only ~20-30%** (18.9-22.8ms down to ~15.5ms). This became the campaign
  target: render 1440p, output/upscale to the 4K panel.
- At 1440p, F is **39% of the GPU budget** (vs 22% at 4K). So resolution-independent GPU
  overhead -- occlusion-query renderpass splits, redundant state, per-draw cost -- is a
  legitimate target again at the new target resolution, where it was marginal at 4K.

### 2.3 Health of the landed fixes (both captures agree)

| metric | 4K | 1440p | note |
|---|---|---|---|
| Index cache hit rate | 75% (1533 hit / 521 miss) | 73% (2019 / 757) | was ~0% with the old 8-slot cache |
| `asyncSkippedDraws` | 0 | 0 | shaders warm, async-compile not a factor in these captures |
| `pipelineMiss` / `descSetMiss` per frame | 0.2 / 0.9 | 0.2 / 0.8 | caches healthy |
| `idleSpin` in 60fps segments | ~14ms/frame | ~8-16ms/frame | condvar sleep headroom, as designed |
| `submits` / forced | 7.7 / 3.3 | 9.4 / 4.5 | forced submits ~= half of all submits |

Still unverified by a human: no geometry corruption from the index cache (visual check).

---

## 3. External tooling

### 3.1 intel_gpu_top -- clocks and utilization (installed)

```bash
sudo intel_gpu_top            # interactive: RCS busy %, actual/requested freq, power
sudo intel_gpu_top -o - -s 200 > ~/Projects/Cemu-perf/gputop-4k.txt   # 200ms samples to file
```
Question it answers: is the iGPU sustaining max clocks during heavy scenes, or is it
frequency/power limited? A throttled 4K run would revise the 26/7 split above.

### 3.2 turbostat -- package power split (NOT installed)

```bash
sudo pacman -S turbostat                      # extra/turbostat 7.1.4-1
sudo turbostat --interval 1 --show PkgWatt,CorWatt,GFXWatt,GFXMHz,Busy%
```
Question: how is package power divided between cores and graphics? Post spin-to-block fix
the CPU side should be light, leaving headroom for the iGPU.

### 3.3 INTEL_MEASURE -- per-renderpass GPU attribution (no install needed)

Mesa built-in; the cheapest way to find *which* passes own the ~19ms at 1440p.

```bash
INTEL_MEASURE=type=render,file=~/Projects/Cemu-perf/measure-1440p.txt ./bin/Cemu_release
# other useful knobs: type=batch (per-submit), interval=<N> frames, type=frame
```
This is the next profiling step: it decides between shadow/AO graphic-pack reduction
(config-only), the occlusion-query renderpass-split fix, and the fp16 decompiler spike.
Also cross-checks the `gpuBusyUs` caveat in section 1.

Alternative quick look: `VK_INSTANCE_LAYERS=VK_LAYER_MESA_overlay` with
`VK_LAYER_MESA_OVERLAY_CONFIG=submit,draw,pipeline_graphics,comp_invocations`.

### 3.4 iaprof -- per-shader GPU flame graphs (experiment, unproven here)

Intel's EU-stall sampling profiler, <https://github.com/intel/iaprof>. This is the tool from
Brendan Gregg's [Doom GPU flame graphs post](https://www.brendangregg.com/blog/2025-05-01/doom-gpu-flame-graphs.html)
(2025-05-01), which attributed GZDoom GPU time as walls 41.4% / postprocessing 35.7% /
stenciling 17.2% / sprites 4.95%. Equivalent output for BOTW would name the exact shaders
worth optimizing -- strictly better than per-pass data, if it runs.

```bash
git clone --recursive https://github.com/intel/iaprof && cd iaprof
make deps && ./build.sh          # deps: libelf, clang, llvm, python-mako, cmake, zstd
sudo build/iaprof > profile.txt  # needs root: eBPF + EU stall sampling
cat profile.txt | iaprof flame > flame.svg
```

Prerequisites verified on this box (2026-07-26):

| requirement | status |
|---|---|
| `xe` kernel driver (not i915) | OK -- `xe` module loaded, card0 at 0000:00:02.0 |
| Kernel >= 6.15 | OK -- 7.1.3-arch1-1-ptl |
| BTF for kernel and module | OK -- `/sys/kernel/btf/vmlinux` and `/sys/kernel/btf/xe` both present |
| `CONFIG_DEBUG_INFO_BTF_MODULES` | implied OK (the `xe` BTF blob exists) |
| root for eBPF/eustalls | available via sudo |

Known unknowns -- why this is an experiment, not a plan:

- **Xe3 / Panther Lake is not on the supported list.** README names Arc B-series
  (Battlemage), Core Ultra with Arc graphics (Lunar Lake), and "other Xe2-based devices
  (untested)". Xe3 may work, may need patches, may not enumerate at all.
- **Vulkan/Mesa is not the documented path.** Docs target compute/AI (PyTorch, SYCL), and
  the CPU-side stack fusion hooks USDT probes in `libze_intel_gpu` (Level Zero), which needs
  a patched NEO build. Cemu goes through Mesa ANV, so expect the GPU-side flame graph to
  work at best *without* fused CPU stacks.
- **Frame pointers.** Arch system libraries are built without them, so any CPU-side stacks
  will be shallow. Only affects fusion, not GPU-side sampling.
- Gregg rates the whole setup "Nightmare! difficulty"; target overhead is <5%.

Verdict: time-box it, and only after INTEL_MEASURE (section 3.3) has answered the coarse
question for free.

### 3.5 perf -- CPU profiling (installed; low priority while GPU-bound)

`perf` IS present (an earlier 2026-07-24 note claiming otherwise was wrong).
`perf_event_paranoid=2`, so use sudo or lower it.

```bash
# CPU flame graph of the LatteThread specifically (find its TID in htop / ps -T)
sudo perf record -F 99 -t <LatteThread-TID> -g -- sleep 30
sudo perf script | stackcollapse-perf.pl | flamegraph.pl > latte.svg

# whole process
sudo perf record -F 99 -p $(pgrep Cemu_release) -g -- sleep 30

# off-CPU: where does the thread block? (independent cross-check of our wait buckets)
sudo perf record -e 'sched:sched_switch' -p $(pgrep Cemu_release) -g -- sleep 10

# cycle efficiency
sudo perf stat -d -p $(pgrep Cemu_release) -- sleep 10     # IPC, stalled cycles, cache misses
```

Use the profiling build for usable stacks (it has `-fno-omit-frame-pointer`; recipe in
`claude/todo.md` Notes). The release build is unstripped but frame-pointer-less: flat
profiles only, no reliable call graphs. Reference: <https://www.brendangregg.com/perf.html>.
