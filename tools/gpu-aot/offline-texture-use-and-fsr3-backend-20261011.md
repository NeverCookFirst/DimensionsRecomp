# Use-time texture refresh and native FSR 3 backend checkpoint

Date: 2026-10-11. Offline only: no game launch, desktop control, network, frame
provider changes or frame-generation dispatch. This is an incomplete integration
checkpoint, not an FSR-enabled game release.

## Texture correction

`AdoptTexture` previously returned a borrowed cached resource even when its
placement header had changed since the last `SetTexture`/resolve. Drawing a
still-bound texture after an in-place streaming header rewrite therefore used
old storage, source layout and sampling metadata. `FindTextureAtUse` now runs
`RefreshTextureHeader` before adoption under the recording lock. Address-only
relocations discard the CPU page stamp and retain host resolve authority;
size/format/mip/type replacements retire old storage/descriptors at their fences.
Owned textures/surfaces never read a borrowed D3DTexture placement header.

The production-body test executes relocation, unchanged use, type/shape reuse,
owned/surface exclusions and retained old resources without SetTexture. Removing
the refresh fails with exit 23. This reproduces a cache bug, **not** attribution
of the current LOTR screenshot. No same-frame LOTR GPU capture is available and
no new gameplay visual acceptance is claimed. Existing sampler and Oz precision
fixes and shader banks remain intact.

## Native temporal inputs

The renderer root layout now has b4/space4 (root descriptor 3). The opt-in
`LEGO_NATIVE_TEMPORAL_INPUTS` path copies previous constants into the existing
immutable, fence-owned draw upload arena. It snapshots current VS/PS/shared
constants from the actual upload's cacheable CPU staging data; it does not read
write-combined upload storage or re-read guest constants later in the draw.

History keys include shader/declaration identity, draw/index ranges, actual
uploaded vertex/index content and conversion hashes, used buffer bindings,
noncamera constants, boolean/conversion state and render dimensions. Only the
three verified LOTR affine camera VS programs currently qualify (B881E7DFA89CF8F4,
AF42C59B97499AB6, 6CB6E1C4106F4571). Noncamera/geometry changes reject history;
unknown instance motion and vertex-texture animation are not guessed. Ambiguous
same-key transforms, missing previous frames, overflow, failures and shutdown
reset history. Static geometry camera motion can use its previous constants.
Moving instances still need verified persistent identity.

The same candidate path records an independent R32_FLOAT depth snapshot after
recognized scene color resolves and before depth clears. It uses the existing
native depth-resolve shader and retains replaced snapshots through submission
fences. Color/portrait/shadow size mismatches fail closed. Frame ownership and
allocation/resolve failure tests execute the production snapshot helper, with
fake allocation/copy/fence boundaries; actual scene correspondence and GPU depth
snapshot pixels have not been verified. These images are **not yet consumed** by
native FSR presentation. The candidate flag stays off by default.

## Production FSR backend

`fsr3_upscaler.cpp` now contains the reusable fixed-size D3D12 FSR 3.1.4 context,
SDK-sized dilated depth/motion/reconstructed-depth UAVs and real dispatch. It is
built/linked in the native executable. Context/history persists across frames;
first frame, camera cut, scene epoch, skipped frame and explicit invalidation
reset it. Rejecting an input also invalidates future history, but cannot permit
re-dispatch of a duplicate/out-of-order frame. Color/depth/motion/output aliasing,
format/extent errors, invalid camera/exposure/sharpness and incorrect jitter are
rejected before SDK recording. Output sharpening is per-dispatch adjustable.
The module has no frame-generation provider or dispatch path.

The windowless GPU fixture now invokes this **production module**, rather than
only a fixture-local SDK context. All five input sizes to 1920x1080 pass static
8-frame and moving 32-frame tests on RTX 4060. Static RGB error <=0.002282;
moving centroid errors <=0.814 output pixel, no nonfinite output. It checks input
rejections, duplicate rejection, five moving-history reset boundaries and 0.2
sharpening. GPU debug layer is unavailable, so this is not debug-layer validation.

The initial 16-frame Ultra Performance run failed the <=1-pixel centroid check
(1.098 px with sharpening, 1.096 without), two frames after explicit history
invalidation. Other modes passed. The longer sequence includes an accumulation
interval after reset and passes the original <=1-pixel bound in all modes. This
short-recovery quality limitation is not erased by the longer pass.

## Build and verification

Native Release build succeeds. Portable/production suite: **82/82 pass**, 37.13s.
Private evidence: `../session-20261011-offline/fsr3-native-inputs/`:
- `texture-use-refresh/verification.json`
- `scene-depth-snapshot/` production allocation/copy/fence fixture
- `production-dispatch-static/verification.json`
- `production-dispatch-moving/verification.json`
- `production-static.log`, `production-moving.log`
- `ctest-final-82.log`, `native-final-build.log`
- `native-texture-use-checkpoint.json`

Executable checkpoint: `../session-20261011-offline/native-candidate/legodimensions-texture-use-fix.exe`
Fingerprint: `8e2467ee233d84a8`.
SHA256: `62a30b1ec048261d468180323470c7296cd127d2a678c0853dfa59ebfab8f291`.
The accepted Oz resource/sampler executable remains separately preserved.

## Remaining work before an FSR-enabled executable

Submit the companion motion shaders with matched material alpha/depth, collect
velocity/reactive images for the actual scene, apply identical scene jitter to
prepass/color without jittering fullscreen/HUD work, validate depth/color phase
correspondence, and invoke production FSR from native presentation. Restore
Plume's state/descriptor cache after SDK recording and retire resized contexts at
completion fences. Then expose native quality/sharpening settings and package a
build with upscaling on by default. No UI toggle should claim that this
checkpoint already performs FSR presentation. LOTR visual acceptance stays open.
