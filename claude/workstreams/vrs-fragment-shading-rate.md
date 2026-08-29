---
name: vrs-fragment-shading-rate
description: Variable Rate Shading via VK_KHR_fragment_shading_rate in Cemu's Vulkan backend -- 2x2 coarse shading on the main 3D pass to cut fragment cost on Xe3, using dynamic state so the pipeline hash and shader disk cache are untouched
status: paused
---

# VRS (VK_KHR_fragment_shading_rate)

## Current state

**Scoped in full with file:line edit sites, nothing written.** The device supports all three rate types.
The recommended approach -- pipeline rate as *dynamic state* -- was sent to a cold-start adversarial
refuter, which **CONFIRMED every sub-claim** and additionally refuted the failure mode I had hypothesised.

Estimated saving 4-7ms at 3200x1800, but that is an estimate with a deliberately wide band and nothing
has been run in-game. It is not enough on its own to reach 60fps; it is one of a stack.

Paused behind [[display-output-cost]] and the resolution ladder in [[botw-frame-budget]], because VRS
attacks P (pixel-scaled cost) and the current evidence suggests F may be the bigger problem. If the
ladder shows F dominates, VRS will not save this and the effort should go elsewhere.

## Next actions

1. Wait on [[display-output-cost]] and the resolution ladder.
2. If P is confirmed as the dominant term, implement in the order in Findings. Off by default,
   **restart-required** (see Decisions).
3. Verify visually in BOTW before trusting any number: coarse derivatives shift implicit-LOD mip
   selection, and on a cel-shaded game with hard colour boundaries that reads as shimmer.

## Decisions

2026-08-29: **pipeline rate as DYNAMIC STATE**, not attachment rate and not the pipeline-create struct.
- Attachment rate needs a rate image per FBO, a compute pass to generate it, and `vkCreateRenderPass2`
  (all three of Cemu's renderpasses are v1). It would also change `VKRObjectRenderPass::AttachmentInfo_t`,
  the renderpass cache key, everywhere -- and would put the Xe3 attachment-view device-loss workaround
  back in scope. Rejected.
- The pipeline-create struct bakes the rate per pipeline, so it must enter
  `draw_calculateGraphicsPipelineHash` AND be reproducible in `VulkanPipelineStableCache::LoadPipelineFromCache`
  -- and it is not, because that path builds a placeholder renderpass with no real render target
  (`__CreateTemporaryRenderPass`, `VulkanPipelineStableCache.cpp:170-208`, every `viewObj = nullptr`) and
  never sees an FBO extent. A size-gated baked rate would hash differently on the live path than on the
  cache-load path. Rejected.
- Dynamic state changes exactly one enum in `dynamicStates`, applied uniformly from one boot-time flag.
  Hash unchanged, cache-load path consistent, no `cacheFileVersion` bump, users keep their shader cache.

2026-08-29: must be **restart-required**, not a live toggle. See risk 2.

## Findings

### Device capabilities, verbatim (B390 / Mesa 26.2.1-arch1.1, `VK_KHR_fragment_shading_rate` rev 2)

```
pipelineFragmentShadingRate   = true
primitiveFragmentShadingRate  = true
attachmentFragmentShadingRate = true

min/maxFragmentShadingRateAttachmentTexelSize = 8x8   (min == max: one legal texel size)
maxFragmentShadingRateAttachmentTexelSizeAspectRatio = 1
primitiveFragmentShadingRateWithMultipleViewports    = true
layeredShadingRateAttachments                        = true
fragmentShadingRateNonTrivialCombinerOps             = true
maxFragmentSize                                      = 4x4
maxFragmentSizeAspectRatio                           = 2
maxFragmentShadingRateCoverageSamples                = 16
maxFragmentShadingRateRasterizationSamples           = SAMPLE_COUNT_4_BIT
fragmentShadingRateWithShaderDepthStencilWrites      = FALSE   <-- correctness cliff, see risk 1
fragmentShadingRateWithSampleMask                    = true
fragmentShadingRateWithShaderSampleMask              = true
fragmentShadingRateWithConservativeRasterization     = true
fragmentShadingRateWithFragmentShaderInterlock       = true
fragmentShadingRateWithCustomSampleLocations         = true
fragmentShadingRateStrictMultiplyCombiner            = true
robustFragmentShadingRateAttachmentAccess            = true
fragmentShadingRateClampCombinerInputs               = true
fragmentShaderShadingRateInterlock                   = false
```

`vkGetPhysicalDeviceFragmentShadingRatesKHR` returns 7 rates (vulkaninfo does not print these; queried
with a small C program built against the repo's own `dependencies/Vulkan-Headers/include`):

```
4x4 samples=1 | 4x2 samples=1 | 2x4 samples=1|2 | 2x2 samples=1|2|4
2x1 samples=1|2|4 | 1x2 samples=1|2|4 | 1x1 samples=1|2|4|8|16|32|64
```

Cemu hardcodes `multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT`
(`VulkanPipelineCompiler.cpp:562`), so **all seven rates are usable**.

### Edit sites, ordered

1. `VulkanRenderer.h:468` -- `bool fragment_shading_rate = false;` in the `deviceExtensions` struct. The
   comment at `:450` states the contract: new optional extensions go in BOTH
   `CheckDeviceExtensionSupport` and `CreateDeviceCreateInfo`.
2. `VulkanRenderer.cpp:1405` -- set it from `isExtensionAvailable(VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME)`.
3. `VulkanRenderer.cpp:285-290` -- chain `VkPhysicalDeviceFragmentShadingRateFeaturesKHR` into
   `prevStruct` exactly as `attachment_feedback_loop_layout` does; gate on
   `pipelineFragmentShadingRate == VK_TRUE` at `:353`. Also chain
   `VkPhysicalDeviceFragmentShadingRatePropertiesKHR` into the `prop2` chain at `:316`.
4. `VulkanRenderer.cpp:1332` -- `used_extensions.emplace_back(...)`. Verify `VK_KHR_create_renderpass2`
   (a dependency) is in `kRequiredDeviceExtensions`; enable explicitly if not.
5. `VulkanRenderer.cpp:665-725` -- feature-enable chain (mirrors `:665` pipeline_robustness, `:692` custom
   border color, `:702` present_wait), `pipelineFragmentShadingRate = VK_TRUE`, other two false, into
   `deviceExtensionFeatures` consumed at `:744`.
6. `VulkanAPI.h:182` -- `VKFUNC_DEVICE(vkCmdSetFragmentShadingRateKHR);`. Cemu builds with
   `VK_NO_PROTOTYPES` and loads via `vkGetDeviceProcAddr`, so this is null when the extension is off --
   every call site must sit behind the feature flag. (A missing loader entry is exactly what broke the
   build once before, for the GPU timestamp functions.)
7. `VulkanPipelineCompiler.cpp:818-821` -- in `InitDynamicState`, append
   `VK_DYNAMIC_STATE_FRAGMENT_SHADING_RATE_KHR` when the boot-time flag is set. This is the ENTIRE
   pipeline-side change. Precedent right there: `VK_DYNAMIC_STATE_ATTACHMENT_FEEDBACK_LOOP_ENABLE_EXT`
   is added the same way, gated on a pure device-extension flag, and is not hashed.
8. `VulkanRendererCore.cpp:1476-1487` (`draw_execute_first`, right after `vkCmdBindPipeline`) and
   `:1681-1687` (`draw_execute_continued`) -- emit `vkCmdSetFragmentShadingRateKHR` when the desired rate
   differs from a new `m_state.currentFragmentShadingRate` tracker. Both combiners must be
   `VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR`.
9. `VulkanRenderer.h:403-416` -- reset the tracker in `resetCommandBufferState()`. Dynamic state is
   command-buffer scoped; without this the tracker goes stale across a command-buffer swap.
10. Policy inputs are both available at `VulkanRendererCore.cpp:1450-1455`: `m_state.activeFBO` is a
    `CachedFBOVk*` with `GetExtend()` (`CachedFBOVk.h:52`, already used this way by
    `IsAsyncPipelineAllowed` at `VulkanRendererCore.cpp:194-198`), and `pipeline_info->pixelShader` is a
    `LatteDecompilerShader*` carrying `depthMask` (`LatteDecompiler.h:176`, set at
    `LatteDecompilerAnalyzer.cpp:404-409`).
11. Setting: `CemuConfig.h:458` next to `vk_accurate_barriers`, load/store at `CemuConfig.cpp:146`/`:379`,
    checkbox at `GeneralSettings2.cpp:409` + `GeneralSettings2.h:75` + `:1272` + `:1919`, copying
    `m_async_compile` verbatim.

### Suggested default policy (off by default)

`1x1` unless ALL of: the pixel shader does not set `depthMask` (**mandatory**, see risk 1), `GetExtend()`
is >= 1280x720 in both axes, and the FBO has a depth buffer (`LatteCachedFBO::hasDepthBuffer()`,
`LatteCachedFBO.h:20`) -- the cheapest proxy for "main 3D pass, not HUD or post". Then `2x2`.
Never `4x4` or `4x2` by default.

### Adversarial refutation result: claim SURVIVED

Sub-claims confirmed with file:line:
- **Pipeline hash unaffected** -- `VulkanRendererCore.cpp:46-136`; grep of both hash functions for
  `dynamic|VK_DYNAMIC|usesDepthBias|usesBlendConstants` returns 2 hits, both comments, 0 code.
- **Renderpass cache key unaffected** -- `VulkanRenderer.cpp:4222-4237`, `m_hashForPipeline` sums colour
  attachment formats (`+ i * 31`) and the depth format, nothing else.
- **No disk cache version bump** -- `VulkanPipelineStableCache.cpp:342-370` serialises a version byte,
  present-mask byte, VS/GS/PS hashes and `Latte::SerializeRegisterState(...)`. No Vulkan-level field.
- **My hypothesised break does not exist** -- the disk-cache load path
  (`VulkanPipelineStableCache.cpp:282`) and the live draw path (`VulkanRendererCore.cpp:239`) call the
  SAME `InitFromCurrentGPUState`, which calls `InitDynamicState` unconditionally
  (`VulkanPipelineCompiler.cpp:887`, no enclosing branch). The cache-load VkPipeline is discarded
  immediately (`:290-300`) and never bound. Independently: enumerating
  `dependencies/Vulkan-Headers/registry/validusage.json` gives 18 VUIDs on
  `vkCmdSetFragmentShadingRateKHR`, **none** referencing a bound pipeline or dynamic state.
- **Blit / surface-copy / imgui excluded** -- only 4 `vkCreateGraphicsPipelines` sites exist; the three
  outside `PipelineCompiler` each build a two-entry literal `{VIEWPORT, SCISSOR}` dynamic state list
  (`VulkanRenderer.cpp:2903`->`:2971`, `VulkanSurfaceCopy.cpp:403`->`:438`,
  `imgui_impl_vulkan.cpp:798`->`:819`).
- **Graphic-pack shaders split correctly** -- output/upscaling shaders (the FXAA-style filters) go through
  `DrawBackbufferQuad` and are EXCLUDED; pack GAME-shader replacements go through `PipelineCompiler` and
  WOULD get the dynamic state, which matches intent since those are main-pass shaders.

**BINDING CONSTRAINT the refuter surfaced:** the hash claim holds only because the dynamic-state ENTRY is
run-global. The per-draw RATE may vary freely, but gating the *entry* per-pipeline would break it -- the
two existing conditional dynamic states DO compensate in the hash (`VulkanRendererCore.cpp:129-133`,
`if (polygonCtrl & (1 << 11)) stateHash += 0x1111;` for `usesDepthBias`; `:88-99` for blend constants).

If a baked per-pipeline size gate is ever wanted, the reproducible input is
`PA_SC_GENERIC_SCISSOR_TL`/`_BR` (`0xA090`/`0xA091`, `LatteReg.h:386-387`), which IS in the serialized
compacted register set (range `{0xA08E, 0x4}` at `RegisterSerializer.cpp:45`). Note that is GUEST
resolution, not host -- the upscale factor is not in register state.

### Risks

1. **`fragmentShadingRateWithShaderDepthStencilWrites = false` is a correctness cliff.** Any draw whose PS
   writes `gl_FragDepth` at rate > 1x1 is undefined on this device. Cemu emits it at
   `LatteDecompilerEmitGLSL.cpp:3350`; the flag is `LatteDecompilerShader::depthMask`. Useful trap at
   `LatteDecompilerAnalyzer.cpp:406-408`: the comment says the depth-buffer-mask check is Metal-only "as
   its not in the PS hash on other backends", so on Vulkan `depthMask` comes purely from the shader export
   and is NOT conditioned on an active depth buffer. That errs conservative, which is what you want --
   gate on it unconditionally and do not refine it.
2. **Mid-session toggling is invalid usage.** `vkCmdSetFragmentShadingRateKHR` against a pipeline that did
   not declare the dynamic state is invalid. `m_pipeline_info_cache` survives a settings change, and the
   stable-cache compiler threads (`VulkanPipelineStableCache.cpp:406`) build pipelines concurrently off
   the LatteThread. Read the setting ONCE into a renderer member at device creation; never read
   `GetConfig()` from `InitDynamicState`.
3. **Coarse derivatives break texture LOD and the post chain** -- the risk most likely to make the feature
   unusable rather than merely imperfect. Under 2x2, `dFdx`/`dFdy` span the coarse fragment, shifting
   implicit-LOD mip selection by a level. Graphics-pack FXAA/post shaders use `RendererShaderVk` with
   `isGfxPackShader` (`RendererShader.h:35`) and the same `PipelineCompiler`, so they WILL carry the
   dynamic state; they are full-screen and typically depth-less so the `hasDepthBuffer()` clause should
   exclude them -- **verify empirically, do not trust it**.
4. One-time cost: the driver's own `VkPipelineCache` blob (`shaderCache/driver/vk/{titleid}.bin`, loaded
   `VulkanRenderer.cpp:2481-2494`) is unversioned by Cemu and never invalidated, but changing
   `VkGraphicsPipelineCreateInfo` will likely miss on every pipeline the first run after VRS ships --
   recompile stutter once, not cache loss.

**NOT a risk:** the Xe3 device-loss workaround. `LatteTextureViewVk::GetAttachmentView`
(`LatteTextureViewVk.cpp:167-179`, commit `7385b18`) forces identity component mapping on attachment views
because a swizzled attachment hangs Xe2/Xe3 (cemu-project/Cemu#1856). Dynamic pipeline rate touches no
image views and adds no attachments. Attachment-rate VRS would.

### Estimate (estimate, not measurement)

At 2x2 the fragment shader runs once per 4 pixels, but only the invocation part of the pixel cost scales
-- per-pixel ROP/blend/depth-test and colour+depth bandwidth do not, and the bandwidth share is large on
an iGPU. Assuming 50-65% of P is FS invocation cost and the gate covers ~70% of it: roughly **6-10ms off
a 38.5ms frame at 4K**, scaling to **~4-7ms at 3200x1800**. Band is wide because whether ANV's Xe3
fragment dispatch actually coarsens as modelled is exactly what a capture would settle.

Budget context: [[botw-frame-budget]]. Other P-side levers: [[fsr-upscale-filter]], [[fp16-shader-precision]].
