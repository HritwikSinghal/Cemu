# Cemu fork -- Panther Lake iGPU performance work

## What this repo is

Fork of [cemu-project/Cemu](https://github.com/cemu-project/Cemu) (Wii U emulator, C++20).
Purpose: performance work targeting Intel integrated GPUs -- specifically Panther Lake
(Xe3, B390) with the Mesa ANV Vulkan driver on Arch Linux. Working branch: `patch`
(upstream tracking on `main`). Fork-specific changes are described in the README's
"About this fork" section and tracked in GitHub issues #1 (profiling) and #2 (optimizations).

## Where live state lives (read on session start)

- `claude/todo.md` -- conventions, tiered bottleneck findings, active checklist.
- `claude/progress.md` -- running log; the LATEST dated entry ends with the current
  next actions. Start there for catch-up.
- `claude/profiling.md` -- how to capture and read perf data (this fork's instrumentation
  plus intel_gpu_top / turbostat / INTEL_MEASURE / iaprof / perf recipes), the `gpuBusyUs`
  attribution caveat, and the measured baseline numbers. Read before profiling anything.
- `claude/tools/analyze_perfstats.py` -- analyzer for `CEMU_PERFSTATS_CSV` captures.

## Architecture crash course

- Guest PPC threads (1-3 host threads, recompiler) produce GX2 command data; a
  lock-free ring in `src/Cafe/OS/libs/TCL/TCL.cpp` carries indirect-buffer pointers to
  the single GPU emulation thread ("LatteThread").
- LatteThread does everything GPU-related: PM4 parsing (`src/Cafe/HW/Latte/Core/
  LatteCommandProcessor.cpp`), texture/buffer/shader caches (`src/Cafe/HW/Latte/Core/`),
  shader decompilation (`src/Cafe/HW/Latte/LegacyShaderDecompiler/`), and the Vulkan
  backend (`src/Cafe/HW/Latte/Renderer/Vulkan/`).
- Draws are batched into "draw sequences"; the first draw of a sequence takes the slow
  path (`draw_execute_first`, VulkanRendererCore.cpp). Almost any state change ends a
  sequence, which is why first-draw costs dominate in BOTW.
- Emulated 60Hz vsync is a polled virtual timer serviced from the LatteThread
  (`LatteTiming.cpp`) -- code that blocks that thread must stay time-bounded.

## Profiling instrumentation (fork-specific)

- `performanceMonitor.bottleneck` (LattePerformanceMonitor.h): stage timers + per-frame
  counters + GPU busy time. Add call sites with `LATTE_PERF_SCOPE/COUNT/ADD`; all values
  must be written from the LatteThread only; timers are nesting-safe.
- Runtime toggle `g_lattePerfStatsEnabled`: on while the debug overlay is shown
  (Options > General settings > Graphics > Overlay > Debug) or while `CEMU_PERFSTATS_CSV=<path>`
  is set (per-frame CSV). Zero-ish cost when off -- keep it that way for new call sites.

## Conventions

- ASCII-only in code, comments, and docs.
- Commits follow upstream style: `Latte:`/`Vulkan:`/`docs:` prefixed, imperative subject.
- Pushes to `patch` trigger the "Build check" GitHub workflow (~35 min) -- that is the
  compile gate; check the latest run before building on recent commits.
- Build locally per BUILD.md; profiling build recipe is in `claude/todo.md`.
- Perf changes need in-game verification (BOTW heavy scenes) plus before/after numbers
  from the overlay/CSV instrumentation before being considered done.
