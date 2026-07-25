# Cemu Perf Campaign -- Progress

## 2026-07-24
- Ran a 3-agent codebase survey (GPU/Vulkan path, CPU/JIT path, instrumentation + build) for the BOTW 4K60 campaign; findings and file refs captured in todo.md.
- Key finding: vertex/attribute/uniform-buffer uploads double-copy through a staging ring into m_bufferCache (allocated with property flags 0, no unified-memory awareness) -- redundant on the target iGPU. Index data and shader uniform vars already write host-visible memory directly.
- CPU side is structurally sound (flat base+R13 memory fast path, table-indexed JIT dispatch); risk areas are interpreter fallback during async-recompile warm-up, the global scheduler mutex, and the global timer spinlock.
- No GPU timestamp queries and no profiler integration exist in tree; per-stage draw timers exist but are compiled out (THasProfiling always false).
- Next: hardware triage (GPU-bound vs CPU-bound vs power-limited), then instrumentation, then targeted fixes per todo.md.

## 2026-07-25
- Ran round-2 5-agent survey (command flow/sync, Vulkan per-draw, texture pipeline, memory/index paths, shader decompiler+uniforms); ~25 new findings recorded in todo.md as tiered "round-2 candidates".
- Cross-cutting insight: texture/context-reg changes end draw sequences, so BOTW hits the slow first-draw path constantly; descriptor-hash, pipeline-hash, uniform-reupload, and FBO-rederive costs all share that trigger and could be fixed together via register dirty-flags.
- Top standalone wins identified: 8-entry index cache (~0% hit rate), texture-upload zero-fill memset, LatteThread busy-spin (power steal from iGPU), occlusion-query renderpass splits + WAIT_BIT.
- Config audit: user's settings.xml has VSync=2 (MAILBOX, safe) and AsyncCompile=true; VSync mode 3 (SYNC_AND_LIMIT) confirmed broken/hang-prone on Linux builds.
- Profiling env: perf/turbostat/hotspot NOT installed; intel_gpu_top present; perf_event_paranoid=2; existing bin/Cemu_release is -O3 no-debug-info but unstripped (flat profiles OK, no call graphs).
- Next: user picks fixes to implement (no builds/tests this session per user instruction); profiling capture recipe delivered in chat.

## 2026-07-25 (later): instrumentation implemented
- Built the bottleneck instrumentation suite (scaffold by main agent, call sites by 2 sonnet subagents, all diffs reviewed line-by-line by main agent):
  - LattePerformanceMonitor.h/.cpp: performanceMonitor.bottleneck struct (15 TSC timers, 18 per-frame counters, GPU-busy ns), nesting-safe LattePerfStatTimer, LATTE_PERF_SCOPE/COUNT/ADD macros, g_lattePerfStatsEnabled runtime toggle (overlay debug checkbox or CEMU_PERFSTATS_CSV env var), per-frame CSV writer.
  - LatteOverlay.cpp: "Bottleneck stats" panel in the debug overlay (GPU busy ms, draw/sequence-end/cache-miss counters, per-stage CPU us, wait buckets, upload KB, vsync lateness).
  - Latte core call sites: idle spin + guest fence timers (LatteCommandProcessor), seq-end reason counters, index cache hit/miss + bytes (LatteIndices), buffer sync / shader update / FBO update / texture bind+hash+upload timers, texture reload counters, vsync lateness (LatteTiming).
  - Vulkan call sites: draw first/fast counters, uniform/pipeline/descriptor/renderpass/submit timers, async-skip + ds/pipeline miss + forced-submit counters, GPU timestamp query pool (per-cmdbuffer TOP/BOTTOM pair, readback at fence retirement in ProcessFinishedCommandBuffers, timestampValidBits masked, timestampPeriod converted to ns).
- Overhead when toggle off: one branch per call site. When on: two TSC reads per scope (~10-20ns each) + 2 GPU timestamps per cmdbuffer -- negligible vs the costs being measured.
- NOT built locally (user instruction); compile verification relies on the push-triggered build-check workflow on branch patch.
- Task queue (task list): #3 spawn no-regret fix agents (index cache / texture zero-fill / spin-to-block) auto-follows the instrumentation push; #4 review+commit+push those fixes.
- Capture workflow for next session: enable Options > Overlay > Debug in a heavy scene, or run with CEMU_PERFSTATS_CSV=/tmp/botw.csv; compare frametime vs "GPU busy" first, then read the CPU/wait buckets.

## 2026-07-25 (later still): first optimizations landed + project tracking
- Landed the three "no-regret" Tier 1 fixes (each implemented by a sonnet agent, diff-reviewed line-by-line by main agent, committed individually):
  - 72d215d texture upload zero-fill (VKRMemoryManager.h, 64 MiB valve). Metal backend has the same bug -- noted as upstream follow-up in todo.md.
  - 4a10a44 command processor spin-to-block (TCL condvar wait bounded at 200us due to polled vsync; throttled fence-loop clock reads; producer yield fallback). Missed-wakeup race prevented by consumer flag-set + ring re-check under the same mutex the producer notifies under; 200us timeout is the backstop for the relaxed-load fast path.
  - f94bcd1 persistent cross-frame index cache (robin_hood map, 2048 cap, sampled validation hash per hit -- key finding: LatteIndices_invalidate only ever covered the immediate-mode scratch buffer, so the hash is the primary staleness defense, same model as the texture cache).
- GitHub tracking on the fork (issues were disabled by default -- enabled them): issue #1 = profiling instrumentation, issue #2 = optimization tracker (landed/in-progress/backlog). README now has an "About this fork" section (563f356).
- NOT built locally (user instruction): compile verification = the push-triggered "Build check" workflow on branch patch (takes ~36 min per run). CHECK THE LATEST RUN RESULT BEFORE BUILDING ON THIS WORK -- the instrumentation run and the fixes run may surface compile errors to fix (robin_hood template instantiation in LatteIndices.cpp flagged as the main risk by the implementing agent).
- Next session: (1) check CI result, fix any compile errors; (2) user plays a heavy BOTW scene with overlay debug or CSV -> read GPU busy vs frametime, SeqEnd counters, IdxCache hit/miss to pick the next target (likely the register dirty-flag scheme if first-draw costs dominate); (3) verify in-game that the three fixes hold (index cache: watch for geometry corruption -- would indicate validation-hash misses; spin-to-block: idleSpin bucket should collapse, core no longer pegged).

## 2026-07-25 (evening): local build gate + /bugs sweep + fix batch
- Switched to local builds (user request; CI queue was ~4 runs deep at ~35 min each). Existing build/ dir (clang release Ninja) reused; bin/Cemu_release now current.
- Found and fixed the compile break CI would have caught: fdbe9e9's vkCmdWriteTimestamp/vkGetQueryPoolResults were missing from the VKFUNC_DEVICE dynamic loader table (Cemu builds with VK_NO_PROTOTYPES). Fix committed as 91aebca.
- Ran /bugs as a 4-agent parallel hunt over the recent work (index cache, spin-to-block, instrumentation suite, zero-fill); main agent re-verified every major finding in code before accepting it. Full findings recorded in todo.md "Bug hunt findings" section.
- Key discoveries: (a) f94bcd1's index cache never actually persisted across frames -- pre-existing LatteCP_signalEnterWait + cleanupBuffers invalidateAll calls wipe it several times per frame, so it is a within-burst cache (the 2026-07-25 "later still" entry above and the commit message overstate it); (b) the spin-to-block wake handshake had a real TSO StoreLoad lost-wakeup race bounded by the 200us timeout; (c) the nesting-depth guard added to the shared timer class froze two legacy timers.
- Fix batch (3 parallel fix agents on disjoint files, diffs line-by-line reviewed by main agent, local build green, committed individually): d048d70 TCL seq_cst fence pair; f073d28 index cache key + honest comments (also adds VGT_MULTI_PRIM_IB_RESET_INDX to the key); 7c57aca timer type split + CSV flush-per-row + "GPU busy (retired)" labeling; d523041 upload-buffer valve hysteresis (trim check every 1024 releases).
- Deliberately NOT fixed: validation-hash sampling blind spots (documented in-code; only matters if the invalidateAll flushes are ever gated -- prereq noted in todo.md).
- DONE (same evening): pushed to patch through 7f8dc37 (5 code fixes + docs); CI "Build check" started on the tip and stale queued runs auto-cancelled. Compile gate is now the LOCAL clang build (cmake --build build, ~2 min incremental, green; bin/Cemu_release current) -- CI is a secondary check only.
- DONE (see next entry): user captured /tmp/botw.csv; analyzed. VERDICT: GPU-bound.

## 2026-07-26: first CSV capture analyzed -- heavy scenes are GPU-bound
(capture file written 2026-07-25, analyzed in the 2026-07-26 session)
- Capture: 7285 frames (~3 min gameplay, /tmp/botw.csv -- SINCE LOST to the 2026-07-26 reboot /tmp wipe). Aggregates preserved in claude/profiling.md section 2; analysis tool now permanent at claude/tools/analyze_perfstats.py.
- Structure: first ~30s and last ~10s locked at 16.7ms (60fps, GPU busy 6-9ms, gpuWait ~0); middle ~2 min heavy open-world scenes settle at 37-39ms (~26fps) with 3400-5000 draws/frame.
- Heavy-frame budget (n=2893 frames >33.3ms): ft 38.5ms = GPU busy 35.1ms (91% util) + 3.4ms gap. LatteThread: gpuWait 25.6ms, idleSpin 0.8ms, ALL tracked CPU stages only 6.9ms (largest: textureUpdate 2.2, textureUpload 2.2, fbo 1.7, bufferSync 1.4 -- all hidden under gpuWait anyway).
- CONCLUSION: the CPU-side register dirty-flag scheme is DEPRIORITIZED -- perfect CPU work would recover at most the 3.4ms gap (~9%). To reach 60fps GPU work must drop >2x (35ms -> <16.7ms). All effort shifts GPU-side.
- Caveat on "GPU busy": timestamps bracket whole cmdbuffers, so GPU-side WAIT_BIT stalls from occlusion queries (22/frame in heavy scenes) count as busy. Weak evidence it is not dominant (occQ=0 heavy frames: 33.5ms busy at 2989 draws vs occQ>0: 35.2ms at 3927 draws -- roughly draw-proportional). Cross-check planned via INTEL_MEASURE=render.
- Landed-fix health from the capture: index cache 75% hit rate (1533 hit / 521 miss per frame, was ~0% with 8 slots); asyncSkippedDraws 0 (shaders warm); pipelineMiss 0.2 / descSetMiss 0.9 per frame; 60fps segments show idleSpin ~14ms/frame = condvar headroom as designed. User still needs to visually confirm no geometry corruption.
- NEXT (in order):
  1. Resolution ladder (zero code): same heavy spot at 4K / 1440p / 720p graphic pack, ~30s CSV each. Splits GPU cost into pixel-scaled vs per-draw/fixed. 4K->1440p = 2.25x fewer pixels; if mostly pixel-bound, 1440p60 is in reach and 4K60 needs ~2x shader/bandwidth reduction.
  2. intel_gpu_top + turbostat during the heavy scene: confirm iGPU sustains max clocks; check package power split (CPU side should be light now post-spin-fix).
  3. Pick GPU-side work from those results, candidates: fp16/RelaxedPrecision decompiler spike (biggest lever, artifact-prone, opt-in), occlusion-query renderpass splits + WAIT_BIT (also feeds gpuWait + 5.6 forced submits/frame), shadow/AO graphic-pack reduction (config-only lever), INTEL_MEASURE cross-check of where GPU time goes (renderpass-level attribution).

## 2026-07-26 (cont.): 1440p capture -- GPU cost split measured, target reset
- Capture: /tmp/botw_2K.csv, 6680 frames, same play area at 1440p graphic pack (ALSO LOST to the /tmp wipe; aggregates in claude/profiling.md section 2). Heavy scenes: ~26fps (4K) -> ~41-49fps; still GPU-bound (busy 17.8-22.8ms of 20.6-24.7ms ft, idleSpin ~0.5ms, gpuWait 10-14ms).
- Like-for-like via draw-count buckets (3000-4500 draws/frame, thousands of frames per side): GPU busy 33.4ms @4K vs 18.9ms @1440p = 1.77x reduction for 2.25x fewer pixels. Solving the two-point model: 4K heavy-frame GPU time ~= 26ms pixel-scaled + ~7ms fixed (geometry/per-draw/query overhead). Caveats: cross-session content proxy (draw count), retired-attribution smearing, and 4K may have run at different GPU clocks (turbostat check pending) -- treat the 26/7 split as approximate.
- Extrapolation: 720p would be ~10ms busy (comfortable 60); 1440p60 needs busy <=~15.5ms = 20-25% GPU cut; NATIVE 4K60 needs ~2.2x total cut (even zeroing fixed cost leaves 26ms pixel work vs 15.5 budget = ~3x pixel-work cut) -- not reachable without upscaling.
- STRATEGY RESET: campaign target becomes "render 1440p at locked 60fps, output to the 4K panel (upscale filter)"; native 4K60 shelved unless fp16 + pass-level wins compound dramatically.
- 1440p fix-health: index cache 73% hit (2019/757), asyncSkippedDraws 0, pipelineMiss 0.2, descSetMiss 0.8 -- consistent with 4K capture.
- NEXT (in order):
  1. intel_gpu_top + turbostat in the heavy scene (both resolutions if easy): sustained clocks + package power split; a power-limited 4K run would revise the pixel/fixed split.
  2. INTEL_MEASURE=render (or VK_LAYER_MESA_overlay) heavy-scene run at 1440p: attribute the ~19ms across renderpasses (main/shadow/AO/post) -> pick the cheapest 20-25% win among shadow/AO pack reduction (config-only), occlusion-query renderpass-split + WAIT_BIT fix, fp16 spike.
  3. Verify in-game: no geometry corruption from the index cache (user visual check, still pending).
- Addendum (same night): user shared brendangregg.com/perf.html + the 2025 Doom GPU flame graphs post. Identified iaprof (Intel EU-stall GPU flame graphs) as a candidate for per-shader GPU attribution; probed the box -- xe driver, kernel 7.1.3, BTF vmlinux+xe all OK; Xe3/PTL support and Vulkan/ANV path unverified (tool documents Battlemage/Lunar-Lake + Level Zero). Logged as an experiment behind INTEL_MEASURE in todo.md. perf IS installed (contrary to 2026-07-24 note; paranoid=2 so sudo needed); turbostat still missing but packaged in extra.

## 2026-07-27: measurement knowledge consolidated into docs
No code changes this session -- documentation only, so a fresh session can act without re-deriving anything.
- Discovered both baseline CSVs were destroyed: machine rebooted 2026-07-26 14:49 and cleared /tmp. Only the derived aggregates survive, which is why they are now written out in full. New convention in todo.md: capture to ~/Projects/Cemu-perf/, never /tmp.
- NEW `claude/profiling.md` -- the durable profiling reference, three parts:
  1. How to capture with the fork's instrumentation (overlay vs CSV), capture hygiene, the interpretation order, and the full `gpuBusyUs` attribution caveat (whole-cmdbuffer bracketing, so query WAIT_BIT stalls read as busy).
  2. Every measured baseline number from both captures: frametime distributions, heavy-frame budget breakdown, the draw-bucket like-for-like table, the pixel/fixed cost derivation, and landed-fix health (index cache 73-75% hit, etc).
  3. Recipes with exact command lines for intel_gpu_top, turbostat, INTEL_MEASURE, iaprof (incl. the verified-prereq table and the Xe3/Vulkan unknowns), and perf (flame graph, off-CPU, perf stat).
- NEW `claude/tools/analyze_perfstats.py` -- permanent replacement for the session-local scratchpad script. Takes N CSVs; per-file summary plus a draws/frame-bucketed cross-capture table. Documents in-code why bucketing is required and why a linear fit of busy-vs-draws is invalid here (it yielded a negative slope at 1440p -- draw count tracks content, not cost).
- todo.md restructured: goal line changed to 1440p60-upscaled, measured verdict recorded as settled (CPU-side parked), capture hygiene + analysis tool added to Notes, measurement section rewritten as completed baselines + ordered next steps, optimization candidates got a GPU-first prioritization header ranking fp16 > occlusion-query fix > shadow/AO pack.
- Root CLAUDE.md: added a pointer to claude/profiling.md (kept phase-agnostic -- no status).
- Corrected stale facts found along the way: two progress entries were mislabeled 2026-07-25 (the analysis happened 2026-07-26); the overlay-panel note still described the index cache as "8-slot"; the 2026-07-24 note claiming perf was absent was wrong.
- NEXT (nothing blocked, all three are user-run captures):
  1. `sudo intel_gpu_top` during a heavy 1440p scene -- sustained clocks or throttled? (profiling.md 3.1). Install turbostat first for the power split (3.2).
  2. `INTEL_MEASURE=type=render,file=~/Projects/Cemu-perf/measure-1440p.txt ./bin/Cemu_release` -- per-renderpass attribution of the ~19ms (3.3). This is the decision-maker for the next code change.
  3. Re-capture a persistent 1440p CSV baseline (the A/B reference for any future fix).
  Then pick from the prioritized GPU levers in todo.md: shadow/AO pack (free), occlusion-query renderpass splits (39% of the 1440p budget is resolution-independent), fp16 spike (biggest, riskiest).

## 2026-08-28: upstream merge + first fork release (RESUME HERE)
No perf work this session -- upstream sync, CI repair, and release plumbing.
- Added the real `upstream` remote (cemu-project/Cemu). None existed -- only `origin` (the fork) -- which is why local `main` looked current at the 3005cb7 branch point. Fetch it before assessing upstream drift.
- Reviewed all 12 commits in `patch..upstream/main` (2026-07-18 -> 2026-08-21). NONE are performance changes; nothing touched the Latte core, Vulkan draw path, texture pipeline or shader decompiler, so no baseline or measured verdict is invalidated. The only one that matters here is 50b9e4b (fence errors raise UnrecoverableError instead of spamming the log) -- worth having because VK_ERROR_DEVICE_LOST is a live Xe3/ANV hazard. Rest: coreinit DynLoad callbacks, ExpHeap resize fix, PPC assembler, AArch64, CLI argstr, wayland-protocols in BUILD.md.
- Merged upstream into `patch` as 87c0099. Only VulkanRenderer.cpp overlapped and the hunks did not collide (our GPU-timestamp readback sits in the `fenceStatus == VK_SUCCESS` branch and the gpuWait timer wraps vkWaitForFences; upstream rewrote the error tails below each). Local clang build green.
- FOUND AND FIXED: the Build check had been RED since the instrumentation landed -- **build-windows only**; Linux x64/arm and macOS x86_64/arm64 all passed. Cause: `performanceMonitor_t` was an unnamed `typedef struct` and `bottleneck.addGpuBusyNs` put a member function inside it, which MSVC rejects (C7626). Fixed by naming the struct (301177c). NOTE: build-windows stops at the first error, so further MSVC-only issues may surface on the next run.
- Release infrastructure: the inherited `deploy_release.yml` CANNOT produce a release on a fork -- `determine_release_version.yml` bumps the newest tag from this repo's releases API and exits 1 when there are none, and its regex rejects a `-perfN` suffix; it also builds a Windows/macOS/ARM matrix we neither ship nor test. Added `.github/workflows/deploy_fork_release.yml` (161dcc7) as a SEPARATE file so upstream merges never conflict with it: `v*-perf*` tag trigger, Linux x64 only, asserts the binary reports the fork version, packages the bin/ zip + AppImage, opens a DRAFT release.
- `EMULATOR_VERSION_SUFFIX` is now a CMake cache var (e8d3f7c) so a fork build reports "Cemu 2.6-perf1" rather than being indistinguishable from upstream's "Cemu 2.6". Verified all three paths (suffix / no suffix / no version) compile to the right string; unversioned local builds still report the commit hash.
- Public-repo hygiene audit (these docs are public): no username, home paths, emails, employer, hostnames, IPs, tokens, game dumps or title keys anywhere in claude/ or the README. Only `~/Projects/Cemu-perf/` as a capture-path convention. Commit identity is the personal domain, not the work address.
- Tagged v2.6-perf1 at 161dcc7 and pushed. Release run 33107279206 SUCCEEDED (27m37s, all 16 steps green): the version-assert step confirms the binary really reports "Cemu 2.6-perf1", the AppImage built fine despite appimage.sh's pinned library sonames, and both artifacts uploaded -- `cemu-2.6-perf1-linux-x64.zip` (33.6 MB) and `Cemu-2.6-perf1-x86_64.AppImage` (62.9 MB). Cold vcpkg cache meant ~20 of those 27 minutes were spent building dependencies from source; later runs should be faster now the NuGet cache is warm.
- The release is still a DRAFT by design. Publish with:
  `gh release edit v2.6-perf1 --repo HritwikSinghal/Cemu --draft=false`
- Cancelled the redundant Build check on 161dcc7 (its build-windows failure was already diagnosed). The Build check on b53cb27 (run 33107834364) was left running unwatched, so **the C7626 fix in 301177c is NOT yet confirmed green on MSVC** -- check that run's build-windows job before trusting the Windows build.
- v2.6-perf1 was deliberately NOT retagged: it still points at 161dcc7, which predates the MSVC fix. The Linux artifacts are unaffected (byte-identical either way) -- this only changes which tree a downloader checks out. Retagging was gated on build-windows going green first, which never got verified this session.
- NEXT (perf work resumes, unchanged from 2026-07-27):
  1. `INTEL_MEASURE=type=render` at 1440p -- per-renderpass attribution of the ~19ms. The decision-maker for the next code change (profiling.md 3.3).
  2. `sudo intel_gpu_top` clock/power check (profiling.md 3.1). turbostat still NOT installed.
  3. Re-capture a persistent 1440p baseline to ~/Projects/Cemu-perf/ before any A/B claim.
  Release loose ends (none blocking perf work): (a) publish the v2.6-perf1 draft when ready; (b) read run 33107834364's build-windows result -- it aborts at the first error, so C7626 may have been masking more MSVC-only issues; (c) only once Windows is green, decide whether to retag v2.6-perf1 or let the fix ship as v2.6-perf2.
