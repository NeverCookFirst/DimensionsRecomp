# LOTR texture and material replay, 2026-10-12

Game remained closed. FSR remains off. No production rendering patch or visual
fix is established by these checks.

Capture directory:
`E:/Claude/LEGO Dimensions/LOTR Texture Capture/captures/20261012-204052-11c12b55`.
User reports the defects remained visible during capture.

## Verified evidence

- All 22 BC1/BC3 decoded DDS files match their raw guest base/mip allocations
  using the SDK tiled-address and packed-mip layout functions. The verifier now
  includes single-level lightmaps with no mip allocation (six 1784x952 atlases
  previously skipped). The one-byte DDS mutation negative control fails.
- Windowless D3D12 upload/readback preserves every compact compressed byte in
  all 22 textures and their mip chains. The test uses block-rounded Plume-style
  upload footprints, including the non-power-of-two lightmaps.
- Reconstructed captured VS/material inputs have no degenerate vertex basis
  vectors and no nonfinite material outputs in the CPU centroid probe. This is
  approximate float32/point/base-level analysis, not a hardware oracle.
- Windowless GPU replay of all 21 captured terrain draws uses the actual raw
  vertex attributes, captured floating constants, DDS mip chains, and terrain
  B881E7DFA89CF8F4 / 963F001993939275 shaders. No nonfinite color outputs.
- Extracted the actual specialization-zero shaders from the built linked bank
  under `game-build/generated/gpu_native/linked-bank`. All 21 draws produce
  byte-identical RT0, RT1 and depth readbacks versus freshly compiled shaders.
- Executed the production AVX2 vertex upload conversion on all 42 captured
  stream windows (stride 40 and 12). Every byte matches the independent expected
  float-DWORD swap / packed-byte preservation. This tests conversion, not cache
  selection or resource lifetime in the original running process.

Artifacts are in `session-20261012-offline/lotr-pixels/`: CPU inputs/generator,
`material.cpp` and `material.exe`, texture upload/readback fixture,
`mip-replay-all/verification.json`, `gpu-texture-verification.json`,
`material-gpu-verification.json`, `actual-bank-verification.json`, and previews.
No game executable was started to run these windowless tests.

## Scope that remains unverified

The material replay isolates these draws. It uses float render targets, a fresh
depth target and no scene stencil/blend sequence; it does not reproduce the
complete live render-target/resolve/deferred sequence or other shader families.
RT2 (normal/material encoding) is not read back in this two-color fixture.
CPU reconstruction expands geometry rather than exercising the live native
buffer-window cache. Texture captures span different frames and do not prove
the GPU-visible descriptor contents of the original failing frame.

The tests do not justify declaring textures, shaders or lifetimes universally
correct. They do establish that the selected captured terrain material can be
replayed without missing texture data, failed copies or nonfinite outputs.
A production patch must be tied to the remaining live-state difference before
shipping it as a LOTR fix. No speculative precision/mip/cache changes made.
