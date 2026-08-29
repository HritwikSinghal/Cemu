# Cemu -- Todo

## Notes & Conventions
- Fresh session: read the LATEST dated entry in claude/progress.md -- it ends with the current next actions. Repo-level durable facts are in the root CLAUDE.md. How to measure: claude/profiling.md. Per-topic detail lives in claude/workstreams/ -- open ONLY the file whose description matches the task; do not bulk-read them.
- GOAL (revised 2026-08-29): Zelda BOTW at a locked, stutter-free 60fps at the user's CURRENT settings (3200x1800 render, Draw Distance pack at Ultra, output to the 3840x2160 panel). The earlier "increase draw distance further" goal is set aside. Native 4K60 was measured out of reach in 2026-07 and remains so. Working branch: `patch`.
- CURRENT GAP (measured 2026-08-29): ~34-35fps, steady, 84-88% GPU-bound. Needs a ~1.75x GPU-work cut. Full budget in claude/workstreams/botw-frame-budget.md.
- Fork context: 7385b18 fixed an ANV Xe3 device-loss (identity swizzle on framebuffer attachments) -- required just to boot BOTW on this GPU.
- Profiling build: `cmake -S . -B build-prof -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCEMU_CXX_FLAGS="-fno-omit-frame-pointer" -DCMAKE_INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO=OFF -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -G Ninja`
- All GX2->Vulkan emulation runs on ONE host thread ("LatteThread", src/Cafe/HW/Latte/Core/LatteThread.cpp:117). Overlay debug stats: VulkanRenderer::AppendOverlayDebugInfo (VulkanRenderer.cpp:3911).
- CAPTURE HYGIENE: never write CEMU_PERFSTATS_CSV to /tmp -- the 2026-07-26 reboot destroyed both baseline captures. Use ~/Projects/Cemu-perf/ with the variable under test in the filename. **STAND STILL**: as of 2026-08-29, free-roam captures are banned for A/B -- two 7k-10k-frame free-roam captures could not resolve a 1-2ms effect because within-bucket variance was 8ms. Fixed viewpoint at a heavy vista, ~60s, ONE variable changed. Also fix the display mode: output configuration is itself a performance variable (claude/workstreams/display-output-cost.md).
- Analysis tool: `python3 claude/tools/analyze_perfstats.py <capture.csv> [more.csv ...]` -- per-file summary plus, for 2+ files, the draw-count-bucketed like-for-like table. Compare via that table, never via per-capture means (scene mix dominates).
- UPSTREAM SYNC: the `upstream` remote (cemu-project/Cemu) was added 2026-08-28; `origin` is the fork. Local `main` tracks the FORK's main, so it does NOT advance on its own -- always `git fetch upstream` and diff against `upstream/main`, never against local `main`.
- RELEASES: fork releases are Linux-x64-only, via `.github/workflows/deploy_fork_release.yml`, triggered by pushing a `vMAJOR.MINOR-perfN` tag; it builds, then PUBLISHES the release immediately -- there is no draft gate, so the tag push is the point of no return. What keeps a bad artifact from going public is that every guard (tag regex, the version assert against the built binary, zip packaging) fails the job before the publish step, which is last. The inherited `deploy_release.yml` + `determine_release_version.yml` pair CANNOT work on a fork (bumps the newest tag from the releases API, exits 1 when there are none, and rejects the -perfN suffix) -- do not try to use it. Fork builds must set EMULATOR_VERSION_SUFFIX or they report the same version string as upstream.
- HISTORY SHAPE: `git log upstream/main..patch` is the entire fork -- one logical commit per change, with all documentation churn in a single `docs:` commit. Keep it that way: squash doc-only commits before pushing instead of accumulating them. A rewrite invalidates every fork sha, so sha citations in these files must be remapped afterwards.
- CI SCOPE: build_check.yml builds Linux x64/arm + macOS x86_64/arm64 + Windows. Windows/MSVC is far stricter than the local clang build and has already caught a portability bug clang accepts (C7626 on an unnamed typedef struct with a member function, b3922b6). After touching a widely-included header, check the build-windows job specifically -- and note it stops at the FIRST error, so one green-looking fix may reveal more.
- MEASURED VERDICT (do not re-litigate without new data): GPU-bound at every resolution tested, reconfirmed 2026-08-29 at the current settings (GPU busy 84-88% of frametime). All 15 CPU stage timers together are ~8ms/frame and hidden under 13-15.5ms of gpuWait. CPU-side work (register dirty-flag scheme, first-draw costs, texture decode-into-staging) is DEPRIORITIZED until the GPU side is broken.
- SECOND MEASURED VERDICT (2026-08-29): this is a STEADY-SLOWNESS problem, not a stutter problem. Genuine hitches are 0.12-0.22% of frames and the worst ones are guest-side CPU stalls (idleSpin), not renderer work. Do not spend effort on hitching unless the user reports it after fps is fixed.
- BEWARE the instrumentation blind spot: gpuBusyUs brackets only Cemu's own command buffers. Compositor passes and display scanout contend for the same iGPU and the same LPDDR bandwidth and never appear in any column -- they inflate gpuBusyUs without being attributable to it. See claude/workstreams/display-output-cost.md.

## Perf campaign: measurement
- [x] 4K + 1440p baselines (2026-07-25/26). Superseded as the reference by the 2026-08-29 captures at the
      real target settings, but the P/F derivation still comes from them -- claude/workstreams/botw-frame-budget.md
- [x] Baseline at the ACTUAL target settings (2026-08-29): ~34-35fps steady, 84-88% GPU-bound, hitches
      0.1-0.2%. Raw CSVs at ~/Projects/Cemu-perf/botw-1800p-{before,after}.csv (these ones survived).
- [ ] NEXT #1, free: settle the display-output tax -- claude/workstreams/display-output-cost.md
- [ ] NEXT #2: resolution ladder at current settings, standing still, at a fixed display mode --
      claude/workstreams/botw-frame-budget.md
- [ ] Power/clock check: `sudo intel_gpu_top` during a heavy scene (profiling.md 3.1). A throttled run
      would revise the P/F split. `turbostat` still NOT installed (extra/turbostat) for the power split.
- [ ] Per-renderpass attribution: `INTEL_MEASURE=type=render,file=...` (profiling.md 3.3). Free, no install.
      Would independently cross-check where the GPU time actually goes.
- [ ] Optional, time-boxed: iaprof per-shader GPU flame graphs (profiling.md 3.4). Box prereqs verified OK;
      Xe3 support and the Vulkan/ANV path are the open unknowns. Only after INTEL_MEASURE.
- [ ] Vsync experiment: FIFO vs MAILBOX vs Immediate -- LOW PRIORITY, captures show a smooth GPU-bound
      equilibrium, not a FIFO halving pattern.
- [ ] User visual check STILL pending: no geometry corruption from the index cache (a7e2adb).

## Upstream sync + fork releases (2026-08-28)
- [x] Added the `upstream` remote and reviewed all 12 commits in 3005cb7..upstream/main (2026-07-18 -> 2026-08-21). None perf-related; only 50b9e4b (fence errors -> UnrecoverableError) matters here, and it is now merged.
- [x] Merged upstream/main into `patch`; only VulkanRenderer.cpp overlapped and the hunks did not collide.
- [x] Fixed the long-red build-windows job: MSVC C7626 on the unnamed `performanceMonitor_t` typedef struct (b3922b6). Linux/macOS were always green, which is why it went unnoticed. All seven Build check jobs pass on this source tree.
- [x] EMULATOR_VERSION_SUFFIX CMake cache var so fork builds report "Cemu 2.6-perfN" instead of being indistinguishable from upstream's "Cemu 2.6" (49042fe).
- [x] Linux-only fork release workflow (08a89cb), publishing directly on a `v*-perf*` tag push rather than drafting (e19754b).
- [x] Recomposed the fork into 10 logical commits replayed linearly onto upstream 5ead580, verified byte-identical (tree 18ccd63) before and after.
- [x] RELEASED v2.6-perf2, live at https://github.com/HritwikSinghal/Cemu/releases/tag/v2.6-perf2 -- run 33159982801, 29m32s, all 14 steps green including the version assert. Artifacts: `cemu-2.6-perf2-linux-x64.zip` (32 MB) + `Cemu-2.6-perf2-x86_64.AppImage` (60 MB). Only tag on the remote; there are no other releases.
- [ ] Consider overriding the AppImage self-update target: `dist/linux/appimage.sh` hardcodes `UPD_INFO=gh-releases-zsync|cemu-project|Cemu|ci|...`, so letting the AppImage update itself replaces the fork build with an upstream CI build. Currently only called out in the release notes; a real fix means patching the shared script (adds upstream merge-conflict surface).

## Perf campaign: instrumentation (IMPLEMENTED 2026-07-25)
- [x] GPU timestamps: vkCmdWriteTimestamp per command buffer + per-frame "GPU busy" ms in overlay (query pool in VulkanRenderer, readback in ProcessFinishedCommandBuffers)
- [x] Per-stage CPU timers + event counters on the Latte thread: 15 timers / 18 counters in performanceMonitor.bottleneck (LattePerformanceMonitor.h), call sites across Latte core + Vulkan draw path
- [x] Runtime toggle: collection gated on g_lattePerfStatsEnabled = overlay debug checkbox (Options > General > Graphics > Overlay > Debug) OR active CSV dump; single-branch cost when off
- [x] Per-frame CSV dump: set CEMU_PERFSTATS_CSV=/path/file.csv env var (forces collection on, one row per frame, header included)
- [x] `hostAllocUs` / `hostAllocs` (2026-08-29, committed 8dacb00): times and counts mid-frame synchronous device
  allocations on the LatteThread -- the staging ring grower and both chunked-heap growers
  (VKRMemoryManager.cpp allocateAdditionalUploadBuffer / VkTextureChunkedHeap::allocateNewChunk /
  VkBufferChunkedHeap::allocateNewChunk). This is the ONLY column that separates a hitch from steady
  slowness: previously a ring growth hid inside textureUploadUs (or uniformUs, depending on which path
  triggered it), indistinguishable from ordinary work. Also on the overlay panel. CSV gained two columns at
  the END of the row, so older captures still parse (the analyzer uses DictReader and skips missing keys).
- [x] Analyzer hitch section (2026-08-29, committed 8dacb00): `analyze_perfstats.py` gained a per-spike section, deliberately
  separate from the slowest-5% table -- that table averages, and averaging is exactly what hides a hitch.
  Threshold is 2x the capture's own mean with a 33.3ms floor, and each spike is blamed on whichever stage
  timer is furthest above its own median. Validated against a synthetic capture with planted spikes.
- [ ] Optional flagship: Tracy integration (CPU zones + Vulkan GPU context) -- only if overlay/CSV prove insufficient. NOTE: low value while GPU-bound; Tracy would sharpen CPU attribution we already know is ~7ms/frame.
- How to read the overlay panel: "SeqEnd tex/ctxReg" = sequence-break causes (validates dirty-flag thesis); "IdxCache hit/miss" = index cache effectiveness (2048-entry map since a7e2adb; measured 73-75% hit in-game); "Waits gpu/idleSpin/guestFence" = stall buckets; "GPU busy" vs frametime = GPU-bound vs CPU-bound (attribution caveat: claude/profiling.md section 1).
- External tool recipes (INTEL_MEASURE, intel_gpu_top, turbostat, iaprof, perf) all live in claude/profiling.md section 3 -- do not re-research them.

## Perf campaign: optimization candidates (verify with profile first)
PRIORITIZATION AS OF 2026-08-29 (supersedes the 2026-07-26 ranking). GPU-side first; rank by whether a
candidate cuts GPU work, not CPU work. All four below were scoped by agent survey on 2026-08-29 with
file:line blueprints; two of the load-bearing claims were adversarially refuted and survived.
  (a) occlusion-query WAIT_BIT removal -- BUILT, A/B REFUTED IT, REVERTED 2026-08-29. Do not re-propose
      without a stationary A/B showing a win. See Tier 2 below.
  (b) FSR1 upscale filter -- biggest lever, because it makes render resolution independently reducible.
      ~180 LOC for an EASU-only first commit. See the FSR section below.
  (c) VRS 2x2 via dynamic state -- attacks P, est. 4-7ms at 1800p. See the VRS section below.
  (d) fp16 -- biggest artifact risk, do last, opt-in. See the fp16 section below.
DEMOTED: shadow/AO graphic-pack reduction was previously ranked "config-only, free, try it first". It is
much weaker than that reads. Shadow maps do NOT scale with $width/$height (Graphics/rules.txt:1161-1194
multiply by $shadowRes only), the user is already at Medium = stock, and the whole shadow atlas is
~0.55 Mpx depth-only against a 5.76 Mpx colour target. Dropping to Low removes ~0.41 Mpx of depth raster.
Shadow SAMPLING in the main pass is screen-resolution-bound and barely moves with $shadowRes.
ALSO DEAD: the "Shadow Draw Distance" pack category does nothing at all in installed pack version 7 --
$shadowNearbyStart/$shadowNearbyEnd/$shadowFarStart/$shadowFarEnd are declared (Graphics/rules.txt:24-27)
and written by all five presets (:569-607) but referenced by no shader, no [TextureRedefine] and no .asm.
Every preset in that category costs zero. Verified by grep across the whole graphicPacks tree.
Everything under "first-draw"/"per-draw CPU" is CPU-side and parked (see Notes & Conventions verdict).
- [ ] UMA zero-copy buffer cache: vertex/attr uploads double-copy (memcpy -> staging ring -> vkCmdCopyBuffer -> m_bufferCache created with flags 0; VulkanRenderer.cpp:3834,3837). Direct host-visible write or the disabled host-import path (:3822). Needs hazard handling for in-flight reads.
- [ ] Texture hash cost: LatteTexture_CalculateTextureDataHash samples every live texture's guest memory every frame (LatteTextureCache.cpp:42)
- [ ] Per-draw texture binding: descriptor decode + linear bucket scan per unit per stage (LatteTextureLegacy.cpp:111, LatteTextureView.cpp:147)
- [ ] Barrier / renderpass-split count (vk_accurate_barriers read at VulkanRendererCore.cpp:1205; overlay Barriers/frame + BeginRP/frame)
- [ ] Async-compile warm-up: interpreter fallback when heavy scenes hit fresh code (PPCRecompiler.cpp:453 worker polls 10ms); test PPCREC_FORCE_SYNCHRONOUS_COMPILATION
- [ ] GX2DrawDone forces full sync unconditionally on Vulkan (GX2_Event.cpp:221) -- measure what honoring gx2drawdone_sync=false would save (correctness risk, experiment only)

## Stutter (frame SPIKES) -- distinct from low average fps (2026-08-29)
Low fps and stutter are different problems with different causes, and the GPU-bound verdict above is
about fps only. A spike is a mid-frame synchronous device allocation on the LatteThread. Measured on
this box, not inferred. `hostAllocUs`/`hostAllocs` (added 2026-08-29) isolate the top two; before that
they hid inside whichever stage triggered the growth.
- [ ] #1 Staging ring growth -- MEASURED mean 2.98ms, MAX 23.1ms. `allocateAdditionalUploadBuffer`
  (VKRMemoryManager.cpp:24-49) does vkCreateBuffer + vkAllocateMemory + vkMapMemory for 32 MiB
  synchronously mid-frame, from AllocateBufferMemory (:105). One occurrence turns a 16ms frame into
  39ms. Fix direction: pre-grow at load, grow off-thread, or grow in larger steps less often.
- [ ] #2 Texture heap chunk growth -- MEASURED mean 4.7ms, max 9.3ms, 128 MiB per chunk
  (VKRMemoryManager.cpp:261). Worse on OOM: `imageMemoryAllocate` (:588-600) deletes up to 20 textures
  per retry round and each is re-decoded on next use, spreading one OOM over several frames.
- [ ] #3 Synchronous SHADER compile on the LatteThread -- `LatteShader_CreateRendererShader(..., false)`
  (LatteShader.cpp:876, :899, :930). NOTE: `async_compile` does NOT cover this; it only covers pipeline
  objects. Lands in shaderUpdateUs.
- [ ] #4 Synchronous pipeline compile when `IsAsyncPipelineAllowed` is false (VulkanRendererCore.cpp:188-210,
  compile at :262-264). Lands in pipelineUs.
- [ ] #5 BOTW 83-slice array reload -- `_botwLargeTexHax` (LatteTextureCache.cpp:234-246) forces a change
  verdict; LatteTexture_ReloadData re-decodes every slice of every mip: ~42 MB, 83 staging reservations,
  83 renderpass splits in one frame. Also the likeliest trigger of #1.
- [ ] #6 GX2DrawDone full drain -- steady per-frame sync rather than a spike; reads as baseline latency.
- RULED OUT as already bounded (do not re-survey): texture cache eviction (explicit cap of 10 deletes/frame,
  LatteTextureCache.cpp:371-397), buffer cache cleanup (one range per call, LatteBufferCache.cpp:1733-1770),
  index cache invalidateAll (predicate-clear of a <=2048-entry map, microseconds).
- POSSIBLE NON-EMULATOR STUTTER SOURCE: the Draw Distance graphic pack's patch_DrawDistance.asm:16 nops
  0x03857F58, commented "Force the draw distance used for load balancing normally to be enabled". If that
  is BOTW's own adaptive draw-distance relief valve, the pack removes the game's load-shedding exactly
  where the budget is tightest. INFERENCE from the patch comment, not verified against game code. It also
  means "pack off" is NOT equivalent to "pack on with presets lowered" -- an A/B must toggle the pack.

## Perf campaign: round-2 candidates (2026-07-25, 5-agent survey)
Cross-cutting: any texture or context-register change ends the current "draw sequence" (LatteCommandProcessor.cpp:1041-1052,1119-1129), so in BOTW most draws take the slow first-draw path. Several items below share that trigger -- a register-write dirty-flag scheme would amortize descriptor/pipeline/uniform/FBO recomputation together.

### Tier 1 (high impact, low risk)
- [x] Index cache (DONE a7e2adb): persistent robin_hood map (2048-entry cap, insert-time sweep, sampled validation hash on hits since explicit invalidation only ever covered the immediate-mode scratch buffer). Was 8 slots / ~0% hit rate.
- [x] Texture upload zero-fill (DONE 3a562a2): buffer kept at high-water mark across uploads, 64 MiB release valve. Was full memset per upload.
- [x] LatteThread busy-spin (DONE 3637d61): consumer now blocks on a condvar signaled by TCLWriteCmd, bounded at 200us because emulated vsync is polled from that loop; IT_WAIT_REG_MEM loop pauses + throttles clock reads to every 64th iteration; producer full-ring wait yields after 4096 spins.
- [ ] Per-first-draw hashing: descriptor-set state hash computed 3x per first-draw (GetDescriptorSetStateHash, VulkanRendererCore.cpp:519-579; ds_cache map alone measured 1.71-3.16% of total CPU per note at VulkanRenderer.h:98-101) + full pipeline hash per first-draw with double robin_hood lookup (:46,:143-158) + minimal pipeline hash on EVERY fast draw (:1609). Fix: dirty-flag gating per state group.
- [ ] Uniform re-assembly/upload per sequence-start with no change detection: full ALU-const bank memcpy + remapped-uniform scatter-gather (dozens-hundreds of 16-byte copies) + full ring upload on every first-draw (VulkanRendererCore.cpp:375-450, LatteBufferData.cpp:73-142); ring wrap can block on fence (:318-357). Incremental dirty-mask path exists (:452) but first-draw ignores it. Fix: extend dirty-mask to first-draws; content-hash to reuse previous ring offset.

### Tier 2
- [ ] FBO fully re-derived from raw registers per draw-pass restart: UpdateCurrentFBO decodes up to 8 CBs + DB + up to 9 view-cache lookups (LatteRenderTarget.cpp:438,302,568) and restarts happen on any context-reg write, not just RT changes.
- [ ] Full texture reload on any change: all mips x all slices re-decoded/re-uploaded, no partial path (LatteTextureLegacy.cpp:39-73); BOTW's 1024x1024x83-slice array workaround (LatteTextureCache.cpp:233,239) confirms the pattern. Fix: per-slice/mip dirty tracking via LatteTextureSliceMipInfo.
- [~] Texture upload double-copy -- INVESTIGATED 2026-08-29, verdict DO NOT BUILD. Real (decode into std::vector scratch at LatteTextureLoader.cpp:639,649, memcpy into staging at VulkanRenderer.cpp:3621-3623 -- the old :3502-3520 citation has drifted), and direct-decode is NOT a write-combined-scatter regression (staging is WC: measured read 0.08 GB/s vs write 84.65 GB/s on B390; but decoder writes are linear 64B cache-line-aligned runs, AddrLibFastDecode.h:115-168, and no decoder reads back from output). It still does not pay: textureUpload is ~2.2ms/frame against 25.6ms of gpuWait slack, so eliminating 100% of it changes frametime by zero while heavy scenes stay GPU-bound. It is also not a local edit -- staging is acquired in the renderer-agnostic loader, so it needs a new Renderer interface across Vulkan/OpenGL/Metal, plus moving the ring reservation ahead of AllocateOnHost. Mild adverse effect: holding the reservation across the decode lengthens ring occupancy, feeding the hitch source below. Revisit ONLY if the GPU side is broken first and textureUpload is then on the critical path.
- [x] Occlusion queries -- IMPLEMENTED then REVERTED 2026-08-29 (A/B showed no win; user chose not to
  carry sync-path risk for no measured gain). Diff preserved at
  `~/Projects/Cemu-perf/occlusion-query-waitbit-removal.patch`. It removed the per-fragment
  `vkCmdCopyQueryPoolResults` with `VK_QUERY_RESULT_WAIT_BIT` and read results on the host via
  `vkGetQueryPoolResults`, gated on the existing command-buffer fence check at VulkanQuery.cpp:106, and
  deleted the now-dead result buffer (VulkanRenderer.cpp:834, :899, VulkanRenderer.h:720). Full record,
  including the Mesa mechanism analysis and the refuting numbers: claude/workstreams/occlusion-query-stalls.md.
  - CORRECTION to the original entry: "in-renderpass queries" is NOT viable. Vulkan requires a query to begin and end within the same subpass OR both outside one; Cemu's query windows deliberately span whole renderpasses, so moving them inside forces re-fragmentation at every renderpass boundary -- strictly more resets and copies. BOTH `draw_endRenderPass` calls (VulkanQuery.cpp:58, :86) are load-bearing and are now commented as such. The begin-side split is required by `vkCmdResetQueryPool` (renderpass="outside" in the bundled vk.xml), the end-side by the begin/end symmetry rule.
  - Availability-bit polling is also unnecessary: the fence gate is strictly stronger. Precedent already in tree at VulkanRenderer.cpp:2207 for the GPU timestamp pool.
  - Mechanism (verified in the installed Mesa 26.1.4 source): for VK_QUERY_TYPE_OCCLUSION ANV emits ANV_PIPE_CS_STALL_BIT unconditionally (genX_query.c:1743), and WAIT_BIT adds an MI_SEMAPHORE_WAIT on the availability dword (genX_query.c:1766-1773) that only lands after the 3D pipeline drains. ~22 such drains per heavy frame, resolution-independent -- the shape of the measured F. Attribution of F to these drains is still an INFERENCE; the A/B capture is what settles it.
  - Deviation from the scout's plan, deliberate: on VK_NOT_READY it falls back to a HOST-side wait rather than deferring the fragment. `LatteQuery_UpdateFinishedQueriesForceFinishAll` (LatteQuery.cpp:112-117) spins on an unbounded `while(true)` until every fragment resolves, so "retry next poll" would convert a driver spec violation into a hard Latte-thread hang.
- [ ] Occlusion queries, maximal variant (NOT done): drop `RequestSubmitSoon()` at VulkanQuery.cpp:140 (it chops the command buffer within 10 draws of every query end, and each chop re-fragments every active query), and move the pool reset to host-side `vkResetQueryPool` via hostQueryReset. Watch cntSubmits/cntSubmitsForced/tmrGpuWait together; revert if tmrGpuWait rises more than cntSubmits falls.
- [ ] Readback ForceFinish: force-submits the partial cmd buffer then vkWaitForFences(UINT64_MAX) on the Latte thread (TextureReadbackVk.cpp:150-154, VulkanRenderer.cpp:2300-2306; triggered from LatteCommandProcessor.cpp:1406,1714).
- [ ] 1-frame-in-flight cap: SwapBuffer blocks on prev-frame fence + acquire fence (VulkanRenderer.cpp:3007-3010; m_maxQueued=1 SwapchainInfoVk.cpp:369) -- when GPU-bound, recording thread sits idle. Consider tunable 2-frames-in-flight (latency tradeoff).
- [ ] Vsync pacing jitter: emulated 60Hz vsync polled only at CP boundaries; heavy frames get late/bursty ticks = microstutter (LatteTiming.cpp:156-175; accumulator LatteCommandProcessor.cpp:1517). Fix: dedicated timer thread. WARNING: VSync mode 3 (SYNC_AND_LIMIT) is broken on Linux -- sets host-driven flag but VsyncDriver is a Windows-only stub -> flip accounting freezes (LatteTiming.cpp:56-64, VsyncDriver.cpp:205). Current config uses MAILBOX (VSync=2) = safe; do not use mode 3.
- [ ] IT_WAIT_REG_MEM fence spin: clock_gettime(CLOCK_MONOTONIC_RAW) + async-cmd check every iteration, no _mm_pause (LatteCommandProcessor.cpp:475-529).
- [ ] PS input table rebuilt from 32 SPI registers every draw, no change detection (LatteShader_CreatePSInputTable, LatteShader.cpp:222-314, called via :1219).

### Tier 3 (cheap fixes or conditional/risky)
- [ ] False sharing: per-core GX2 write-gather state shares a cache line across cores (GX2_Command.h:7-15, currentWritePtr written per emitted dword) -> alignas(64); TCL ring read/write indices adjacent (TCL.cpp:68-69) -> alignas(64).
- [ ] Every submit resets all bindings (resetCommandBufferState, VulkanRenderer.h:402); acquire does a standalone forced submit (VulkanRenderer.cpp:2916) -> re-bind churn. Fix: coalesce acquire wait-semaphore into present submit; fewer query-driven submits.
- [ ] sync_inputTexturesChanged loops fboCandidates of all 3 descriptor sets per first-draw (VulkanRendererCore.cpp:998,1187-1223).
- [ ] Aux-hash walks VS+PS texture-unit registers every draw even on shader-cache hit (LatteShader.cpp:1066,598-660). Gate on texture-reg dirty flag.
- [ ] Cache vkGetImageMemoryRequirements alignment on LatteTextureVk at creation; currently re-queried per slice upload with .size discarded (VulkanRenderer.cpp:3510-3514).
- [ ] Fuse memcpy + hashPage passes in buffer-cache page upload (LatteBufferCache.cpp:1255-1272, in-code TODO).
- [ ] GPU-side research spike: fp16/RelaxedPrecision emission -- SCOPED 2026-08-29, see the fp16 section below.
- [ ] Array-GPR zero-init: array-GPR shaders declare vec4[128] and zero-init all 128 regardless of use
  (LatteDecompilerEmitGLSL.cpp:3983-3996). The behavior is confirmed, but the previously recorded fix
  ("size to actual gprUseMask") is WRONG and would introduce out-of-bounds writes: the array path is
  entered only when `usesRelativeGPRWrite` (LatteDecompilerRegisterDataTypeTracker.cpp:21-23), and
  gprUseMask is documented as ignoring relative-index accesses (LatteDecompilerInternal.h:237). The
  analyzer only patches the mask for relative READS (LatteDecompilerAnalyzer.cpp:977-1013), never writes.
  Correct bound is the shader's hardware GPR allocation (SQ_PGM_RESOURCES.NUM_GPRS). Separate spike;
  unrelated to fp16.

### Spot-check additions (2026-07-25, main-agent read of draw path)
- [ ] First-draw re-syncs ALL buffers unconditionally: draw_execute_first passes 0xFFFFFFFF dirty masks to LatteBufferCache_Sync (VulkanRendererCore.cpp:1436), so every attribute buffer and every uniform block in the shader's quickBufferList pays an interval-tree lookup + rebind per first-draw even when untouched (LatteBufferData.cpp:188-295; in-code TODO :279 confirms the skip opportunity). The incremental path also flips ALL attrib buffers dirty whenever maxIndex/maxInstance grows (:199-213). Same register-dirty-flag scheme as the other first-draw items would fix it.
- [ ] Async-compile skipped draws still pay almost the full CPU path: pipeline validity is only checked at VulkanRendererCore.cpp:1443, AFTER streamout prep, 3x uniform upload, index decode+alloc+flush, buffer-cache sync, and pipeline hash/lookup. During compile-stutter windows Cemu burns near-full per-draw CPU on draws it then discards. Fix: hoist the pipeline lookup + VK_NULL_HANDLE check above the uniform/index/buffer work.

## Workstreams (per-topic detail; bodies are NOT loaded at session start -- open only what matches)
- [ ] display-output-cost -- ACTIVE, run first, zero code -- claude/workstreams/display-output-cost.md
      Same settings are much smoother on the laptop panel than on the 4K/240Hz external. Four
      compounding output-side costs, three fixable in one command. All 2026-08-29 baselines were
      captured on the SLOW display, so this may be contaminating the measured F.
- [ ] botw-frame-budget -- ACTIVE, blocked on the resolution ladder -- claude/workstreams/botw-frame-budget.md
      The P/F cost model, measured baselines, target-settings budget, and the capture methodology.
      Everything else is downstream of the ladder.
- [ ] fsr-upscale-filter -- PAUSED, scoped, ~180 LOC first commit -- claude/workstreams/fsr-upscale-filter.md
      Biggest lever. Render-below-output confirmed viable by adversarial refuter.
- [ ] vrs-fragment-shading-rate -- PAUSED, scoped, est. 4-7ms -- claude/workstreams/vrs-fragment-shading-rate.md
      Dynamic-state approach; pipeline hash and shader cache confirmed untouched by adversarial refuter.
- [ ] fp16-shader-precision -- PAUSED, do last, highest artifact risk -- claude/workstreams/fp16-shader-precision.md
      mediump + spirv-opt route verified on this machine; two corruption hazards reproduced.
- [x] occlusion-query-stalls -- CLOSED, built then REVERTED (A/B showed no win) -- claude/workstreams/occlusion-query-stalls.md
      One open item survives the revert: the latent HOST_COHERENT query-buffer bug it had also fixed is
      back in tree. Listed under Upstream follow-ups.
- [ ] frame-hitches -- PAUSED, instrumented then deprioritised by its own measurement -- claude/workstreams/frame-hitches.md
      Hitches are 0.1-0.2% of frames. Not the problem. hostAllocUs instrumentation is worth keeping.

## Bug hunt findings (2026-07-25, /bugs on recent work; all majors verified by main agent; fixes landed same day)
- [x] VulkanAPI.h loader-table fix committed (b3922b6) -- b3922b6 did not compile without it; local clang build green
- [x] TCL wake handshake StoreLoad race FIXED (3637d61): seq_cst fence pair added; fast paths still lock-free; in-code comment now describes the fence-based guarantee
- [x] Index cache key now includes VGT_MULTI_PRIM_IB_RESET_INDX (a7e2adb); misleading cross-frame-persistence comments corrected (entries live one draw burst; invalidateAll fires per scanbuffer swap + CP starvation + retired cmdbuffer)
- [x] Timer regression FIXED (b3922b6): bottleneck.* timers moved to new LattePerfNestingTimer, legacy LattePerfStatTimer restored to forgiving pre-b3922b6 behavior (also moots the non-atomic depth concern); CSV now flushed per row; overlay GPU busy labeled "(retired)" with attribution caveat documented
- [x] Texture upload valve hysteresis (e2a6be9): trim check every 1024 releases, only if nothing in the window needed >64MiB
- [ ] Measure IdxCache hit/miss in-game to quantify within-burst reuse before deciding whether gating the invalidateAll flushes is worth pursuing
- [ ] STILL OPEN (deliberate): validation hash ~2KB sampling blind spots (LatteIndices.cpp) -- documented in-code, MUST be closed before ever unlocking cross-frame persistence; not fixed now because denser sampling costs per-hit CPU with zero benefit under the current flush regime

### Upstream follow-ups (not for this campaign)
- [ ] MetalMemoryManager.h/.cpp has the identical texture-upload-buffer zero-fill bug fixed in VKRMemoryManager.h (3a562a2) -- and its TextureUploadBufferRelease (~line 32) also clears unconditionally on every release, so the e2a6be9 hysteresis valve applies there too if upstreaming
- [ ] Occlusion query result buffer can be read stale. VulkanRenderer.cpp:837's FALLBACK allocation asks
  for DEVICE_LOCAL|HOST_VISIBLE|HOST_CACHED without HOST_COHERENT, and nothing calls
  vkInvalidateMappedMemoryRanges before reading ptrQueryResults (VulkanQuery.cpp:108). An under-counted
  occlusion result makes BOTW cull visible geometry -- silent and gameplay-visible. Inherited from
  upstream; NOT reproduced here (B390/ANV takes the first allocation, so the fallback never runs). Minimal
  fix: add HOST_COHERENT to the fallback flags, or invalidate before the read. Was fixed incidentally by
  the reverted occlusion-query change; now stands alone.

### Verified already-efficient (do not re-survey)
FetchShader lookup (O(1) L3 table + per-frame skip), shader-state selection (robin_hood on register state, no byte rehash per draw), SPIR-V disk cache + glslang optimizer enabled, buffer-cache interval tree, streamout setup, buffer-cache chronon rehash (PAC-MAN-only flag, inert for BOTW), LatteAsyncCommands_checkAndExecute (lock-free empty fast path, LatteAsyncCommands.cpp:124).
