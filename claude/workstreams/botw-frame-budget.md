---
name: botw-frame-budget
description: What BOTW actually costs per frame on Panther Lake and what render resolution reaches 60fps -- the P/F cost model, measured baselines, target-settings budget, resolution ladder, and how to run a capture that is not swamped by route noise
status: active
---

# BOTW frame budget and the cost model

## Current state

Measured 2026-08-29 at the user's real target settings: **the game is steadily GPU-bound at
~34-35 fps and is NOT stuttery.** Median frametime 28.5-29.0ms, GPU busy 84-88% of frametime,
70-80% of frames miss 60fps but only 2.4% miss 30fps, and genuine hitches are 0.12-0.22% of frames.
The gap to 60fps is a **1.75x GPU-work cut**, all of it steady-state.

The cost model is `GPU time = P * (pixels / 4K pixels) + F`, where P is pixel-scaled shading and
F is resolution-independent (geometry, per-draw GPU overhead, query stalls). P and F have only ever
been solved from 2026-07 captures taken WITHOUT the Draw Distance pack, so applying them to the
current config is an extrapolation, and it is currently the weakest link in every projection below.

BLOCKED ON: the resolution ladder (see Next actions). Everything else in the campaign -- whether FSR
alone suffices, whether VRS is mandatory, how much F matters -- is downstream of that one measurement.

## Next actions

0. **FIRST, and free: settle the display-output tax** ([[display-output-cost]]). The user reports the
   same settings run much smoother on the laptop panel than on this 4K/240Hz external, and all baselines
   below were captured on the slow one. Dropping DP-1 to 60Hz is one command and the game is capped at
   60fps anyway. Until this is pinned down, the resolution ladder would be measuring two variables.
1. **Resolution ladder at current settings, captured STANDING STILL** (at a fixed display mode). Three ~60s captures from one
   fixed viewpoint at a heavy vista, changing only the Graphics pack resolution preset:
   ```bash
   cd /home/hritwik/Projects/Cemu
   CEMU_PERFSTATS_CSV=~/Projects/Cemu-perf/ladder-1800p.csv ./bin/Cemu_release
   CEMU_PERFSTATS_CSV=~/Projects/Cemu-perf/ladder-1440p.csv ./bin/Cemu_release
   CEMU_PERFSTATS_CSV=~/Projects/Cemu-perf/ladder-1080p.csv ./bin/Cemu_release
   ```
   Solve P and F for the ACTUAL config, then read off which render resolution hits 16.7ms.
2. Depending on the answer: if 1440p or 1080p render clears 60fps, [[fsr-upscale-filter]] alone is
   the whole campaign. If F turns out to dominate, [[vrs-fragment-shading-rate]] will not save it
   either (VRS attacks P) and the work has to move to F-side reduction.
3. Optional and cheap once the ladder exists: `sudo intel_gpu_top` during a heavy scene to confirm the
   iGPU sustains clocks. A power-limited run would invalidate the P/F solve.

## Decisions

2026-07-26: campaign target changed from native 4K60 to render-below-output, after the 4K vs 1440p
experiment showed native 4K60 needs a ~3x pixel-cost cut. Not reachable.

2026-08-29: target settings fixed to the user's current config and the "increase draw distance
further" goal set aside. Plan against what is currently configured, not an aspirational setting set.

2026-08-29: **stop using free-roam captures for A/B.** Two captures of the same area (7442 and 10688
steady frames) could not resolve a 1-2ms effect -- within-bucket variance was 8ms. Draw-count
bucketing controls content only roughly. Stationary captures from a fixed viewpoint from now on.

## Findings

### Hardware and display (measured 2026-08-29)

- GPU `Intel(R) Arc(tm) B390 (PTL)`, Xe3, Mesa 26.2.1-arch1.1, Vulkan 1.4.354, `xe` kernel driver.
- Panel: **3840x2160 @ 240Hz** on DP-1. Cemu runs `fullscreen=true`, `FullscreenScaling=0`
  (keep-aspect), `UpscaleFilter=1` (bicubic), `DownscaleFilter=0` (linear).
- **CONSEQUENCE THAT REFRAMES THE WHOLE RESOLUTION QUESTION:** at 3200x1800 render on a 3840x2160
  window, `downscaling` is FALSE (`LatteRenderTarget.cpp:890` -- it needs `imageWidth <= effectiveWidth`
  in EITHER axis), so `GetConfig().upscale_filter` governs and the game is **already being bicubic
  upscaled to 4K**. 3200x1800 was never native on this panel. Cutting render resolution further is a
  change of degree, not of kind -- which makes [[fsr-upscale-filter]] a much smaller ask than it looks.

### Target settings (the user's stated minimum, do not plan below it)

Graphics pack: 3200x1800, Normal FXAA, Shadows Medium (100%), Shadow Draw Distance High.
Draw Distance pack: NPC/Enemies Ultra 1.5x, Terrain/Buildings High 1.25x, Trees High, Grass Medium,
Texture Distance Detail (LOD) Higher (-2). FPS++ : Normal Settings, 60FPS Limit.

| setting | what it changes | P or F |
|---|---|---|
| Resolution 3200x1800 | 2.5x per axis on every render-target `[TextureRedefine]` (Graphics/rules.txt:625-1157) | **P**, and it is the single highest-cost setting |
| Normal FXAA | `$fxaa=1`, the pack default | P (full-screen post) |
| Shadows Medium 100% | pack default, `$shadowRes=1`; shadow redefines multiply by `$shadowRes` ONLY, never by `$width/$height` (rules.txt:1161-1194) | F, and **zero delta from stock** |
| Shadow Draw Distance High | **INERT** -- see the dead-category finding below | neither |
| NPC Ultra 1.5x | `$actor=1.5`, patch_DrawDistance.asm:7-17 | F primary, P secondary |
| Terrain High 1.25x | `$object=1.25`; objectDivider `1.0-($object-1.0)=0.75`, so effective linear radius 1/sqrt(0.75) = 1.155x | F primary |
| Trees High | `$tree=0.375` vs default 0.5 = 1.33x further before the 3D->2D billboard swap | F primary (3D tree meshes replace 2-triangle billboards), P secondary (alpha-tested foliage overdraw) |
| Grass Medium | `$grass = 1000*1.0`, equals default | **zero** |
| Texture LOD Higher (-2) | `overwriteRelativeLodBias = -2` on art textures only (rules.txt:220-222); 2 mip levels sharper = 16x texel density at a given distance | **P** -- the only other P multiplier; compounds with AF 4x |
| FPS++ 60 limit | PPC constant patches only (patch_GameSpeed.asm:38-69) | neither -- no per-frame GPU cost |

Absolute shadow atlas at Medium: ~0.55 Mpx across five alternative (not concurrent) entries, against a
5.76 Mpx colour target. Under 10%, depth-only, and invariant to the resolution setting.

### DEAD CATEGORY: "Shadow Draw Distance" does nothing in pack v7

`$shadowNearbyStart` / `$shadowNearbyEnd` / `$shadowFarStart` / `$shadowFarEnd` are declared at
Graphics/rules.txt:24-27 and written by all five presets (:569-607), but are referenced by **nothing** --
no shader, no `[TextureRedefine]`, no `.asm`. Verified by grep across the entire graphicPacks tree; the
only pack variables any shader consumes are `$width`, `$height`, `$gameWidth`, `$gameHeight`,
`$shadowRes`, `$fxaa`, `$ultrawideHUDMode`, `$subPix`, `$edgeThreshold*`. Every preset in that category
costs zero. Keep it at High; it is free.

### Measured baselines, 2026-08-29 (the current reference; supersedes the 2026-07 aggregates)

Raw CSVs preserved at `~/Projects/Cemu-perf/botw-1800p-{before,after}.csv`. Free-roam, same area,
different routes -- comparable only in aggregate, NOT for A/B (see the Decisions note).

**DISPLAY CONTEXT, load-bearing:** both captures ran on the external Dell AW3225QF at
**3840x2160 @ 240Hz**, which the user reports is markedly SLOWER than the laptop panel at identical Cemu
settings. So every number below includes an unmeasured output-side tax -- a 16-tap bicubic blit instead
of the laptop's cheap linear one, full-screen compositing, and ~8 GB/s of continuous scanout bandwidth.
See [[display-output-cost]]. **The derived `F` of ~10.4ms is therefore partly suspect: some of it may be
display configuration rather than game cost.** Re-derive P and F only from captures taken at a FIXED and
deliberately chosen display mode.

| | before (7322 steady frames) | after (10568) |
|---|---|---|
| mean frametime | 25.05ms (39.9 fps) | 27.09ms (36.9 fps) |
| median frametime | 28.50ms (35.1 fps) | 29.01ms (34.5 fps) |
| p95 / p99 / max | 32.5 / 34.6 / 207.0ms | 32.6 / 34.3 / 165.3ms |
| GPU busy mean / median | 21.0 / 26.7ms | 23.8 / 27.5ms |
| **GPU busy / frametime** | **83.8%** | **87.9%** |
| misses 60fps | 70.4% | 80.4% |
| misses 30fps | 2.4% | 2.4% |
| gpuWait | 12.96ms (51.7%) | 15.52ms (57.3%) |
| idleSpin (mean / median / p95) | 3.43 / 0.04 / 15.95ms | 2.79 / 0.04 / 15.86ms |
| SUM all 15 CPU stages | 21.2ms incl. waits; ~8ms excl. | 23.1ms incl.; ~8ms excl. |
| draws (first + fast) | 876 + 3146 | 888 + 3547 |
| occlusion queries | 16.5 | 17.1 |
| index cache hit / miss | 3132 / 890 (**78%**) | 3523 / 912 (**79%**) |
| pipelineMiss / descSetMiss | 0.2 / 1.0 | 0.1 / 0.9 |
| asyncSkippedDraws | 0 | 0 |
| vsyncLateUs | 16.6ms | 19.5ms |

`idleSpin` is strongly bimodal (median 40us, p95 ~16ms): light scenes have huge condvar headroom, heavy
scenes have none. That is the spin-to-block fix working as designed, not a problem.

`vsyncLateUs` at 16-19ms per frame is a CONSEQUENCE of being GPU-bound (frames take 29ms against a
16.7ms budget), not an independent bug. It should collapse on its own once frametime approaches 16.7ms.

### The budget, and why F is now the worry

```
3200x1800 = 5,760,000 px ; 3840x2160 = 8,294,400 px ; ratio 0.69444
P at 1800p (from the 2026-07 fit, P_4K = 26ms) = 18.06ms
measured heavy-scene GPU busy at 1800p          = ~28.5ms
=> implied F                                     = ~10.4ms
```

**If F is really ~10.4ms it is 62% of the entire 16.7ms 60fps budget**, far worse than the 7.3ms the
2026-07 baselines showed, and consistent with the Draw Distance pack inflating per-draw cost.
Projections on that basis:

| render resolution | P | F | total | fps |
|---|---|---|---|---|
| 3200x1800 (today) | 18.1 | 10.4 | 28.5ms | 35 |
| 2560x1440 | 11.6 | 10.4 | 22.0ms | 45 |
| 1920x1080 | 6.5 | 10.4 | 16.9ms | 59 |

TREAT ALL THREE ROWS AS PROVISIONAL. `P_4K = 26ms` comes from captures without the Draw Distance pack,
and the pack inflates P too (LOD -2, foliage overdraw). The ladder replaces this whole table with
measurement. Do not build anything on these numbers first.

Model self-check on the OLD data: predicting 1440p from the 2026-07 fit gives 18.86ms against 18.9ms
measured -- the fit is internally consistent, it is the transfer to the new config that is unproven.

### Methodology: why the 2026-08-29 A/B was worthless, and what to do instead

The two captures differed in length (7322 vs 10568 steady frames) and in draw distribution (before peaks
at 5000-5500 draws/frame, after at 5500-6000), i.e. different routes. Draw-count bucketing was supposed
to hold content constant. It does not, well enough:

Within-capture test on the BEFORE capture -- does occlusion-query count predict GPU busy at equal draw
count? Per-bucket deltas came out `+0.4, -3.2, +5.0, +4.1, +0.4, -8.6, -0.1` ms. Pure noise with 8ms
swings. A 1-2ms effect is unresolvable this way.

Rules adopted:
- **Stand still.** Fixed viewpoint at a heavy vista, ~60s. Kills route variance almost entirely.
- Change exactly ONE variable between captures of a set.
- Never compare per-capture means; use the bucketed table, and distrust it below ~5ms of effect.
- Capture to `~/Projects/Cemu-perf/`, never `/tmp` (a 2026-07-26 reboot destroyed both original baselines).
- `python3 claude/tools/analyze_perfstats.py <csv> [more.csv ...]`. The analyzer uses `DictReader`, so
  captures from older binaries with fewer columns still parse.

### Related workstreams

Levers: [[fsr-upscale-filter]] (P), [[vrs-fragment-shading-rate]] (P), [[fp16-shader-precision]] (P),
[[occlusion-query-stalls]] (F). Stutter, which turned out NOT to be the problem: [[frame-hitches]].
