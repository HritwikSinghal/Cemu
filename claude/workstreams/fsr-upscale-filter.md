---
name: fsr-upscale-filter
description: Add AMD FSR 1.0 (EASU/RCAS) as a Cemu upscale filter so BOTW can render below output resolution and still look sharp on the 4K panel -- present-path blueprint, sRGB colour-space trap, ~180 LOC first commit
status: paused
---

# FSR 1.0 upscale filter

## Current state

**Scoped in full, nothing written.** This is the biggest single lever in the campaign because it makes
render resolution independently reducible while the output stays 4K -- and the user is *already* being
upscaled (bicubic, 3200x1800 -> 3840x2160), so this is a change of degree, not of kind.

The load-bearing assumption -- that a graphic-pack resolution changes only the internal render target
while the window stays native -- was sent to a cold-start adversarial refuter and **CONFIRMED**, with one
sub-clause corrected (which config value selects the filter). See Findings.

Deliberately not started: it should be built only after the resolution ladder in [[botw-frame-budget]]
says which render resolution actually reaches 60fps. Building the upscaler before knowing the target
resolution risks building the wrong thing.

## Next actions

1. Wait on the resolution ladder ([[botw-frame-budget]]).
2. Build the **EASU-only** variant first (~180 LOC, no intermediate image). It is the cheapest thing that
   produces a measurable answer to "does render-below-output plus a good filter beat native?"
3. Capture stationary A/B: current 3200x1800 + bicubic, vs the ladder's chosen resolution + FSR.
4. Only then decide whether RCAS (the sharpening pass, +intermediate image, ~450-600 LOC total) is worth it.

## Decisions

2026-08-29: EASU-only as the first commit rather than full EASU+RCAS. RCAS requires an intermediate
image, its own renderpass/framebuffer/view, and teardown -- and drags in the descriptor-cache bug below.
EASU alone answers the question the campaign actually needs answered.

2026-08-29: write a self-contained single-file GLSL port of `FsrEasuF`/`FsrRcasF` rather than vendoring
AMD's headers. FidelityFX FSR 1.0 is MIT, but `ffx_a.h` + `ffx_fsr1.h` are HLSL/GLSL-macro-heavy and
assume a preprocessor-driven include model that does not fit Cemu's "one GLSL string per filter"
structure. Retain AMD's copyright header and MIT notice at the top of the string constant.

## Findings

### The present path, traced (all on LatteThread)

1. `LatteThread.cpp:78,100` -- `LatteRenderTarget_copyToBackbuffer(...)` per screen
2. `LatteRenderTarget.cpp:866` -- the function itself
3. `:874-876` -- source extent from `textureView->baseTexture->GetEffectiveSize(...)`
4. `:882` -> `:829` `LatteRenderTarget_getScreenImageArea`, which reads the WINDOW size at `:835`
   (`WindowSystem::GetWindowPhysSize`) and returns the letterboxed `imageX/Y/W/H`
5. `:890` -- `downscaling = (imageWidth <= effectiveWidth || imageHeight <= effectiveHeight)`
6. `:925` -- `scaling_filter = downscaling ? GetConfig().downscale_filter : GetConfig().upscale_filter`;
   branches at `:927` kLinear, `:936` kBicubic, `:945` kBicubicHermite, `:954` kNearestNeighbor
7. `:964` -- `g_renderer->DrawBackbufferQuad(...)`
8. `VulkanRenderer.cpp:3271` -- pipeline `:3289`, viewport `:3298-3305`, descriptor set `:3312`,
   `vkCmdBeginRenderPass` into the SWAPCHAIN framebuffer `:3293-3315`, UBO `:3329`,
   `vkCmdDraw(...,6,...)` `:3336`, end `:3338`
9. overlay/imgui draw after, same swapchain image, `loadOp = LOAD`
10. `VulkanRenderer.cpp:3179` `SwapBuffers` -> `:3079` -> `vkQueuePresentKHR` `:3142`

**The existing filters are real fragment shaders, not sampler modes.** `useLinearTexFilter` only picks
`VK_FILTER_LINEAR` vs `NEAREST` on the sampler (`LatteTextureViewVk.cpp:218-244`); bicubic and hermite
are GLSL (`RendererOuputShader.cpp:27-71`, `:124-181`).

### Render-vs-output decoupling: CONFIRMED by adversarial refuter

- Pack rule sets `overwriteInfo.hasResolutionOverwrite` + width/height: `LatteTexture.cpp:1274-1283`
- The **VkImage is physically allocated at the overwritten size**: `LatteTextureVk.cpp:18-27`
  (`effectiveBaseWidth = overwriteInfo.width` -> `imageInfo.extent.width`)
- **No clamp anywhere** -- grep for `maxImageDimension2D`, `std::min(...width`, `clamp` over
  `LatteTextureVk.cpp` and `LatteTexture.cpp` returns nothing; the parse site
  (`GraphicPack2.cpp:1018-1019`) applies no validation either
- The game really rasterizes at that size: viewport/scissor scaled by
  `currentEffectiveSize/currentRenderSize` (`LatteRenderTarget.cpp:1054-1057`, `:1071-1074`), inverse
  carried to shaders by `LatteMRT::GetCurrentFragCoordScale` (`:641-642`)
- Nothing in the graphic-pack system touches the window: grep for
  `WindowSystem|swapchain|SetWindowSize|m_actualExtent` over `src/Cafe/GraphicPack/` hits only an error
  dialog. Swapchain extent comes solely from `VulkanRenderer.cpp:3024` -> `SwapchainInfoVk.cpp:33,334`
- Only path that ignores the override is CPU readback, which bails with a log line
  (`LatteTextureReadback.cpp:79-82`)

**Sub-clause CORRECTED:** `upscale_filter` governs only when BOTH output dimensions strictly exceed the
source. Equality routes to `downscale_filter` (note the `<=` at `:890`). On this setup
(3200x1800 render, 3840x2160 window) `downscaling` is FALSE, so the upscale path is live -- but add FSR
to the **upscale** list only; a downscale FSR selection would be meaningless.

**Name collision to avoid:** `GraphicPack2.h:247` also has `m_output_settings.upscale_filter`, but that
is a `LatteTextureView::MagFilter` (linear vs nearest sampler, parsed from `upscaleMagFilter`) and selects
NO shader. Only `GetConfig().upscale_filter` picks among the shaders.

**PRECONDITION, currently satisfied:** `LatteRenderTarget.cpp:894-921` walks active graphic packs FIRST;
if any supplies an output/upscaling/downscaling shader, the entire config-filter block at `:923` is
SKIPPED. Grep for `[OutputShader]` across installed packs hits only
`MarioKart8/Enhancements/Debanding/rules.txt`. The BOTW Graphics pack has none, so the config path is
live -- **re-check this if pack versions change**, because a pack-supplied output shader would silently
disable FSR.

### Shader mechanism -- no build-system work needed

GLSL source strings -> glslang -> SPIR-V at runtime (`RendererShaderVk.cpp:303-371`). No `.spv` files, no
offline step; `CMakeLists.txt:165` only does `find_package(glslang)`. Adding a shader = adding a
`static const std::string` plus one `new RendererOutputShader(...)`. Shaders are built at
`LatteThread.cpp:123` (`RendererOutputShader::InitializeStatic`).

**The shared preamble already supplies everything EASU needs.** `RendererOuputShader.cpp:441-492` binds a
`sampler2D textureSrc` at binding 0 and a UBO at binding 1 carrying `textureSrcResolution`,
`nativeResolution`, `outputResolution`, `applySRGBEncoding`, `targetGamma`, `displayGamma`. Source and
destination extents are both already there -- **no new constants for EASU**. RCAS sharpness would be one
appended UBO float. (The `#ifdef VULKAN` at `:448` resolves true: glslang predefines `VULKAN 100` under
`EShMsgVulkanRules`, set at `RendererShaderVk.cpp:313`.)

### Edit sites, ordered

1. `CemuConfig.h:96` -- append `kFSR1Filter` **after** `kNearestNeighborFilter`. Append-only: the value
   is persisted as a raw int (`CemuConfig.cpp:142`, `:375`).
2. `RendererOuputShader.h:20-25,44-51,65-74` -- `kFSR` enum entry, `s_fsr_easu_shader{,_ud}` /
   `s_fsr_rcas_shader` statics, source-string declarations. Add `float fsrSharpness` to
   `OutputUniformVariables` (`:11-19`) **appended last** (std140-safe: current tail is three scalars at
   offsets 24/28/32).
3. `RendererOuputShader.cpp` -- the GLSL strings (near `:124`); fill `fsrSharpness` in
   `FillUniformBlockBuffer` (`:276-292`); instantiate in the Vulkan arm of `InitializeStatic`
   (`:533-551`); free in `ShutdownStatic` (`:555-565`). Leave the GL/Metal arms alone and gate the UI so
   FSR is Vulkan-only, or the GL path asserts at `LatteRenderTarget.cpp:964`.
4. `LatteRenderTarget.cpp:954` -- `else if (scaling_filter == kFSR1Filter)` selecting the EASU shader with
   `filter = kLinear`.
5. `Renderer.h:76` + `VulkanRenderer.h:272` + `VulkanRenderer.cpp:3271` -- the real work. EASU-only can
   reuse `DrawBackbufferQuad` as-is. Full EASU+RCAS needs two renderpasses: EASU into the intermediate,
   then RCAS into the swapchain framebuffer with the existing viewport/scissor/clear logic unchanged.
6. **Intermediate image (RCAS only)** -- allocate in `SwapchainInfoVk.h:69-90` / `SwapchainInfoVk.cpp:42-130`
   alongside the swapchain views and framebuffers, sized to `m_actualExtent`, format
   `VK_FORMAT_A2B10G10R10_UNORM_PACK32`, usage `COLOR_ATTACHMENT | SAMPLED`, via
   `VKRMemoryManager::imageMemoryAllocate` (`VKRMemoryManager.h:320`). Size it to the FULL extent, not the
   letterboxed image area, so it is recreated only with the swapchain and both passes share the viewport
   -- RCAS can then use `texelFetch(..., ivec2(gl_FragCoord.xy), 0)` for its 3x3 tap with no UV rescale.
   NOTE: a compute-shader FSR is not possible without changing swapchain creation -- swapchain images
   carry only `COLOR_ATTACHMENT | TRANSFER_DST`, no `STORAGE` (`SwapchainInfoVk.cpp:44`).
7. `GeneralSettings2.cpp:486` -- the `choices[]` array is **SHARED** by the upscale and downscale radio
   boxes (`:487`, `:494`). Split it into two arrays and add `_("FSR 1.0")` to the upscale one only.
   `:1924` needs no change. No other UI lists the filter names.

### Risks

1. **Colour space -- shapes the design.** The scanbuffer is created `VK_FORMAT_R8G8B8A8_SRGB` when the
   title uses sRGB (`VulkanRenderer.cpp:2637-2638`, flag set at `GX2_Misc.cpp:204`), so
   `texture(textureSrc, uv)` returns **linear**, and the preamble's `main()` applies `sRGBEncode` + gamma
   *after* (`RendererOuputShader.cpp:478-489`). FSR1 wants perceptual-space input for both passes.
   Recommended: have the EASU pass apply the existing encode chain to its own output so the intermediate
   holds display-space values and RCAS is correct; EASU then runs in linear, a mild quality deviation, not
   a break. The fully correct alternative -- sampling an aliased UNORM view for raw sRGB bytes -- is
   available (`VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT` is set on every non-depth texture,
   `LatteTextureVk.cpp:57-58`) but needs a new view path in `LatteTextureViewVk`. Per-tap encode inside
   EASU is NOT viable: 12 `pow()` per output pixel at 3200x1800.
2. **Latent bug to fix alongside (RCAS only).** `m_backbufferBlitDescriptorSetCache`
   (`VulkanRenderer.cpp:3376`, `:3424`) is keyed on the image-view pointer and freed ONLY in
   `~VulkanRenderer` (`:851`, `:863-879`) -- never on swapchain recreate. The RCAS descriptor set would
   point at the intermediate view, which dies with the swapchain on every window resize. Invalidate that
   cache entry in the recreate path (`VulkanRenderer.cpp:3043` -> `RecreateSwapchain`) or a resize gives a
   dangling `VkImageView`.
3. **FSR is not free.** The whole present path runs on LatteThread, the same thread servicing the polled
   60Hz virtual vsync. EASU is ~12 gathers per output pixel; at 5.8M pixels that is real GPU time eating
   into the saving from the lower render resolution. **Measure end-to-end** -- the crossover is exactly
   what decides whether this is worth it, and whether the two-pass version beats EASU-only.

### Incidental

`RendererOuputShader.h:69-70` declares `s_bicubic_shader_source_vk` and `s_hermite_shader_source_vk`,
which have no definition in the `.cpp` and are never referenced. Dead declarations, not a Vulkan-specific
path to follow.

Sub-native presets already exist in the pack without editing it: 320x180 / 640x360 / 960x540 at
`Graphics/rules.txt:103-120`; the render-target rule at `:625-632` scales by `($width/$gameWidth)`.

Budget context: [[botw-frame-budget]]. The other P-side lever: [[vrs-fragment-shading-rate]].
