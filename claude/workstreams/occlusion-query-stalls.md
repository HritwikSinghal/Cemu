---
name: occlusion-query-stalls
description: Occlusion query GPU stalls in the Vulkan backend -- the VK_QUERY_RESULT_WAIT_BIT copy removal that was REVERTED after its A/B showed no measurable win, why in-renderpass queries are illegal here, and the latent HOST_COHERENT query-buffer bug still in tree
status: closed
---

# Occlusion query stalls (WAIT_BIT removal)

## Current state

**REVERTED 2026-08-29. Implemented, built green, A/B showed no win, discarded by user decision.**

The change removed the per-query-fragment `vkCmdCopyQueryPoolResults` with `VK_QUERY_RESULT_WAIT_BIT`
and read results on the host instead, gated on the command-buffer fence the code already checked.
The mechanism argument was strong (Vulkan spec, Mesa source, an in-tree precedent) and the A/B still
came out flat-to-slightly-worse. See Findings for both the argument and its refutation.

It was defensible on correctness grounds alone, but it touches a load-bearing sync path and the user
chose not to carry that risk for no measured gain. The working tree is back to upstream behavior.

The diff is preserved at `~/Projects/Cemu-perf/occlusion-query-waitbit-removal.patch` (112 lines,
machine-local, outside the repo). Reapply with `git apply` if a stationary A/B is ever run.
`bin/Cemu_release_baseline` is now redundant -- after the revert, `bin/Cemu_release` IS the pre-change build.

## Next actions

1. NONE for the WAIT_BIT removal itself. Do not re-propose it without a stationary A/B that shows a win.
2. STILL OPEN, unrelated to the perf question -- the latent mapped-memory bug the revert put back.
   See "Latent bug still in tree" below. Small, self-contained, upstream-worthy.
3. IF a clean number is ever wanted: stationary A/B per the methodology in [[botw-frame-budget]] --
   fixed viewpoint, ~60s each, patched build vs stock. Free-roam captures cannot resolve an effect this size.
4. NOT started, low priority: the maximal variant (drop `RequestSubmitSoon`, host-side
   `vkResetQueryPool`). Details in Findings.

## Decisions

2026-08-29: REVERTED. The perf prediction was falsifiable, was tested, and failed; the remaining
justification was correctness cleanup on a load-bearing sync path, which does not clear the bar for a
change that buys no measured frametime. Patch preserved rather than deleted.

2026-08-29: rejected the previously-recorded fix ("in-renderpass queries where legal"). It is
**illegal** in this code path -- see Findings.

2026-08-29: deviated from the scout's proposed failure path. It suggested deferring an unready fragment
to the next poll; that would hang the emulator. `LatteQuery_UpdateFinishedQueriesForceFinishAll`
(`LatteQuery.cpp:112-117`) is an unbounded `while(true)` that spins until every fragment resolves, so a
fragment that never resolves is a hard Latte-thread hang. Falls back to a HOST-side wait instead, which
blocks only that thread and not the GPU command streamer -- keeping the point of the change while
guaranteeing forward progress.

2026-08-29: do not commit as a perf fix. The prediction was falsifiable, it was tested, and it failed.

## Findings

### What was changed, then reverted (preserved in the patch file above)

| file:line | change |
|---|---|
| `VulkanQuery.cpp:92` | deleted `vkCmdCopyQueryPoolResults(... VK_QUERY_RESULT_64_BIT \| VK_QUERY_RESULT_WAIT_BIT)` |
| `VulkanQuery.cpp:108` | host `vkGetQueryPoolResults` (no wait bit), fence-gated, with a `VK_NOT_READY` -> host-wait fallback |
| `VulkanQuery.cpp:50` | removed a dead commented-out `ptrQueryResults` read |
| `VulkanQuery.cpp:70` | added a comment recording WHY both renderpass splits are load-bearing |
| `VulkanRenderer.cpp:834` | removed the now-dead result buffer allocation + `vkMapMemory` |
| `VulkanRenderer.cpp:899` | removed its `DeleteBuffer` |
| `VulkanRenderer.h:720` | removed `bufferQueryResults` / `memoryQueryResults` / `ptrQueryResults` |

### Latent bug still in tree (the revert put it back)

The occlusion query result buffer is created with `HOST_VISIBLE | HOST_COHERENT | HOST_CACHED`, falling
back on failure to `DEVICE_LOCAL | HOST_VISIBLE | HOST_CACHED` **without HOST_COHERENT**
(`VulkanRenderer.cpp:837`), and nothing ever calls `vkInvalidateMappedMemoryRanges` before reading
`ptrQueryResults` (`VulkanQuery.cpp:108`). On a device that takes the fallback path the host can read a
stale occlusion count, and an under-count makes BOTW cull geometry that is actually visible -- silent and
gameplay-visible.

Inherited from upstream, not introduced here. Not reproduced on this box (B390/ANV takes the first
allocation, so the fallback never runs). The minimal fix is to add `HOST_COHERENT` to the fallback flags
or invalidate the range before the read -- independent of anything else in this workstream, and a
candidate to upstream on its own.

`vkGetQueryPoolResults` was already in the `VKFUNC_DEVICE` loader table (`VulkanAPI.h:205`) -- Cemu builds
with `VK_NO_PROTOTYPES`, so a missing entry there is a link error, not a runtime null.

### Why in-renderpass queries are ILLEGAL here (corrects the old backlog entry)

From this repo's own `dependencies/Vulkan-Headers/registry/vk.xml`:

```
vkCmdResetQueryPool        renderpass=outside
vkCmdCopyQueryPoolResults  renderpass=outside
vkCmdBeginQuery            renderpass=both
vkCmdEndQuery              renderpass=both
```

So the split at `VulkanQuery.cpp:58` exists for the **reset**, not for `vkCmdBeginQuery`. But the Vulkan
queries chapter also requires that a query "either begin and end inside the same subpass of a render pass
instance, or both begin and end outside of a render pass instance." Cemu's query windows deliberately
span whole renderpasses, and renderpasses end constantly for FBO changes and barriers. Moving begin
inside would force ending and re-fragmenting at **every** renderpass boundary -- strictly more resets,
copies and fragments. **Both `draw_endRenderPass()` calls (`:58`, `:86`) are load-bearing** and are now
commented as such so nobody "optimizes" them away.

Availability-bit polling is also unnecessary: the fence gate is strictly stronger than the availability
bit, and a `WITH_AVAILABILITY_BIT` copy would still have to be outside the renderpass.

### The mechanism argument (sound, and still did not predict reality)

Verified in the installed Mesa 26.1.4 source:
- `queryCount = 1` is below the copy-with-shader threshold of 6 (`anv_instance.c:40`), so ANV takes
  `copy_query_results_with_cs` (`genX_query.c:2045`).
- That path adds `ANV_PIPE_CS_STALL_BIT` unconditionally for `VK_QUERY_TYPE_OCCLUSION`
  (`genX_query.c:1741-1743`).
- `WAIT_BIT` additionally emits `MI_SEMAPHORE_WAIT` in PollingMode on the availability dword
  (`genX_query.c:1766-1773`), which is written by a post-sync PIPE_CONTROL from `vkCmdEndQuery` and lands
  only after all prior fragment work retires. The command streamer is blocked from feeding further work
  until the 3D pipeline fully drains.

In-tree precedent for relying on the fence instead, three lines from the equivalent code
(`VulkanRenderer.cpp:2207`): *"fence is signaled, so the timestamp results are guaranteed to be available
-> no wait bit needed"*. There is also a pre-existing upstream TODO at `VulkanQuery.cpp:72` asking the
same question: *"we already synchronize with command buffers, should we also set wait bits?"*

### THE MEASUREMENT THAT REFUTED THE EXPECTED WIN (2026-08-29)

Prediction made before capturing: `gpuBusyUs` drops with draw count unchanged, because the change removes
GPU stalls rather than real shader work.

Like-for-like by draw bucket (`botw-1800p-before.csv` vs `botw-1800p-after.csv`):

| draws/frame | before GPU busy | after GPU busy |
|---|---|---|
| 4000-4500 | 26.6ms | 26.1ms |
| 4500-5000 | 25.4ms | 27.6ms |
| 5000-5500 | 27.1ms | 27.9ms |
| 5500-6000 | 27.3ms | 28.5ms |
| 6000-6500 | 25.3ms | 29.0ms |
| 6500-7000 | 29.4ms | 29.0ms |

Flat to slightly worse. Occlusion query count was unchanged across the two (16.5 vs 17.1/frame), as
expected -- the change does not affect how many queries the game issues.

Cleaner within-capture test, immune to route differences -- does query COUNT predict GPU busy at equal
draw count? On the BEFORE capture, where the stall actually existed, per-bucket deltas were
`+0.4, -3.2, +5.0, +4.1, +0.4, -8.6, -0.1` ms. Noise with 8ms swings.

**Conclusion: the 7.3ms attribution of resolution-independent cost to query drains is NOT supported.**
The captures are also too noisy to distinguish "no effect" from "small regression", so no claim is made
in either direction. A stationary A/B could still settle it; the free-roam data cannot.

### The maximal variant, NOT implemented

- `VulkanQuery.cpp:140` -- drop `RequestSubmitSoon()`, keep `RequestSubmitOnIdle()`. `RequestSubmitSoon`
  lowers the submit threshold to `m_recordedDrawcalls + 10` (`VulkanRenderer.cpp:2371`), chopping the
  command buffer within 10 draws of every query end; each chop re-fragments every still-active query
  (`VulkanRenderer.cpp:2272`, `:2361`), adding a reset + endQuery + split pair. NOTE: the measured
  ~17 "occlusion queries" per frame are query FRAGMENTS, not GX2 queries -- `cntOcclusionQueries` is
  incremented in `beginFragment` (`VulkanQuery.cpp:56`), so submits and query count amplify each other.
  Risk: results reach the CPU later, lengthening the once-per-frame GX2DrawDone block. Watch
  `cntSubmits` / `cntSubmitsForced` / `tmrGpuWait` together; revert if gpuWait rises more than submits fall.
- Host-side `vkResetQueryPool` via `hostQueryReset` (ANV reports it, `anv_physical_device.c:543`): chain
  `VkPhysicalDeviceHostQueryResetFeatures` at `VulkanRenderer.cpp:642-745`, move the reset from
  `VulkanQuery.cpp:70` into `acquireQueryIndex`. Saves one PIPE_CONTROL per fragment. Does NOT remove the
  begin-side split -- begin must stay outside to match end.

### Correctness notes for whoever picks this up

- Result flow: `vkCmdEndQuery` -> host read in `handleFinishedFragments` (`VulkanQuery.cpp:106-108`, gated
  on `HasCommandBufferFinished`) -> `m_acccumulatedSum` -> `LatteQuery_UpdateFinishedQueries`
  (`LatteQuery.cpp:77`) -> summed into overlapping GX2 queries by event-id range (`:84-88`) ->
  `LatteQuery_finishGX2Query` writes guest memory (`:55-64`) -> guest `GX2QueryGetOcclusionResult`.
- Never drop a fragment. `m_acccumulatedSum` accumulates ACROSS fragments, so one lost sample corrupts the
  whole query and BOTW culls geometry that is actually visible -- silent and gameplay-visible.
- Event ordering is an invariant: `LatteQuery.cpp:80` asserts `latestQueryFinishedEventId <
  queryObject->queryEventEnd`, and attribution is by event-id range. Anything that lets queries resolve
  out of order breaks GX2 attribution.
- `acquireQueryIndex` hard-asserts on pool exhaustion (`VulkanQuery.cpp:116-120`, pool size 1024).
- Separate and unavoidable: an active occlusion query forces `ps_extra.PixelShaderHasUAV` on ANV
  (`genX_gfx_state.c:1129-1132`), i.e. forced fragment-shader execution for every draw in the query
  window. Resolution-dependent, and out of scope for this workstream.
- `GX2DrawDone` emits a full-sync packet UNCONDITIONALLY on Vulkan (`GX2_Event.cpp:221-229`), so there is
  at least one full GPU drain per frame regardless of anything done here.

Budget context and the measurement methodology live in [[botw-frame-budget]].
