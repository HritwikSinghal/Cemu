# Cemu Perf Campaign -- Progress

## Current state (2026-08-29)

GOAL: BOTW at a locked, stutter-free 60fps at the user's CURRENT settings -- 3200x1800 render, Draw
Distance pack at Ultra, output to the 3840x2160 panel. Working branch `patch`, tree clean, build green.

MEASURED, at those exact settings (n=8156 heavy frames >=4000 draws, medians): frametime 29.60ms
(33.8fps), gpuBusy 28.13ms = 95% of frametime, gpuWait 18.94ms, all twelve CPU stages together 5.57ms,
5521 draws/frame. GPU-bound; needs a ~1.77x GPU-work cut. Steady slowness, NOT stutter (hitches are
0.1-0.2% of frames). No time-based degradation -- the "smooth for a minute" effect is draw count.

ESTIMATED split at 1800p (3.15 ms/Mpx slope from the 4K-vs-1440p fit): pixel-scaled P ~18.1ms, fixed
F ~10.0ms. Two consequences drive everything: every CPU-side candidate has ceiling ~0 (5.57ms hidden
under 18.94ms of gpuWait), and FSR alone does not reach 60fps at Draw Distance Ultra because no
pixel-side lever touches F.

BLOCKING THE WHOLE CAMPAIGN: the measurement harness. The control test's noise floor is 8.6ms, so only
a >=10ms effect is currently resolvable. A stationary fixed-viewpoint workload is the prerequisite for
VRS and fp16, not optional hygiene.

NEXT: (1) [[display-output-cost]], free and may reframe the budget; (2) stationary workload + noise
floor; (3) [[botw-frame-budget]] resolution ladder; (4) then FSR. Detail per topic is in
claude/workstreams/; measurement recipes in claude/profiling.md; candidate list and ceilings in
claude/todo.md.

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

## 2026-08-29: measured at the real target settings; several planned wins REFUTED (RESUME HERE)
Per-topic detail now lives in `claude/workstreams/` -- this entry is the index, not the record.
Open only the workstream matching the task; the SessionStart hook lists one manifest line each.

GOAL RESTATED by the user: stutter-free locked 60fps at the CURRENT settings (3200x1800 render, Draw
Distance pack at Ultra, 3840x2160 output). The "increase draw distance further" goal is set aside.

WHAT THE MEASUREMENTS SAY (first captures ever taken at the real target settings; raw CSVs survive at
~/Projects/Cemu-perf/botw-1800p-{before,after}.csv):
- ~34-35fps, dead steady, 84-88% GPU-bound. 70-80% of frames miss 60fps but only 2.4% miss 30fps.
- **This is a steady-slowness problem, NOT a stutter problem.** Genuine hitches are 0.12-0.22% of frames,
  and the worst ones are guest-side CPU stalls (idleSpin, gpuBusy ~0.7ms on a 200ms frame), not renderer
  work. Gap to 60fps is a ~1.75x GPU-work cut.

THREE THINGS I PREDICTED OR CLAIMED THAT THE DATA REFUTED -- recorded so they are not re-proposed:
1. The occlusion-query WAIT_BIT removal was predicted to drop gpuBusyUs. It did not; like-for-like by
   draw bucket it is flat-to-slightly-worse. The mechanism argument (Vulkan spec, Mesa source, an in-tree
   precedent) was sound and still did not predict reality. The 7.3ms attribution of resolution-independent
   cost to query drains is UNSUPPORTED. Built green, then REVERTED -- the remaining case for it was
   correctness cleanup on a load-bearing sync path, which does not justify the risk at zero measured gain.
   Diff kept at ~/Projects/Cemu-perf/occlusion-query-waitbit-removal.patch. [[occlusion-query-stalls]]
2. I called mid-frame staging-ring growth "your stutter" off a microbenchmark of the allocation. It fires
   in 7 of 10568 frames (0.066%), six of them during warm-up. Severe when it fires, almost never fires
   during play. [[frame-hitches]]
3. I passed on a "2x faster" texture-staging benchmark with more weight than it deserved -- it used a
   synthetic decode stand-in and does not transfer to a textureUploadUs ratio. The verdict (do not build)
   stands but rests on ~2.2ms of CPU being hidden under 25.6ms of gpuWait, not on that benchmark.

METHODOLOGY FAILURE WORTH REMEMBERING: the A/B was two free-roam captures (7322 and 10568 steady frames)
of the same area. Different routes made them uncomparable -- draw-count bucketing controls content only
roughly, and a within-capture control test produced per-bucket deltas of +0.4, -3.2, +5.0, +4.1, +0.4,
-8.6, -0.1 ms. Free-roam A/B is now banned in todo.md: stand still, fixed viewpoint, one variable.

USER OBSERVATION THAT MAY OUTWEIGH EVERYTHING ELSE HERE: the same settings run much smoother on the
laptop panel than on the 4K external -- and every number above was captured on the external
(AW3225QF at 3840x2160 @ 240Hz). Four compounding output-side costs, three fixable in one command, none
of them visible to Cemu's own instrumentation. This may mean the derived F of ~10.4ms is partly display
configuration rather than game cost. RUN THIS FIRST. [[display-output-cost]]

ALSO BUILT, and COMMITTED as 8dacb00: `hostAllocUs`/`hostAllocs` instrumentation + an analyzer hitch
section, kept as permanent diagnostic capability even though they deprioritised their own workstream.
This is the only column that separates a hitch from steady slowness.

TWO TRACKER ENTRIES CORRECTED IN PLACE: the array-GPR "size to gprUseMask" fix would cause out-of-bounds
writes (gprUseMask ignores relative indexing, the only reason that code path exists) -- correct bound is
SQ_PGM_RESOURCES.NUM_GPRS; and "shadow/AO reduction is a free config win" is much weaker than it read.
The pack's entire "Shadow Draw Distance" category is inert in v7 -- nothing reads those variables.

ADVERSARIAL VERIFICATION: two cold-start refuters were run. The VRS dynamic-state claim survived intact
and the failure mode I hypothesised was itself refuted; the render-vs-output decoupling claim survived
with one sub-clause corrected. Both recorded in their workstreams with the binding constraints.

NEXT, in order:
  1. [[display-output-cost]] -- free, one command, and it may reframe the budget.
  2. [[botw-frame-budget]] -- resolution ladder, standing still, at a fixed display mode.
  3. Then pick a code lever with real numbers: [[fsr-upscale-filter]] (biggest), then
     [[vrs-fragment-shading-rate]], then [[fp16-shader-precision]] (last, highest artifact risk).
  4. SETTLED 2026-08-29: [[occlusion-query-stalls]] was reverted and its workstream closed. One item
     outlived it -- the latent HOST_COHERENT query-buffer bug it had incidentally fixed is back in tree
     and is now tracked on its own under todo.md "Upstream follow-ups".
  5. Still pending from 2026-07: user visual check for index-cache geometry corruption.

WORKING TREE AT SESSION END: clean. 8dacb00 committed the hostAlloc instrumentation; the occlusion-query
change was reverted; docs and the seven new claude/workstreams/ files committed alongside. Local clang
build green after both the commit and the revert.

## 2026-08-29 (later): "smooth for a minute" refuted; ceilings computed; Eden surveyed (RESUME HERE)
No code changes. Analysis only, against captures that already existed -- no new capture was needed.

- USER OBSERVATION REFUTED, with data: "BOTW is smooth 60fps regardless of resolution until ~1min in
  when more detail loads, then fps drops" is NOT time-based degradation. Binning the 288s capture by
  wall time shows 58fps at 462 draws/frame in the first 15s and 38fps at 4722 draws/frame in the next
  15s -- a ~10x step in DRAW COUNT, and fully reversible later in the same capture (1872 draws -> 50.6fps
  at t=270s; 99 draws -> locked 60fps at t=285s). A like-for-like drift test within fixed draw-count
  buckets, first third vs last third of the session, shows frametime +1.7%/+2.2% and textureHashUs
  +1.0%/+9.5% -- inside the residual draw-count difference. Nothing grows with session time. In
  particular the per-frame hash over every live texture does not become a problem as residency builds.
  Recorded as the THIRD MEASURED VERDICT in todo.md so it is not re-hunted.
- CEILINGS COMPUTED before proposing any code (n=8156 heavy frames, medians): frametime 29.60ms,
  gpuBusy 28.13ms (95%), gpuWait 18.94ms, all twelve CPU stages 5.57ms, 5521 draws/frame. Estimated
  split at 1800p: pixel-scaled P ~18.1ms, fixed F ~10.0ms. Consequences, both recorded in todo.md:
  (a) every CPU-side candidate has ceiling ~0 -- 5.57ms hidden inside 18.94ms of gpuWait;
  (b) **FSR alone does not reach 60fps at Draw Distance Ultra** -- 1440p render lands ~23ms (43fps),
  1080p render ~18ms (~55fps), because F is untouched by any pixel-side lever. F is bought by the 5521
  draws/frame the pack exists to produce. That tension is the campaign's real decision and belongs to
  the user, not to me.
- ORDERING INSIGHT: the 8.6ms noise floor from the earlier control test, not candidate size, sets what
  is buildable. Only a >=10ms effect is resolvable with the current harness. VRS (4-7ms) and fp16
  (2-4ms) are unmeasurable until the stationary fixed-viewpoint workload exists -- so that workload is
  the prerequisite for two of the three remaining levers, not optional hygiene.
- EDEN SURVEYED (~/Projects/eden @ 54cd5fb8eb, now INDEXED for claude-context). Their headline perf work
  is dynarmic / buffer-cache sync / shader cache -- none of it touches a GPU-bound iGPU frame. Three
  transferable items in todo.md "Reference codebase: Eden", of which one is declined by the ceiling gate
  (their ANV HOST_CACHED stream-buffer change, 978d9d935d -- right driver, wrong side of the wait).
- FP16 ROUTE REDIRECTED, and the redirect is VERIFIED not assumed. Eden uses real `float16_t` via
  `GL_EXT_shader_explicit_arithmetic_types`, not mediump/RelaxedPrecision. Test-compiled on this box:
  system glslang 16.4.0 accepts it and emits `OpCapability Float16` -- genuine fp16 ops, no spirv-opt
  post-pass needed. `vulkaninfo` confirms ANV/B390 exposes `shaderFloat16 = true` and
  VK_KHR_shader_float16_int8. BLOCKER FOUND: Cemu enables the feature NOWHERE -- grep for
  shaderFloat16/float16 across src/Cafe/HW/Latte/Renderer/Vulkan/ is empty, so the device feature must be
  requested at device creation before any such SPIR-V is legal. Small, well-defined prerequisite edit.

## 2026-08-28: upstream merge, linear fork history, v2.6-perf2
No perf work this session -- upstream sync, CI repair, release plumbing, history rewrite.
- Added the real `upstream` remote (cemu-project/Cemu). None existed -- only `origin` (the fork) -- which is why local `main` looked current at the 3005cb7 branch point. Fetch it before assessing upstream drift.
- Reviewed all 12 commits in `patch..upstream/main` (2026-07-18 -> 2026-08-21). NONE are performance changes; nothing touched the Latte core, Vulkan draw path, texture pipeline or shader decompiler, so no baseline or measured verdict is invalidated. The only one that matters here is 50b9e4b (fence errors raise UnrecoverableError instead of spamming the log) -- worth having because VK_ERROR_DEVICE_LOST is a live Xe3/ANV hazard. Rest: coreinit DynLoad callbacks, ExpHeap resize fix, PPC assembler, AArch64, CLI argstr, wayland-protocols in BUILD.md.
- Merged upstream into `patch`. Only VulkanRenderer.cpp overlapped and the hunks did not collide (our GPU-timestamp readback sits in the `fenceStatus == VK_SUCCESS` branch and the gpuWait timer wraps vkWaitForFences; upstream rewrote the error tails below each). Local clang build green.
- FOUND AND FIXED: the Build check had been RED since the instrumentation landed -- **build-windows only**; Linux x64/arm and macOS x86_64/arm64 all passed. Cause: `performanceMonitor_t` was an unnamed `typedef struct` and `bottleneck.addGpuBusyNs` put a member function inside it, which MSVC rejects (C7626). Fixed by naming the struct (b3922b6). All seven Build check jobs -- Linux x64/arm, macOS x86_64/arm64, Windows and both AppImage jobs -- pass on this source tree, so nothing further was hiding behind the first MSVC error.
- Release infrastructure: the inherited `deploy_release.yml` CANNOT produce a release on a fork -- `determine_release_version.yml` bumps the newest tag from this repo's releases API and exits 1 when there are none, and its regex rejects a `-perfN` suffix; it also builds a Windows/macOS/ARM matrix we neither ship nor test. Added `.github/workflows/deploy_fork_release.yml` (08a89cb) as a SEPARATE file so upstream merges never conflict with it: `v*-perf*` tag trigger, Linux x64 only, asserts the binary reports the fork version, packages the bin/ zip + AppImage, and publishes the release directly -- the tag push is the point of no return, which is why every artifact guard fails the job before the publish step rather than warning.
- `EMULATOR_VERSION_SUFFIX` is now a CMake cache var (49042fe) so a fork build reports "Cemu 2.6-perfN" rather than being indistinguishable from upstream's "Cemu 2.6". Verified all three paths (suffix / no suffix / no version) compile to the right string; unversioned local builds still report the commit hash.
- Public-repo hygiene audit (these docs are public): no username, home paths, emails, employer, hostnames, IPs, tokens, game dumps or title keys anywhere in claude/ or the README. Only `~/Projects/Cemu-perf/` as a capture-path convention. Commit identity is the personal domain, not the work address.
- HISTORY REWRITE: 26 fork commits plus the upstream merge became 10 logical commits replayed linearly onto 5ead580 -- one per change, with ten documentation commits collapsed into a single `docs:` commit. The merge commit is gone; `merge-base(patch, upstream/main)` is still 5ead580, so future syncs are unaffected. Built in an isolated worktree and verified BEFORE applying: identical tree hash (18ccd63) and a byte-identical `git diff 5ead580..` against the old branch.
- Rewrite technique worth reusing: recompose by staging file contents per logical group, not by hunk-splitting. Files touched by exactly one group take their final state; the three touched by two groups (LatteCommandProcessor.cpp, LatteIndices.cpp, VKRMemoryManager.h) take a real historical blob in the earlier commit, so no intermediate state is synthesized. Group order must preserve each shared file's chronological order. Confirmed mechanically that every instrumentation identifier used by the historical call sites still exists in the final header.
- Every fork sha cited in these notes was remapped to the new commits; the pre-rewrite shas are unreachable and were removed rather than annotated. `patch-prerewrite` is a local-only backup ref at the old tip.
- Released v2.6-perf2 (run 33159982801, 29m32s, all 14 steps green, published live rather than drafted): `cemu-2.6-perf2-linux-x64.zip` (32 MB) + `Cemu-2.6-perf2-x86_64.AppImage` (60 MB). The version-assert step confirms the binary reports "Cemu 2.6-perf2". A warm NuGet cache did NOT shorten the run much -- the vcpkg Configure step still dominated at ~25 of the 29 minutes, so budget ~30 min per release regardless.
- Compile coverage for what shipped: the release job built Linux x64 green, and the identical source tree had already passed all seven Build check jobs (incl. Windows/MSVC and macOS). The Build check triggered by this push was cancelled, so there is no green Build check run against e19754b itself -- the source tree, not the run, is the evidence.
- NEXT (perf work resumes, unchanged from 2026-07-27):
  1. `INTEL_MEASURE=type=render` at 1440p -- per-renderpass attribution of the ~19ms. The decision-maker for the next code change (profiling.md 3.3).
  2. `sudo intel_gpu_top` clock/power check (profiling.md 3.1). turbostat still NOT installed.
  3. Re-capture a persistent 1440p baseline to ~/Projects/Cemu-perf/ before any A/B claim.
  Only open release item: the AppImage self-update target still points at upstream's CI channel (see todo.md).
