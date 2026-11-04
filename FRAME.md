# MHGU Recomp frame map (in a hunt, 4K, RenderResolution 3840x2160, FXAA on, MSAA off)

From an F10 dump (shots/f1_pass_*) on 2026-10-26. Image names are the recomp's own texture
handles from that run; sizes scale with RenderResolution. Colour targets are RGBA8 (nvn 0x25, LDR,
not HDR); the main depth is D24S8 (nvn 0x35) at render size.

| pass | target | draws | what it is |
|---|---|---|---|
| 000 | depth 1024x3072 | 83 | shadow map, 3 cascades stacked (do not jitter) |
| 001 | 256x1 | 1 | small LUT |
| 002 | main depth only | 81 | depth pre-pass |
| 003 | colour + main depth | 43 | screen-space shadow mask (reads the shadow map) |
| 004 | scene colour A + main depth | 100 | opaque geometry + sky, reads the shadow mask |
| 005 | scene colour A + main depth | 2 | fog / atmosphere (reads depth) |
| 006 | scene colour B | 1 | copy A -> B |
| 007 | half colour + half depth | 1 | depth downsample / half-res effects |
| 008 | scene colour B | 1 | composite the half-res layer into B |
| 009-023 | 1080p..67p chain | 1 each | brightness downsample + bloom blur chain |
| 024-028 | 540p/1080p | 1 each | depth-based blur (DOF / light shafts; black in this shot) |
| 029 | colour C | 1 | tonemap / colour grade + bloom combine (B -> C) |
| 030 | colour D + main depth | 2 | **game FXAA** (fs code hash 0x8530ded68f4407df) + DOF combine |
| 031 | colour C | 1 | plain copy D -> C |
| 032 | colour D | 128 | brightness/gamma + HUD (2048x2048 UI atlas) |
| 033 | output | 1 | final copy to the presented image |

## What this means for DLSS
- **Jitter** only the scene passes on the main depth: 002, 003, 004, 005 (not shadows 000, not the
  fullscreen post passes).
- **DLSS input:** scene colour B after pass 008 plus the main depth. This is HUD-free and before
  bloom/tonemap, so post effects get the stable, anti-aliased image.
- **Alternative:** replace FXAA in pass 030 (after post, before HUD). Simpler, but bloom would then be
  built from the jittered image.
- With FXAA = Off the recomp itself skips the draw with fs hash 0x8530ded68f4407df (draw() checks
  g_config+0x20); DLSS mode should force that off.
- Upscaling (render < output) is harder than DLAA: the recomp scales every target by one factor, so
  the post chain would also run at the low resolution. Do DLAA first.
