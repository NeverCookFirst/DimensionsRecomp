# Oz regression correction, resource-consistent sampling, and FSR component tests

This supersedes the candidate and global precision policy described in
`oz-position-precision-fix-20261011.md`. No game was started, no desktop input
was sent, and no network was used during this continuation. All visual evidence
comes from an existing RenderDoc capture, not a new gameplay acceptance run.

## Leaves and black rocks

The previous precision patch changed shared prepass `E669A744921D81E3` without
changing all its color-pass partners. That introduced a depth disagreement on
rocks and static foliage. At saved-frame pixel (600,440), the changed prepass
writes 0.9607715606689453 while the material at event 32540 computes
0.9607716202735901. The material fails depth and leaves black pixels. Restoring
the original shared prepass makes both depths 0.9607716202735901 and restores
material RGB (0.0431373, 0.0392157, 0.0392157).

The corrected compiler policy enables precise position only for the two
replay-validated wind shaders: color `1E13090CD0FA6780` and prepass
`FFA4DD4B364A999D`. The shared prepass is restored byte-for-byte. The final
linked banks differ from the pre-precision executable in only four variants
(the two wind containers, variants 0 and 1). A fresh compiler reproduction
changes exactly the wind pair; four other tested containers remain identical.

The expanded saved-frame regression checks three opaque leaf pixels, a
transparent cutout, and the rock pixel, with baseline/candidate/restored-baseline
replay. It also requires the shared prepass to match the original capture.
The corrected candidate passes. The previous shared-prepass candidate fails
the new guard. This proves the selected failures in the saved frame; moving
camera/wind acceptance and every missing-leaf material remain unverified.

## Grass and texture sampling

The saved grass material at event 32064 binds sharp full-resolution base/detail
textures, but its base sampler clamps a 1024-pixel texture to mip 8 through 9.
The detail sampler is fixed at mip 3. This is evidence of lost sampling detail,
not broken decoded texture bytes. A private replay using full-range samplers
restores finer grass detail, but is a diagnostic override, not shipped code.

Production binding now obtains resource fetch metadata from the same cached
resource that supplies the uploaded texture and immutable SRV. Immediately
before sampler registration, it merges those resource fields with the live
sampler fields and the application's separate requested-mip arrays at device
offsets 12356 and 12382. This reproduces the original TU23 SetTexture merge:
explicit requested and streamed mip limits remain respected, as do wrap,
filtering, anisotropy, and LOD bias. Native-owned zero-fetch resources retain
their existing path. Guest fetch state is not overwritten.

Previously, sampler decoding independently trusted the device fetch shadow,
even when the selected resource had different size/mip metadata. The change
removes that inconsistency. It does not identify the later writer that corrupts
the fetch shadow, establish all resource-lifetime semantics, or prove that all
observed road/grass blur is fixed. The old capture lacks simultaneous requested
mip-array data, so the diagnostic full-range replay is not a complete replay of
the new production binding decision.

## Offline validation and candidate

- Native Release build succeeds; full renderer CTest suite: **78/78**, 38.60 s.
- Original TU23 sampler differential oracle: 100,000 randomized cases, plus
  stale-resource, requested-limit, streaming-limit and sampler-preservation cases.
- Production binding fixture verifies corrected mip limits and unchanged guest
  shadow; removing the reconciliation assignment fails the regression.
- Sampled-depth fixture verifies that the returned view carries its resource's
  exact metadata. Existing descriptor-generation mutation checks still fail as
  expected when the safe behavior is removed.
- Corrected saved-frame replay passes; old shared-prepass candidate is rejected.
- Both compressed shader banks were located byte-for-byte in the built executable.

Private evidence is under `../session-20261011-offline/moving-regression/`,
relative to this source checkout. Key files: `ctest-final.log`,
`pair-policy-verification.json`, `candidate-verification.json`,
`position_precise-scene.png`, and the replay JSON/error records.

Candidate: `../session-20261011-offline/native-candidate/legodimensions-resource-sampler-fix.exe`.
The same build is in `../game-build/legodimensions.exe`. Native fingerprint:
`2d4e893eef729101`. SHA-256:
`8cd4ab8eca898064a7a401d0d602b3bd4abf228081584354ac744e047a6e5f20`.
Previous candidates and original bank backups are preserved. This is not a
claim of live visual acceptance, and it is not an FSR-enabled game build.

## FSR 3: upscaling and frame generation

The windowless D3D12 fixture now feeds actual upscaler output into manual frame
generation, with distinct color/depth/motion resources, jitter, retained history,
explicit transitions, and GPU completion before resource release/readback.
Five modes pass on the RTX 4060: Native AA, Quality, Balanced, Performance, and
Ultra Performance, all at 1920x1080 output.

Static tests use eight frames per mode, no nonfinite output, and maximum RGB
error no greater than 0.00001353. A 16-frame moving-rectangle test checks the
generated intermediate position, not merely successful API calls. The expected
last rendered center is 828 pixels and the generated center is 826 pixels.
All five modes are within one pixel. Resetting frame-generation history every
frame makes Quality produce 828.039630 instead of 826 and fails with exit 12.

The SDK upscaler reports 3.1.4. The working, already-local signed frame-generation
provider reports 1.1.3; these are distinct version reports. Provider path:
`E:/Claude/LEGO Dimensions/rexglue-sdk/thirdparty/FidelityFX-SDK/PrebuiltSignedDLL/amd_fidelityfx_dx12.dll`.
The current game-build DLL returns NO_PROVIDER: its SDK configuration has
`FFX_API_ENABLE_FRAMEGEN_PROVIDER=OFF`. No shared SDK or game DLL was replaced.

Reproduce after loading `E:/devtools/env.ps1`:

```powershell
$env:LEGO_FSR_TEST_MOVING = '1'
./tools/gpu-aot/run_fsr3_offline.ps1 `
  -SdkRoot 'E:/Claude/LEGO Dimensions/rexglue-sdk' `
  -OutputDirectory '../session-20261011-offline/fsr3-moving-final' `
  -FrameGenerationProvider 'E:/Claude/LEGO Dimensions/rexglue-sdk/thirdparty/FidelityFX-SDK/PrebuiltSignedDLL/amd_fidelityfx_dx12.dll'
```

Final moving results and exact input hashes are in `fsr3-moving-final/verification.json`;
static results are in `fsr3-both-signed/verification.json`. Both directories are
under `../session-20261011-offline/`. D3D12 debug-layer support was unavailable,
so these are output/API checks, not a validation-layer-clean claim.

**Native integration remains unfinished.** `HostDevice::PresentTexture` still
copies the final game image. It has no connected real scene motion/depth,
projection jitter/history, HUD separation, or frame-generation swapchain pacing.
The synthetic fixture proves the local GPU/provider can execute both components;
it does not make the game ready for FSR testing. The old SDK path that substitutes
color for depth/motion and resets history is still disabled. No FSR4 provider
was established. The next offline work is scene-input extraction and validation,
followed by native temporal dispatch and presentation integration.
