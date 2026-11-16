# Native FSR 3 stability follow-up, 2026-10-12

The owner reports blur, screen shaking and no FPS gain in yesterday's build,
plus persistent LOTR black materials and temporarily empty first visits.
These observations supersede any interpretation of the prior offline tests as
gameplay acceptance. No game, desktop capture or network access was used here.

## Implemented

- A jittered scene could fall back directly to the spatial presentation copy
  when temporal depth/camera validation failed. That copy now samples with the
  corresponding normalized jitter offset. Successful FSR reconstruction and
  unjittered frames use zero offset. The actual production presentation shader
  is exercised by the GPU fixture, including transitions to/from fallback.
- A scene-depth copy could be replaced by a later color resolve after the
  original depth surface was cleared. Copy publication now requires new scene
  geometry since the previous snapshot. Failed copies invalidate the frame.
- The independent R32 depth allocation persists across frames. Copies and FSR
  reads remain ordered on the same direct queue; a size change retires the old
  owner. Persisting storage does not preserve frame validity.
- Draws with no RT0 color writes no longer replay into motion/reactive buffers
  or upload previous constants. Saved LOTR draws include color-mask-zero
  prepasses. Their original depth/stencil work remains intact.
- Normal logs report 300-presentation windows with dispatch/reset/fallback and
  previous-draw-history counts. Bounded texture binding warnings identify
  unavailable resources, rejected uploads and lost views with shader identities.

## Offline evidence

Evidence directory: `../session-20261012-offline/`.

- Native Release build succeeds: `build-delivery.log`.
- 85/85 portable checks pass: `ctest-final.log`. Relevant checks are repeated
  after the final bounded texture logging change in `ctest-changed.log`.
- `test_temporal_depth_sequence.py --verify-regression` executes the production
  snapshot helper. Copy / clear / color-resolve preserves the saved depth;
  removing the dirty guard returns exit 23. Allocation/reuse/failure ownership
  checks execute the production depth capture body with fence mocks.
- The production presenter, FSR backend, Plume and final presentation shader run
  windowless on D3D12, with constant and analytic textured scenes. The textured
  scene samples a static reference at the actual jittered sample positions.
  Five modes and both command-state-cache settings cover temporal output,
  failed-present reset, reversed depth, reactive rejection and missing-depth
  spatial fallback. Host ownership is a real-fence-backed fixture.
- Detailed pixel results and negative controls are saved in
  `presentation-verified/verification.json`. Temporal cases allow .01 absolute
  RGB error; spatial-only fallback requires .004 (approximately one 8-bit code
  value) on the smooth reference. Reversing the input jitter and removing the
  final-copy compensation are separately compiled/executed negative controls.
  Earlier .01-only validation did not detect the omitted spatial offset
  (.008919 error), motivating the separate spatial accuracy criterion.

## Remaining limits

The jitter sign itself passes the patterned reference; it was not blindly
flipped. This fixes demonstrated fallback and depth-lifetime defects, but does
not establish that all owner-visible shaking/blur is resolved. The fixture is
not a full captured gameplay frame. HUD still participates in the scene image
instead of being composed separately at output resolution; unjittered overlays
can still have temporal artifacts. Mixed cameras, unknown material replay and
history rejection need actual draw/presentation correlation.

No FPS improvement is claimed from offline tests. Removing allocations and
colorless replays reduces specific work, but CPU limits, material replay cost
and GPU frame time remain to be measured. Quality stays 1280x720 -> 1920x1080,
sharpening .2, adjustable through the existing settings. Frame generation is off.

LOTR black textures and delayed first-visit geometry are NOT fixed by this
checkpoint. The saved LOTR draw/header audits are incomplete and uploads are
from different frames; there is no simultaneous affected LOTR RenderDoc frame
in the inspected evidence. Existing logs confirm FSR dispatch occurred, but
did not count fallback, history rejection or texture residency failures. New
bounded logging is intended to distinguish these on the next owner-run test.
Accepted Oz material banks, sampler merge, and precise wind pair are preserved.
