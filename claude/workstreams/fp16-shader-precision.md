---
name: fp16-shader-precision
description: Emitting fp16 / RelaxedPrecision from the Latte shader decompiler to cut ALU and bandwidth on the Xe3 iGPU -- why mediump alone is a no-op on ANV, the spirv-opt route that works, and the reproduced bitcast and depth corruption hazards
status: paused
---

# fp16 / RelaxedPrecision shader emission

## Current state

**Scoped, with the key questions settled empirically rather than by argument, nothing written.**

Both of the obvious routes are wrong. The workable one is a third: emit `mediump` in the decompiler, then
run SPIRV-Tools' `--convert-relaxed-to-half` after `GlslangToSpv`. That was verified end to end on this
machine and produces real `OpTypeFloat 16`.

**Deliberately last in the queue.** It has the largest artifact surface of any lever in the campaign, two
of its three hazards were *reproduced* rather than predicted, and the emitter it touches is shared with
the OpenGL backend. It should only be attempted after [[display-output-cost]], [[fsr-upscale-filter]] and
[[vrs-fragment-shading-rate]], and only as an opt-in mode.

## Next actions

1. Do not start until the cheaper P-side levers have been measured.
2. When started: prereqs 1 and 2 in Findings BEFORE any emitter change -- without them the pass converts
   almost nothing and the spike would falsely read as "fp16 does not help here".
3. Settle the open question cheaply first: build with the `mediump`-only variant and check
   `INTEL_DEBUG=fs` output to confirm whether ANV lowers RelaxedPrecision ALU to fp16 on its own. If it
   does, the whole spirv-opt half is unnecessary.

## Decisions

2026-08-29: rejected route (a), `mediump` alone. Mesa's `spirv_to_nir` maps RelaxedPrecision onto mediump
hints and handles the decoration on *variables*; the instruction-result -> fp16 case is a long-standing
hole tracked at mesa#6346, whose driver consumers are turnip and v3dv. Recent mediump-IO work is
AMD/RadeonSI-side (Mesa 25.3). No evidence ANV/brw lowers RelaxedPrecision ALU to fp16. Shipping it alone
risks a no-op. NOTE: this rests on the issue tracker and release notes, not on reading Mesa source --
strong evidence, not proof.

2026-08-29: rejected route (b), explicit `float16_t` types. `LatteDecompilerEmitGLSL.cpp` is 163k;
hand-threading float16 through `_emitALUOperationBinary` (`:1017`), OP2 (`:1053`), OP3 (`:1642`),
reductions (`:1773`) plus every type-conversion site is a very large, very artifact-prone diff.

2026-08-29: adopted route (c) -- `mediump` emitter change plus `spirv-opt --convert-relaxed-to-half`.
Route (b)'s control at route (a)'s cost.

## Findings

### Shader pipeline map

```
R700 bytecode -> CF/clause parse   LatteDecompiler.cpp:1058 (_LatteDecompiler_Process)
                                   -> :1062 ParseCF, :1065 ParseClauses
analyze + type tracking            LatteDecompiler.cpp:1068, :1070
GLSL emit (GL *and* Vulkan)        LatteDecompiler.cpp:1074-1078 -> LatteDecompilerEmitGLSL.cpp:3943
Metal emit (separate emitter)      LatteDecompiler.cpp:1080-1084 -> LatteDecompilerEmitMSL.cpp:3957
per-stage entry points             LatteDecompiler.cpp:1103 VS / :1128 GS / :1159 PS
GLSL string -> renderer            LatteShader.cpp:359 -> :372 g_renderer->shader_create
glslang setup                      RendererShaderVk.cpp:303-331
  setEnvClient EShTargetVulkan_1_1   :307
  setEnvTarget  EShTargetSpv_1_3     :308
  preprocess (450, ENoProfile)       :314
  parse / link / mapIO               :324 / :336 / :344
GLSL -> SPIR-V                     RendererShaderVk.cpp:371 (GlslangToSpv), options :355-358
SPIR-V -> VkShaderModule           RendererShaderVk.cpp:229, called from :281 (cache hit) and :384
```

glslang options in full (`RendererShaderVk.cpp:355-358`):
```cpp
spvOptions.disableOptimizer = false;   // optimizer IS enabled
spvOptions.validate = false;
spvOptions.optimizeSize = true;        // equivalent to -Os
```

### "No precision qualifiers anywhere" -- CONFIRMED

`grep -rn -E "mediump|lowp|highp|RelaxedPrecision|float16|shader_16bit_storage|explicit_arithmetic_types" src/`
returns exactly 3 hits, all imgui GLES UI shaders (`imgui_impl_opengl3.cpp:514`, `:544`, `:565`). Zero in
`LegacyShaderDecompiler/`, zero in `Renderer/Vulkan/`, zero in `Renderer/OpenGL/`.

### The optimizer does NOT strip the decoration -- measured, not assumed

Compiled a representative fragment shader with `mediump` GPR locals using the installed
glslangValidator 11:16.4.0:
```
glslangValidator -V --target-env vulkan1.1       -> 9 RelaxedPrecision decorations
glslangValidator -V --target-env vulkan1.1 -Os   -> 6 RelaxedPrecision decorations
```
The 9 -> 6 drop is dead-code folding, not decoration removal: the survivors sit on the surviving ALU
(verified in `spirv-dis`, e.g. `%44 = OpFMul %v3float %41 %43` with `%41`/`%43`/`%44` all decorated). So
Cemu's current `disableOptimizer=false` + `optimizeSize=true` is not a blocker.

Also confirmed: glslang emits RelaxedPrecision from `mediump` in desktop `#version 450` GLSL targeting
Vulkan. No ES profile needed.

### Route (c) verified end to end

`spirv-opt --convert-relaxed-to-half` on the `-Os` output produced genuine fp16:
```
OpCapability Float16
%half = OpTypeFloat 16
%44 = OpFMul %v3half %41 %43
```
and correctly LEFT `OpImageSampleImplicitLod` at `%v4float` (the pass skips image/sample instructions by
design). API: `spvtools::CreateConvertRelaxedToHalfPass()`, declared
`/usr/include/spirv-tools/optimizer.hpp:723`; `libSPIRV-Tools-opt.so` is present and reaches the link line
transitively via glslang. `Float16` capability is fine under the existing
`EShTargetSpv_1_3` / Vulkan 1.1 target.

### TWO HARD PREREQUISITES

1. **`shaderFloat16` is not enabled today.** Grep for `shaderFloat16` / `Float16Int8` /
   `VK_KHR_shader_float16_int8` across `src/Cafe/HW/Latte/Renderer/Vulkan/` returns nothing. Add the
   extension to the list at `VulkanRenderer.cpp:48-51` and chain
   `VkPhysicalDeviceShaderFloat16Int8Features` into the pNext chain at `:679-699` (same shape as the
   existing `cacheControlFeature` block at `:682-689`). The device supports it -- vulkaninfo reports
   `shaderFloat16 = true` on this B390 / Mesa 26.2.1.
2. **Uniform reads block propagation, so `mediump` on GPRs alone buys little.** Cemu declares remapped
   uniforms as `ivec4` (`LatteDecompilerEmitGLSLHeader.hpp:28,30,32` remapped; `:43,45,47`
   uniform-register) and reads them via `intBitsToFloat` (`LatteDecompilerEmitGLSL.cpp:702-712` REMAPPED,
   `:717-730` FULL_CFILE). That read is highp, and GLSL computes an expression at the HIGHEST operand
   precision, so any ALU mixing a GPR with a uniform stays fp32. Measured: highp uniforms -> 1 of 3 ops
   converted; mediump uniforms -> 3 of 3. The emitter must land uniform reads into a mediump temp first.
   `FULL_CBANK` mode (`:747`) reads a real float array and is unaffected.

### Safe vs unsafe, with emit sites

SAFE to relax:
| what | file:line |
|---|---|
| pixel colour output (8-bit render targets) | `LatteDecompilerEmitGLSL.cpp:3319` `passPixelColor{}` |
| ALU float ops | `:1017` binary, `:1053` OP2, `:1642` OP3, `:1773` reduction |
| texture sample RESULT | `:2256` `_emitTEXSampleTextureCode` |

UNSAFE -- must stay fp32:
| what | file:line | why |
|---|---|---|
| texture COORDINATES | `:2164` `_emitTEXSampleCoordInputComponent` | fp16 has ~11 bits mantissa; breaks addressing past ~2048px and on atlas sub-rects |
| position / clip | `:3218-3240` SET_POSITION (macro `LatteDecompilerEmitGLSLHeader.hpp:291-295,325`) | VS-only |
| point size | `:3242-3252` | |
| depth output | `:3350` `gl_FragDepth` | |
| int / bit ops + bitcasts | `:989-1006` `_emitTypeConversionPrefix`, suffix `:1008-1014` | see hazard 1 |
| uniform loads | `:650` `_emitUniformAccessCode` | int-backed, see prereq 2 |

**Structural exclusion worth taking:** `typeTracker` picks int OR float GPRs globally
(`LatteDecompilerRegisterDataTypeTracker.cpp:10-19`; DTYPE constants `LatteConst.h:90-93`). A shader using
any integer op stores its floats as bit patterns in `ivec4`, so exclude those shaders from the mode
WHOLESALE rather than filtering per-instruction.

### Hazards -- the first two REPRODUCED, not speculated

1. **Bitcast round-trip corruption (highest).** Running `spirv-opt --relax-float-ops
   --convert-relaxed-to-half` over a shader replicating Cemu's int/float GPR pattern produced:
   ```
   %88 = OpFConvert %float %36     (%36 is half)
   %37 = OpBitcast %int %88
   ```
   The int-domain value is now the fp16-widened bit pattern, not the original fp32 bits. Silent,
   game-specific corruption anywhere `_emitTypeConversionPrefix` fires. This is why the aggressive
   "relax everything" variant cannot be used unguarded.
2. **Depth at fp16.** Same run produced `%67 = OpFMul %half %36 %96` feeding `OpStore %gl_FragDepth`.
   Nothing in the pass protects depth; expect z-fighting.
3. **Texture coordinate precision.** Not reproduced (needs in-game verification). BOTW UI and terrain
   atlases are the likely first visible failure.

### Cache versioning -- MUST invalidate the precompiled SPIR-V cache

It is keyed only by baseHash/auxHash plus a global magic, with no precision input:
`RendererShader.cpp:5-25` `GeneratePrecompiledCacheId()`, `:27-38`
`GenerateShaderPrecompiledCacheFilename()`, consumed at `RendererShaderVk.cpp:439-442` (open), `:276-284`
(read), `:377-382` (write). The precedent is already in that function -- `RendererShader.cpp:20` folds
`GetAccurateShaderMul()` into the id. Fold the precision toggle in the same way. Manual-invalidation
constant is `0x820a5277` at `RendererShader.cpp:22`.

NOT affected: the transferable shader cache (`LatteShaderCache.cpp:300-306`, stores GX2 bytecode) and the
Vulkan pipeline cache (`:308-313` + `VulkanPipelineStableCache.cpp:342-370`, stores shader hashes +
serialized register state, re-resolves by hash).

### No Vulkan-only path exists

`LatteDecompilerEmitGLSL.cpp` feeds BOTH OpenGL and Vulkan -- `LatteDecompiler.cpp:1074` dispatches on
`GetType() == RendererAPI::OpenGL || GetType() == RendererAPI::Vulkan` into the single emitter at `:1077`.
Only Metal has its own. So this must be gated INSIDE the shared emitter via a flag on
`LatteDecompilerOptions` (`LatteDecompiler.h:252-263`), set Vulkan-only in
`LatteShader_GetDecompilerOptions` (`LatteShader.cpp:793-805`), mirroring
`spirvInstrinsics.hasRoundingModeRTEFloat32` at `:799-802`.

### Separate spike found alongside: array-GPR zero-init

`LatteDecompilerEmitGLSL.cpp:3983-3996` declares `ivec4 Ri[128]` / `vec4 Rf[128]` unconditionally and
zero-inits all 128 entries. The non-array path at `:3969-3982` already consults `gprUseMask` and declares
only used registers (guard at `:3974`).

**The previously-recorded fix -- "size to actual gprUseMask" -- is WRONG and would cause out-of-bounds
writes.** The array path is entered only when `usesRelativeGPRWrite`
(`LatteDecompilerRegisterDataTypeTracker.cpp:21-23` is the only writer of `useArrayGPRs`), and
`gprUseMask` is documented at `LatteDecompilerInternal.h:237` as "1 bit per GPR, set if GPR is
read/written anywhere in the program (**ignores GPR accesses with relative index**)". So the mask is
incomplete precisely on the path that exists to serve relative indexing; sizing to `popcount(gprUseMask)`
would let a dynamic `Rf[idx]` write past the end. The analyzer only patches the mask conservatively for
relative READS (`LatteDecompilerAnalyzer.cpp:977-1013`), never for relative writes.

Correct bound: the shader's hardware GPR allocation, `SQ_PGM_RESOURCES.NUM_GPRS` -- declare `Rf[N]` and
run the init loop to N. Worth a separate spike; the 128 vec4 zero-stores per invocation are a real cost
either way, and unrelated to fp16.

Budget context: [[botw-frame-budget]]. Cheaper P-side levers first: [[fsr-upscale-filter]],
[[vrs-fragment-shading-rate]].
