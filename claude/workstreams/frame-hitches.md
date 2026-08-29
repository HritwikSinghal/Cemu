---
name: frame-hitches
description: Frame SPIKES in BOTW as distinct from low average fps -- the hostAllocUs instrumentation added to catch mid-frame device allocations, the ranked hitch sources, and the measurement showing hitches are only 0.1-0.2% of frames so this is NOT the current problem
status: paused
---

# Frame hitches (spikes, not slowness)

## Current state

**Instrumented, measured, and DEPRIORITISED by its own measurement.**

Low fps and stutter are different problems with different causes, and it was worth separating them. The
answer turned out to be that BOTW on this box **does not have a stutter problem**: genuine hitches are
0.12-0.22% of frames, and the thing that feels bad is steady GPU-bound slowness at 34-35fps -- which is
[[botw-frame-budget]]'s problem, not this one.

The `hostAllocUs` / `hostAllocs` instrumentation added here is **built and worth keeping** regardless: it
is permanent diagnostic capability, near-zero cost when the profiling toggle is off, and it is what
allowed the deprioritisation to be a measurement instead of a guess. It is UNCOMMITTED, in the working
tree alongside [[occlusion-query-stalls]].

I overstated this earlier in the session -- I called mid-frame ring growth "your stutter" on the strength
of a microbenchmark of the allocation itself, before measuring how often it actually fires in gameplay.
It fires in 0.066% of frames. Recorded so the same mistake is not repeated.

## Next actions

1. Nothing active. Revisit only if the user reports hitching AFTER the steady-state fps problem is solved,
   or if a future capture shows `hostAllocUs` non-zero during normal play rather than during loads.
2. If revisited, start with source #1 below -- it is the largest and the fix direction is known.
3. Keep the instrumentation in whatever commit [[occlusion-query-stalls]] lands as (or split it out; it is
   independent and defensible on its own).

## Decisions

2026-08-29: added `hostAllocUs`/`hostAllocs` rather than any of the other candidate columns. It was the
only one that could isolate the top two hitch sources -- previously a ring growth hid inside
`textureUploadUs` (or `uniformUs`, depending on which path triggered it), indistinguishable from ordinary
work.

2026-08-29: gave the analyzer a hitch section SEPARATE from its slowest-5% table. That table averages, and
averaging is exactly what hides a hitch: one 200ms frame in 3000 is invisible in every mean and is also
the single most noticeable thing while playing.

2026-08-29: deprioritised on measurement. Not abandoned -- the instrumentation stays.

## Findings

### What was added (working tree, uncommitted)

| file:line | change |
|---|---|
| `LattePerformanceMonitor.h` bottleneck struct | `LattePerfNestingTimer tmrHostAlloc` + `LattePerfStatFrameCounter cntHostAllocs` |
| `LattePerformanceMonitor.cpp` | `frameFinished()` latches; two new CSV columns appended at the END of the row |
| `VKRMemoryManager.cpp:24` | `LATTE_PERF_SCOPE/COUNT` in `VKRSynchronizedRingAllocator::allocateAdditionalUploadBuffer` |
| `VKRMemoryManager.cpp:253` | same in `VkTextureChunkedHeap::allocateNewChunk` |
| `VKRMemoryManager.cpp:368` | same in `VkBufferChunkedHeap::allocateNewChunk` |
| `LatteOverlay.cpp:134` | live `HostAlloc: %u us (%u allocs)` line in the debug panel |
| `claude/tools/analyze_perfstats.py` | `hostAllocUs`/`hostAllocs` in the column lists + a new `hitches()` section |

Columns are appended at the END of the CSV row, so captures from older binaries still parse -- the
analyzer uses `csv.DictReader` and skips missing keys.

The analyzer's hitch section: threshold is `max(2x capture mean, 33.3ms)` -- relative to the capture's own
mean so it stays meaningful whether the capture sits at 60fps or 30fps -- and each spike is blamed on
whichever stage timer is furthest above its own median. Validated against a synthetic capture with planted
`hostAllocUs` spikes; it attributed both correctly.

### THE MEASUREMENT that deprioritised this (2026-08-29, 10568 steady frames at target settings)

```
frames with any host allocation : 7 of 10568  (0.066%)
total time in host allocs       : 158ms over the whole capture
worst single frame              : 67.7ms
frame indices                   : 583, 587, 650, 830, 874, 1007, 3078
```

Six of the seven are inside the first ~1000 frames, i.e. the warm-up / area-load window. **Mid-frame
device allocation is a LOADING phenomenon here, not a steady-gameplay one.** It is genuinely severe when
it fires -- 67.7ms is a visible freeze -- but it is not what the user feels while playing.

Overall hitch rate, both captures:

| | before (7322 frames) | after (10568 frames) |
|---|---|---|
| hitches (> 2x mean, floor 33.3ms) | 16 (0.22%) | 13 (0.12%) |
| total excess frametime | 1389ms | 804ms |
| dominant stage | idleSpin 7, submit 4, textureUpload 3, textureUpdate 2 | idleSpin 5, textureUpload 3, textureUpdate 3, submit 2 |
| frames missing 30fps | 2.4% | 2.4% |

**`idleSpin` dominating the hitch list is the interesting part** -- that is the Latte thread STARVED,
waiting for PM4 data from the guest CPU, not anything GPU-side. The worst frame in each capture
(207ms and 165ms) had `gpuBusyUs` of 722us and 711us respectively, with every other stage at ~0. Those are
pure guest-side / host-OS stalls -- plausibly asset streaming, disk I/O, or guest-side compilation --
and nothing in the Vulkan backend will fix them. If hitching is ever chased again, start there rather
than in the renderer.

### Ranked hitch sources (measured microbenchmarks + file:line reads)

| # | source | measured | file:line |
|---|---|---|---|
| 1 | Staging ring growth -- 32 MiB `vkCreateBuffer` + `vkAllocateMemory` + `vkMapMemory`, synchronous, mid-frame on the LatteThread | mean 2.98ms, **max 23.1ms** (8 iterations) | `VKRMemoryManager.cpp:24-49`, called from `:105` |
| 2 | Texture heap chunk growth, 128 MiB per chunk; worse on OOM, where `imageMemoryAllocate` deletes up to 20 textures per retry round and each is re-decoded on next use | mean 4.7ms, max 9.3ms | `VKRMemoryManager.cpp:261`, OOM path `:588-600` |
| 3 | Synchronous SHADER compile on the LatteThread. **`async_compile` does NOT cover this** -- it only covers pipeline objects | -- | `LatteShader.cpp:876` (`compileAsync=false`), GS/PS `:899`, `:930` |
| 4 | Synchronous pipeline compile when `IsAsyncPipelineAllowed` is false (tracing tool, 1600x1600 FBO, or no depth buffer with <=6 indices) | -- | `VulkanRendererCore.cpp:188-210`, compile `:262-264` |
| 5 | BOTW 83-slice array reload: `_botwLargeTexHax` forces a change verdict on the 1024x1024x83 array; `LatteTexture_ReloadData` re-decodes every slice of every mip -- ~42 MB, 83 staging reservations, 83 renderpass splits in one frame. Also the likeliest trigger of #1 | -- | `LatteTextureCache.cpp:234-246`, `LatteTextureLegacy.cpp:41-78` |
| 6 | GX2DrawDone full drain -- a STEADY per-frame sync, so it reads as baseline latency rather than a spike | -- | `LatteCommandProcessor.cpp:1422-1426`, `TextureReadbackVk.cpp:150` |

Note the caveat on #1 and #2: those are microbenchmarks of the allocation call itself. They say what one
costs, not how often it happens -- and the capture above says: almost never, during play.

### RULED OUT as already bounded -- do not re-survey

- Texture cache eviction: scans 25, deletes at most 10 per frame, explicit anti-stutter cap
  (`LatteTextureCache.cpp:371-397`).
- Buffer cache cleanup: one range per call (`LatteBufferCache.cpp:1733-1770`).
- Index cache `invalidateAll`: predicate-clear of a <=2048-entry map (`LatteIndices.cpp:191-194`).
  Microseconds.

### Possible NON-EMULATOR stutter source, unverified

`patch_DrawDistance.asm:16` in the Draw Distance graphic pack nops address `0x03857F58`, commented
*"Force the draw distance used for load balancing normally to be enabled"*. If that is BOTW's own adaptive
draw-distance relief valve, enabling the pack removes the game's load-shedding exactly where the budget is
tightest. This is an INFERENCE from the patch comment, not verified against game code.

Practical consequence regardless of whether the inference holds: **"pack off" is NOT equivalent to "pack on
with all presets at Medium"** -- an A/B must toggle the pack itself, not just lower the presets.

### Full CSV column inventory (38 columns after this change)

`frame, frameUs, gpuBusyUs, shaderUpdateUs, fboUpdateUs, textureUpdateUs, textureHashUs, textureUploadUs,
uniformUs, indexUs, bufferSyncUs, pipelineUs, descriptorSetsUs, renderpassUs, submitUs, gpuWaitUs,
idleSpinUs, guestFenceUs, drawsFirst, drawsFast, seqEndTexture, seqEndContextReg, indexCacheHit,
indexCacheMiss, pipelineMiss, descSetMiss, asyncSkippedDraws, submits, submitsForced, occlusionQueries,
textureReloads, textureReloadSlices, bytesUniform, bytesTexture, bytesIndex, vsyncLateUs, hostAllocUs,
hostAllocs`

Header at `LattePerformanceMonitor.cpp:221`, row written at `:71-86`.

Sources 3, 4, 5 and 6 are each already identifiable from existing columns (`shaderUpdateUs`, `pipelineUs`,
`textureUploadUs` + `textureReloadSlices`, `gpuWaitUs`); 1 and 2 needed the new column.
