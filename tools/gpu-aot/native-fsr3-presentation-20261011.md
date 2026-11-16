# Native FSR 3 presentation candidate — 2026-10-11

The native draw and swap hooks now submit real motion, a separate scene-depth
snapshot, reactive coverage and final color to the persistent FSR 3.1.4 upscaler.
The runnable copy enables Quality upscaling, with frame generation absent from
the native dispatch path. All work and testing stayed offline; no game was started.

## Delivery

- Folder: `../session-20261011-offline/native-fsr3-upscaling`
- Executable: `legodimensions.exe`, with runtime DLLs and the existing game/save/mod paths.
- Native fingerprint: `1ed0ba0fd8f9189a`.
- Executable SHA256: `cbb2a71fb31e374654e80875c6942df846073e1bc8b83922f46cd4eb418fd0f2`.
- Default: 1280x720 render → 1920x1080 output, sharpening enabled at 0.2.
- The earlier texture-use checkpoint and accepted Oz shader banks remain preserved.

Settings → Graphics contains FSR 3 enable, quality, sharpening and sharpness.
Enable and quality require restart; sharpening and strength update at dispatch.
The adjacent TOML exposes `native_fsr3`, `native_fsr3_quality`,
`native_fsr3_sharpen` and `native_fsr3_sharpness`.

| Quality value | Render extent | Output extent |
|---|---|---|
| native | 1920x1080 | 1920x1080 |
| quality | 1280x720 | 1920x1080 |
| balanced | 1129x635 | 1920x1080 |
| performance | 960x540 | 1920x1080 |
| ultra_performance | 640x360 | 1920x1080 |

## Integration

The auxiliary motion draw uses the actual submitted VB/IB ranges and immutable
constant snapshot. Its companion vertex program evaluates current and previous
constants; its pixel program retains the material's discard and depth outputs.
It writes RG16_FLOAT velocity and R8_UNORM reactive coverage, with depth/stencil
writes disabled. Auxiliary draws stay outside the game's occlusion queries and
restore the ordinary framebuffer and pipeline.

Known world profiles identify the object transform in c48..51. Geometry content,
declarations, bindings and that transform identify history; camera and wind
constants no longer invalidate every moving-camera frame. The previous program
still evaluates the entire previous constant image. Changed object matrices,
ambiguous instances, new geometry, vertex-textured programs, blended/stencilled
draws and unsupported programs reject history. Unknown overlays produce
conservative reactive coverage.

Camera discovery uses the individually inspected world profiles and captured
programs that explicitly read c0..3 and c48..51. Perspective decoding must still
succeed. This broader discovery allows unfamiliar captured prepasses to supply
the camera while their motion remains reactive. Jitter freezes at the first
scene draw and applies through viewport offsets to both depth and color passes.
A camera discovered only after an unjittered first scene draw causes fallback.
Missing/mismatched inputs, failed presents and projection changes invalidate
history; depth orientation changes replace and fence-retire the context.

Opaque fullscreen scene-image passes carry the existing motion field through
postprocessing. Replaying tone mapping as unknown geometry would otherwise
erase it. HUD/overlay coverage remains reactive. Game HUD currently participates
in upscaling; it is not recomposed separately at output resolution.

Presentation transitions the four inputs to compute SRV state and the output to
UAV state. After SDK dispatch it invalidates Plume's external heap and command
state caches before the normal final copy. Resources and replaced contexts are
retained through the recording fence. Shutdown releases the presenter only after
GPU idle, without reentering the host mutex.

The original material/prepass banks and accepted precise wind pair are unchanged.
A separate bank contains 891 companion variants from 286 captured programs.

## A failure caught before delivery

Independent shader compilation hid a real linkage failure. The original pixel
signature placed SV_IsFrontFace before the added clip interpolators, shifting
their register numbers relative to the vertex outputs. D3D12 rejected the LOTR
motion PSO. The generator now places rasterizer-generated inputs after those
interpolators. A compiled mutant that restores the old order reproduces driver
rejection (exit 2); the corrected production builder passes.

## Offline verification

- Release native executable builds; `git diff --check` passes.
- Portable/production-body CTest: **84/84**, final run 36.40 seconds.
- Production settings: default enabled, five modes, explicit off, environment
  overrides, restart snapshots and live sharpening bounds. Cvar storage is a fixture.
- Projection decoder: **558 saved full-scene camera matrices** pass, alongside
  synthetic translated/off-axis/reversed projection checks.
- Driver: **112 production motion PSOs**, seven saved world shader pairs, four
  specializations and four depth comparisons; cache reuse checked. Front-face
  register mutant fails as expected. Original-VS fallback resolution is a fixture.
- Motion GPU fixture: static, camera, object, wind, alpha-cutout and new-object
  cases; seven captured VS families; unchanged current depth/coverage. The precise
  wind prepass/material pair passes four combinations of 128 controlled affine
  transforms. Previous=current, removed discard and displaced-depth mutants fail.
- **Production presenter + production Plume + production FSR**, five modes with
  command caching off/on: 24 frames each, real final copy/readback, normal/reversed
  depth, reactive rejection, live sharpening, failed-present history invalidation
  and late-camera fallback. Maximum sampled constant-color copy error 0.001465;
  no nonfinite output. Host ownership is a fixture with real GPU fences.

Evidence is under `../session-20261011-offline/fsr3-native-inputs/`, including
`ctest-delivery.log`, `camera-delivery`, `pipeline-delivery-gpu`,
`motion-package-gpu` and `presentation-final-gpu`. Reproduction tools are
`build_temporal_shader_bank.py`, `test_temporal_pipeline_gpu.py`,
`test_temporal_camera_capture.py`, `test_temporal_motion_gpu.py` and
`run_temporal_present_gpu.ps1`; they require local SDK/compiler artifacts.

## Acceptance still required

These checks establish the built integration and its offline component behavior.
They do not establish gameplay FPS, full-scene material replay, HUD quality,
travel/cut behavior, or visual acceptance of LOTR's texture-use fix. The prior
short Ultra Performance moving test also needed more post-reset accumulation
to satisfy its unchanged centroid criterion. The longer moving backend fixture
passes, but it does not replace gameplay acceptance. No game launch, capture,
network access, frame generation or push was performed.
