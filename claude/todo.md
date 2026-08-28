# Cemu -- Todo

## Notes & Conventions
- Fresh session: read the LATEST dated entry in claude/progress.md -- it ends with the current next actions. Repo-level durable facts are in the root CLAUDE.md. How to measure anything + all baseline numbers: claude/profiling.md.
- GOAL (revised 2026-07-26 after the resolution experiment): Zelda BOTW locked 60fps at **1440p render resolution, upscaled to the 4K panel**. Native 4K60 is measured out of reach (needs ~3x pixel-cost cut) -- see claude/profiling.md section 2.2. Working branch: `patch`.
- Fork context: 7385b18 fixed an ANV Xe3 device-loss (identity swizzle on framebuffer attachments) -- required just to boot BOTW on this GPU.
- Profiling build: `cmake -S . -B build-prof -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCEMU_CXX_FLAGS="-fno-omit-frame-pointer" -DCMAKE_INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO=OFF -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -G Ninja`
- All GX2->Vulkan emulation runs on ONE host thread ("LatteThread", src/Cafe/HW/Latte/Core/LatteThread.cpp:117). Overlay debug stats: VulkanRenderer::AppendOverlayDebugInfo (VulkanRenderer.cpp:3911).
- CAPTURE HYGIENE: never write CEMU_PERFSTATS_CSV to /tmp -- the 2026-07-26 reboot destroyed both baseline captures, so only the aggregates in claude/profiling.md survive. Use ~/Projects/Cemu-perf/ with resolution in the filename, and walk the SAME route in every capture of a comparison set.
- Analysis tool: `python3 claude/tools/analyze_perfstats.py <capture.csv> [more.csv ...]` -- per-file summary plus, for 2+ files, the draw-count-bucketed like-for-like table. Compare via that table, never via per-capture means (scene mix dominates).
- UPSTREAM SYNC: the `upstream` remote (cemu-project/Cemu) was added 2026-08-28; `origin` is the fork. Local `main` tracks the FORK's main, so it does NOT advance on its own -- always `git fetch upstream` and diff against `upstream/main`, never against local `main`.
- RELEASES: fork releases are Linux-x64-only, via `.github/workflows/deploy_fork_release.yml`, triggered by pushing a `vMAJOR.MINOR-perfN` tag; it builds, then PUBLISHES the release immediately -- there is no draft gate, so the tag push is the point of no return. What keeps a bad artifact from going public is that every guard (tag regex, the version assert against the built binary, zip packaging) fails the job before the publish step, which is last. The inherited `deploy_release.yml` + `determine_release_version.yml` pair CANNOT work on a fork (bumps the newest tag from the releases API, exits 1 when there are none, and rejects the -perfN suffix) -- do not try to use it. Fork builds must set EMULATOR_VERSION_SUFFIX or they report the same version string as upstream.
- HISTORY SHAPE: `git log upstream/main..patch` is the entire fork -- one logical commit per change, with all documentation churn in a single `docs:` commit. Keep it that way: squash doc-only commits before pushing instead of accumulating them. A rewrite invalidates every fork sha, so sha citations in these files must be remapped afterwards.
- CI SCOPE: build_check.yml builds Linux x64/arm + macOS x86_64/arm64 + Windows. Windows/MSVC is far stricter than the local clang build and has already caught a portability bug clang accepts (C7626 on an unnamed typedef struct with a member function, b3922b6). After touching a widely-included header, check the build-windows job specifically -- and note it stops at the FIRST error, so one green-looking fix may reveal more.
- MEASURED VERDICT (do not re-litigate without new data): heavy scenes are GPU-bound at both 4K and 1440p; all 15 CPU stage timers together are ~7ms/frame and hidden under gpuWait. CPU-side work (register dirty-flag scheme, first-draw costs) is therefore DEPRIORITIZED until the GPU side is broken.

## Perf campaign: measurement (baselines DONE -- full results in claude/profiling.md section 2)
- [x] 4K baseline (captured 2026-07-25, analyzed 2026-07-26): heavy scenes HARD GPU-BOUND -- 38.5ms frametime = 35.1ms GPU busy (91% util) + 3.4ms gap; gpuWait 25.6ms; all CPU stages 6.9ms.
- [x] Resolution experiment at 1440p: heavy scenes ~26fps -> ~41-49fps, still GPU-bound. Like-for-like 33.4ms -> 18.9ms GPU busy for 2.25x fewer pixels => ~26ms pixel-scaled + ~7.3ms resolution-independent at 4K. Drove the 1440p60 goal change.
- [ ] NEXT: power/clock check -- `sudo intel_gpu_top` during a heavy scene (recipe: profiling.md 3.1). Is the iGPU sustaining max clocks or frequency/power limited? A throttled 4K run would revise the 26/7.3 split. Needs `sudo pacman -S turbostat` (extra/turbostat 7.1.4-1) for the package power split (3.2).
- [ ] NEXT: per-renderpass GPU attribution -- `INTEL_MEASURE=type=render,file=...` at 1440p (recipe: profiling.md 3.3). Free, no install. Decides between shadow/AO pack reduction, the occlusion-query renderpass-split fix, and the fp16 spike. Also cross-checks whether query WAIT_BIT stalls inflate our gpuBusy number.
- [ ] Optional, time-boxed: iaprof per-shader GPU flame graphs (profiling.md 3.4). Box prereqs all verified OK; Xe3 support and the Vulkan/ANV path are the open unknowns. Only after INTEL_MEASURE.
- [ ] Re-capture a 1440p baseline to a persistent path before any A/B claim (the originals were lost to the /tmp wipe).
- [ ] Confirm FPS++ pack enabled and shadow/AO pack settings noted (shadow/AO resolution reduction is a candidate GPU-load lever, and at 1440p the resolution-independent cost is 39% of the budget)
- [ ] Vsync experiment: FIFO vs MAILBOX vs Immediate -- LOW PRIORITY, captures show a smooth GPU-bound equilibrium, not a FIFO halving pattern
- [ ] User visual check still pending: no geometry corruption from the index cache (a7e2adb)

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
- [ ] Optional flagship: Tracy integration (CPU zones + Vulkan GPU context) -- only if overlay/CSV prove insufficient. NOTE: low value while GPU-bound; Tracy would sharpen CPU attribution we already know is ~7ms/frame.
- How to read the overlay panel: "SeqEnd tex/ctxReg" = sequence-break causes (validates dirty-flag thesis); "IdxCache hit/miss" = index cache effectiveness (2048-entry map since a7e2adb; measured 73-75% hit in-game); "Waits gpu/idleSpin/guestFence" = stall buckets; "GPU busy" vs frametime = GPU-bound vs CPU-bound (attribution caveat: claude/profiling.md section 1).
- External tool recipes (INTEL_MEASURE, intel_gpu_top, turbostat, iaprof, perf) all live in claude/profiling.md section 3 -- do not re-research them.

## Perf campaign: optimization candidates (verify with profile first)
PRIORITIZATION AFTER THE 2026-07-26 BASELINES: the measurements say GPU-side first. Rank candidates by
whether they cut GPU work, not CPU work. Best-supported GPU levers right now:
  (a) fp16/RelaxedPrecision emission in the decompiler (Tier 3 GPU research spike below) -- attacks the
      ~26ms/11.6ms pixel-scaled cost directly, biggest lever, artifact-prone, opt-in only.
  (b) occlusion-query renderpass splits + WAIT_BIT (Tier 2 below) -- attacks the ~7.3ms
      resolution-independent cost, which is 39% of the budget at the new 1440p target.
  (c) shadow/AO graphic-pack resolution reduction -- config-only, zero code, try it first.
Everything under "first-draw"/"per-draw CPU" is CPU-side and parked (see Notes & Conventions verdict).
- [ ] UMA zero-copy buffer cache: vertex/attr uploads double-copy (memcpy -> staging ring -> vkCmdCopyBuffer -> m_bufferCache created with flags 0; VulkanRenderer.cpp:3834,3837). Direct host-visible write or the disabled host-import path (:3822). Needs hazard handling for in-flight reads.
- [ ] Texture hash cost: LatteTexture_CalculateTextureDataHash samples every live texture's guest memory every frame (LatteTextureCache.cpp:42)
- [ ] Per-draw texture binding: descriptor decode + linear bucket scan per unit per stage (LatteTextureLegacy.cpp:111, LatteTextureView.cpp:147)
- [ ] Barrier / renderpass-split count (vk_accurate_barriers read at VulkanRendererCore.cpp:1205; overlay Barriers/frame + BeginRP/frame)
- [ ] Async-compile warm-up: interpreter fallback when heavy scenes hit fresh code (PPCRecompiler.cpp:453 worker polls 10ms); test PPCREC_FORCE_SYNCHRONOUS_COMPILATION
- [ ] GX2DrawDone forces full sync unconditionally on Vulkan (GX2_Event.cpp:221) -- measure what honoring gx2drawdone_sync=false would save (correctness risk, experiment only)

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
- [ ] Texture upload double-copy: decoder writes CPU scratch, texture_loadSlice memcpys it again into staging (VulkanRenderer.cpp:3502-3520). Fix: decode directly into the staging allocation (UMA-friendly).
- [ ] Occlusion queries: renderpass split on begin AND end + vkCmdCopyQueryPoolResults with VK_QUERY_RESULT_WAIT_BIT (GPU-side stall) + RequestSubmitSoon/OnIdle per query (VulkanQuery.cpp:53-93,137-138). Fix: in-renderpass queries where legal + availability-bit polling (throttled poll infra exists, LatteQuery.cpp:120-142).
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
- [ ] GPU-side research spike: fp16/RelaxedPrecision emission -- zero precision qualifiers anywhere in the decompiler; big ALU+bandwidth win on iGPU but artifact-prone; opt-in mode only. Related: array-GPR shaders declare vec4[128] and zero-init all 128 regardless of use (LatteDecompilerEmitGLSL.cpp:3983-3997) -- size to actual gprUseMask.

### Spot-check additions (2026-07-25, main-agent read of draw path)
- [ ] First-draw re-syncs ALL buffers unconditionally: draw_execute_first passes 0xFFFFFFFF dirty masks to LatteBufferCache_Sync (VulkanRendererCore.cpp:1436), so every attribute buffer and every uniform block in the shader's quickBufferList pays an interval-tree lookup + rebind per first-draw even when untouched (LatteBufferData.cpp:188-295; in-code TODO :279 confirms the skip opportunity). The incremental path also flips ALL attrib buffers dirty whenever maxIndex/maxInstance grows (:199-213). Same register-dirty-flag scheme as the other first-draw items would fix it.
- [ ] Async-compile skipped draws still pay almost the full CPU path: pipeline validity is only checked at VulkanRendererCore.cpp:1443, AFTER streamout prep, 3x uniform upload, index decode+alloc+flush, buffer-cache sync, and pipeline hash/lookup. During compile-stutter windows Cemu burns near-full per-draw CPU on draws it then discards. Fix: hoist the pipeline lookup + VK_NULL_HANDLE check above the uniform/index/buffer work.

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

### Verified already-efficient (do not re-survey)
FetchShader lookup (O(1) L3 table + per-frame skip), shader-state selection (robin_hood on register state, no byte rehash per draw), SPIR-V disk cache + glslang optimizer enabled, buffer-cache interval tree, streamout setup, buffer-cache chronon rehash (PAC-MAN-only flag, inert for BOTW), LatteAsyncCommands_checkAndExecute (lock-free empty fast path, LatteAsyncCommands.cpp:124).
