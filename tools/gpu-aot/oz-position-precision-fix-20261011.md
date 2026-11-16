# Oz foliage: depth/color position disagreement

**Superseded candidate:** the shared-prepass change described below introduced
rock/static-foliage depth failures. It has been restored, and compiler precision
is now restricted to the validated wind pair. See
[the correction and offline evidence](oz-resource-sampler-and-fsr-20261011.md).
This document records the earlier investigation, not the current shader policy.

The owner's October 11 test still showed dark blinking leaves and a smeared
road. The running game was captured, profiled, and closed as requested. Its
existing shutdown watchdog hard-exited after the close request. Subsequent
work used saved captures and windowless tools; no game was launched.

## Reproduced cause

Offline RenderDoc 1.46 replay of `renderdoc-road/road_frame2473.rdc` reproduces
black leaf material pixels before lighting. At pixel (40,875), primitive 60:

| Pass | Event | Depth | Result |
| --- | ---: | ---: | --- |
| Foliage prepass | 820 | 0.9023383259773254 | Writes depth |
| Foliage material | 31775 | 0.902338445186615 | Fails depth; color stays black |

The difference is two float32 ULPs. The material pass is not discarding this
pixel for alpha. This is a concrete cause of a black leaf region in the saved
frame, independent of the unresolved fetch-state/lifetime observations.
Captured vertex DXIL matches the original bank bytes for `FFA4DD4B364A999D`
variant 0 at event 820 and `1E13090CD0FA6780` variant 0 at event 31775.
Wind/camera changes can change rounding and explain blinking, but a static
replay does not establish animation-wide acceptance.

DXC could reassociate/contract equivalent vertex position computations
differently in the depth and color shaders. Strict compilation of both passes
removes the selected failures. Changing only the material pass fixes two of
three tested opaque pixels and leaves the third broken.

## Fix

`prepare_position_precision.py` emits `out precise float4 oPos` for native
vertex shaders. HLSL precision propagates through position dependencies;
unrelated material calculations are not globally forced strict. Non-native
declarations retain their previous form. The normal compiler preparation now
applies this change. Shared constants remain ABI624.

The local candidate updates three foliage vertex shader containers and all six
specialization variants:

- `1E13090CD0FA6780`: material/color pass.
- `FFA4DD4B364A999D`: wind foliage prepass.
- `E669A744921D81E3`: related foliage prepass used in saved/current evidence.

The replacement utility validates masks and payload sizes, writes a new bank,
and round-trips every entry. Main bank: 9,290 entries, one replaced and 9,289
unchanged. Runtime bank: 30,080 entries, two replaced and 30,078 unchanged.
No pixel shader, texture decoder, sampler policy, depth comparison, bias, or
resource lifetime rule is altered by this fix. Full future shader rebuilds
apply precise position to all native vertex shaders through the preparation
script; this candidate is deliberately limited to the investigated family.

## Verification

`replay_oz_position_regression.py` tests the actual production-prelinked DXIL
with baseline, replacement, and restored-baseline replay. At (40,875),
(42,875), and (50,870), original shaders fail depth and leave black; the
candidate agrees exactly with prepass depth and writes visible leaf color.
Restoring the original shaders reproduces all three failures. Transparent
control (25,890) remains discarded in every case. Regression exit code: 0.

Both broad `-Gis` and narrower position-only experiments passed; only the
position-only implementation is packaged. Three preparation/cache tests pass.
Existing renderer suite: 76/76, 56.90 seconds. An initial invocation used the
obsolete `renderer-tests` directory referencing a deleted C: checkout; the
successful run used `renderer-tests-e` against this source.
The new `position_precision` CTest was subsequently registered and passed
separately (the suite now contains 77 tests).

Native Release build/link succeeded with the existing prebuilt SDK. Embedded
banks were independently checked: all six changed variants match the candidate
production-linker output. Build fingerprint: `215fb37a9c3a05f8`. Executable:
`../game-build/legodimensions.exe`, also preserved as
`../session-20261011-offline/native-candidate/legodimensions-foliage-position-fix.exe`.
SHA256: `8DC9914B7D02A441E920E36C253395C762F8472E3678236BB8E65612706506B8`.
The previous texture-access candidate remains available for rollback. Source
bank backups are under `oz-followup/bank-backup`. No SDK/Plume changes or push.

Private evidence and outputs are in
`../session-20261011-offline/oz-followup`: pixel histories, position regression,
baseline/replacement images, generated HLSL/DXIL, source-bank backups,
compiler build, and native build log. To reproduce the capture regression,
set `LEGO_OZ_REPLAY_DIRECTORY` to that directory and
`LEGO_OZ_REPLAY_CAPTURE` to the saved capture, then run qrenderdoc with
`--python` pointing at `replay_oz_position_regression.py`. It closes its helper
process and records an exit file; it never runs the game.

## Still open

The older replay frame has intact road bricks, unlike the owner's latest
screenshot. This does not establish a road fix. The prior fetch capture lacks
simultaneous setter/header evidence, and the latest plain launch did not
enable the optional setter history. Do not repair fetch words heuristically.
The prior borrowed LockRect fix is retained.

FSR 3 upscaling and frame generation are both requested but not enabled in
this candidate. The local synthetic upscaler test is not native integration.
The native path still needs valid per-frame scene depth, motion vectors,
jitter/history, HUD handling, and the frame-generation presentation path.
Existing SDK placeholders that reuse color as depth/motion must not be enabled
as a substitute. No FSR 4 provider was located in the offline dependency trees.

Next gameplay acceptance must check animated leaves, road bricks, world travel,
and ordinary frame times. No new gameplay or performance acceptance is claimed.
