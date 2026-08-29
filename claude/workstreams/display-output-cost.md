---
name: display-output-cost
description: Why BOTW is much smoother on the laptop panel than on the 4K external display at identical Cemu settings -- output-side costs (16-tap bicubic blit, compositing, 240Hz scanout bandwidth, blocked direct scanout) that are invisible to the emulator's own instrumentation
status: active
---

# Display output cost (laptop vs 4K external)

## Current state

**User-reported 2026-08-29: identical Cemu settings run much smoother on the laptop display than on the
4K external.** This is not in doubt as an observation and it has a strong mechanical explanation, but
NOTHING here is measured yet -- the whole workstream is currently four hypotheses with arithmetic behind
them and a set of free experiments to run.

Why it matters more than its size suggests: every one of these costs is **invisible to Cemu's own
instrumentation**. `gpuBusyUs` brackets Cemu's command buffers only. Compositor work and display scanout
contend for the same iGPU and the same LPDDR bandwidth but never appear in any CSV column, so the
campaign has been optimizing the ~28.5ms it CAN see while an unmeasured tax sits alongside it.

Three of the four candidate fixes are **zero code and reversible in one command**, which makes this the
cheapest available experiment in the whole campaign -- cheaper than [[fsr-upscale-filter]], and it should
be run before any more code is written.

## Next actions

1. **Drop DP-1 to 60Hz and re-capture, stationary.** The game is locked to 60fps by FPS++ and 240Hz buys
   it nothing. This is the single highest-value free test.
   ```bash
   hyprctl keyword monitor "DP-1,3840x2160@60,0x0,1"     # revert: @239.99
   ```
2. **Unblock direct scanout / solitary** so Hyprland hands the game's buffer to the display instead of
   compositing every frame. `directScanoutBlockedBy: user settings, missing candidate` and
   `solitaryBlockedBy: windowed mode` -- investigate both; "windowed mode" is suspicious given Cemu's
   config says `fullscreen=true` (it may be running as a fullscreen *window* rather than a true
   fullscreen surface).
3. **Test `UpscaleFilter` bicubic -> linear** at 4K output. See hypothesis 1: this is the effect most
   likely to be large AND it is the one that genuinely differs between the two displays rather than just
   scaling with pixel count.
4. Capture the laptop-panel case too (eDP-1 enabled, DP-1 off) as the reference point -- it is the
   configuration the user says is smooth, so it is the target to explain.
5. Feed whatever survives back into [[botw-frame-budget]]; if output-side cost is material, the P/F model
   needs a third term and the resolution ladder must be run at a FIXED display mode.

## Decisions

2026-08-29: run this before writing any more optimization code. Three of the four fixes are free and
none of the code levers are, and if the output-side tax is several ms it changes what the code levers
even need to achieve.

## Findings

### Measured display configuration (2026-08-29, `hyprctl monitors all`)

| | eDP-1 (laptop, SMOOTH) | DP-1 (external, SLOW) |
|---|---|---|
| model | LG Display 0x07C6 | Dell AW3225QF |
| mode | 2880x1800 @ 120.001Hz | **3840x2160 @ 239.991Hz** |
| scale | 1.3333334 | 1 |
| currentFormat | XRGB8888 | XRGB8888 |
| vrr | false | false |
| disabled | **true** (off when external is in use) | false |
| directScanoutBlockedBy | user settings, missing candidate | **user settings, missing candidate** |
| solitaryBlockedBy | invalid workspace | **windowed mode, missing candidate** |
| colorManagementPreset | srgb | srgb |

`bitdepth = 10` is configured in `~/.config/hypr/hyprland-gui.lua:55`, but `currentFormat` reports
XRGB8888, so the link appears to be running 8-bit. Worth confirming rather than assuming either way --
10-bit would add ~25% to both scanout and compositing bandwidth.

Pixel counts: eDP-1 5.18 Mpx, DP-1 8.29 Mpx (**1.60x**).

### Hypothesis 1 -- the upscale filter FLIPS between the two displays. Probably the biggest single effect.

`LatteRenderTarget.cpp:890`:
```
downscaling = (imageWidth <= effectiveWidth || imageHeight <= effectiveHeight)
```
With the game rendering at 3200x1800:
- **Laptop, 2880x1800 output:** `2880 <= 3200` is TRUE -> `downscaling` -> `GetConfig().downscale_filter`
  = 0 = **kLinearFilter**, a plain bilinear sampler tap.
- **4K, 3840x2160 output:** both comparisons false -> `GetConfig().upscale_filter` = 1 =
  **kBicubicFilter**, a GLSL bicubic (`RendererOuputShader.cpp:27-71`).

So the external display does not merely run the same blit over 1.6x more pixels -- it runs a
**fundamentally more expensive blit** (bicubic, many taps) over 1.6x more pixels, while the laptop runs
the cheapest possible one. That is a multiplicative difference in the present pass, and it is the effect
most specific to the user's observation rather than a generic "4K is bigger" argument.

Test: set `UpscaleFilter` to linear (0) and compare. If a large chunk of the gap closes, this is it, and
it also becomes a direct argument for [[fsr-upscale-filter]] -- FSR would replace bicubic with something
that costs more but delivers far more, at a lower render resolution.

### Hypothesis 2 -- 240Hz scanout bandwidth, continuously stolen from the iGPU

Scanout is a hardware DMA read of the whole framebuffer, every refresh, from the SAME LPDDR the iGPU
renders out of. It never stops and never appears in any Cemu counter.

```
DP-1  @ 4K 240Hz : 3840 * 2160 * 4 B * 240 = 7.96 GB/s
DP-1  @ 4K  60Hz : 3840 * 2160 * 4 B *  60 = 1.99 GB/s
eDP-1 @ 2880x1800 120Hz: 2880 * 1800 * 4 B * 120 = 2.49 GB/s
```

The external display at 240Hz costs **~5.5 GB/s more than the laptop panel** and ~6 GB/s more than it
would at 60Hz. On an iGPU whose entire performance story is shared-memory bandwidth, that is not noise.
**And the game is capped at 60fps by FPS++, so 240Hz buys literally nothing.**

This is a hypothesis about magnitude, not about mechanism -- the mechanism is certain, the share of the
28.5ms it accounts for is not. The 60Hz test settles it in one capture.

### Hypothesis 3 -- compositing instead of direct scanout

`directScanoutBlockedBy: user settings, missing candidate` on BOTH outputs, and
`solitaryBlockedBy: windowed mode` on DP-1. So Hyprland is compositing every frame rather than handing
the game's buffer straight to the display controller. That is an extra full-screen read+write pass at
output resolution, per frame, on the same iGPU -- 8.29 Mpx on the external versus 5.18 Mpx on the laptop.

`solitaryBlockedBy: windowed mode` is worth chasing specifically: Cemu's own config says
`fullscreen=true`, so either it presents as a fullscreen *window* rather than a true fullscreen surface,
or a Hyprland rule (blur, rounding, animation, an overlay layer) is disqualifying it. Whatever the cause,
fixing it removes a whole 4K pass per frame.

NOTE: the user's Hyprland config has its OWN tracker at `~/.config/hypr/claude/`, and records
`misc.vrr = 0` as a deliberate 2026-07-31 decision. Do not change VRR or any Hyprland setting on this
project's behalf without asking -- surface findings and let the user decide there.

### Hypothesis 4 -- output pixel count alone

The plain 1.60x. Real but the least interesting of the four, and the only one already implicitly covered
by the P/F model. Listed so it is not double-counted against the others.

### Why none of this shows up in the existing measurements

`gpuBusyUs` is a TOP/BOTTOM timestamp pair per Cemu command buffer, read back at fence retirement. It
measures only work Cemu submitted. Compositor passes and scanout DMA are other clients of the same GPU
and the same memory controller: they lengthen Cemu's work by contending for bandwidth, which inflates
`gpuBusyUs` **without ever being attributable to it**. Some of the ~28.5ms already measured in
[[botw-frame-budget]] may therefore be this, misattributed to Cemu's own shading -- which would mean the
derived `F` of ~10.4ms is partly an artifact of the display configuration rather than of the game.

That possibility is exactly why this runs before more code.
