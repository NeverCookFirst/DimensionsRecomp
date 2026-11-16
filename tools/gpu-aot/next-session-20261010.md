# Native renderer: next session after October 10 travel tests

## State and constraints

Source: `rexlego/out/pr28-validation/source`, branch `codex/alen-pr28-local-test`,
renderer commit e69c242 / fingerprint 499ed11f3d95f053. Target remains native
1920x1080, stable 30 FPS, DoF off, monitor 3. Owner launches test instances;
do not launch the game independently. Preserve dirty SDK, Plume and mod-manager
submodules. C++ native D3D12 work only; no SDK/Vulkan rebuild or push/merge.

Owner ended testing for today. PID17416 was closed using the game window's
Alt+F4. Process enumeration confirms it exited. Log records the known wedged
shutdown hard-exit after the close request; do not describe this as a clean
shutdown. Final hub screenshot saved before closing.

## Evidence from this run

- Benchmark folder: `native1080-check/benchmark-20261010-022631`.
  Slow operations enabled; GPU snapshots, draw tracing and detailed timing off.
- Private evidence: `transition-audit/blink-20261010/`: owner rock/ground images,
  final returned-hub PNG, closed-run game log and transition-failures.json.
- Owner reports initially bad foliage later becoming correct, but other surfaces
  blinking. Rock screenshot shows stretched bands; ground screenshot shows flat
  colour without expected detail. Still images do not establish the flicker rate.
- Owner says Simpsons adventure world looks perfect. Its story-level result has
  not been supplied. This is an owner observation, not a reference comparison.
- Owner repeatedly identifies travel out of dimensions as the trigger for
  stutter and poor texture rendering. Final main-hub still has coherent floor
  detail and shadows; it does not disprove transient or upper-hub corruption.
- Log has 65 accessible 4096-byte pool copies rejected at 02:38:21.840..847,
  all with overlaps_host_resolve=true, F667B000<-F667A000 down through
  F663B000<-F663A000, caller lr82BD2B44. There is no exact scene marker tying
  this burst to a particular world transition.
- Source confirms PoolCopyHook tests both SOURCE and DESTINATION with
  PoolCopyHasHostTextureSource. Its unsupported branch invokes the guest
  helper before CPU memmove and destination invalidation. Therefore the new
  overwrite invalidation cannot repair these rejected copies. This is a
  concrete control-flow gap, not proof of the cause of all visual defects.
- Logged texture-upload totals include 36.136,72.722,65.500ms. One pool copy
  takes12.380ms, of which cache_invalidation_ms=12.090. 75 slow entries in the
  final current log, below the128-entry process cap. Texture subphase timing
  is not yet available; do not assign the whole upload total to decoding.

## Priority 1: preserve resource content through travel

1. Instrument the rejected-copy decision with bounded matching-resource facts:
   source versus destination overlap, header versus backing allocation,
   guest address, current/cached header, physical ranges, host authority and
   depth generation. Audit whether old-world resources retain stale ownership.
2. Separate a destination overwrite from a GPU-authoritative source read.
   A CPU-authoritative source can supersede destination authority through the
   existing invalidation path. A genuinely GPU-authoritative source must retain
   its data through a native GPU copy/relocation or an explicitly supported
   substitute. Do not just remove the source guard or upload stale guest RAM.
3. Check header-only relocation independently of pixel-data copying. Preserve
   host storage across proven identical relocation; retire stale registry and
   framebuffer/descriptor references when the resource is replaced.
4. Build production-body regression fixtures using the observed backwards
   4KB pool-copy sequence and overlapping aliases: CPU->CPU, CPU->old GPU
   destination, GPU source, header-only copy, partial overlap, failure and
   already-recorded draw lifetime. Mutate back to current behavior to prove
   the relevant case fails. Preserve original helper bookkeeping.

## Priority 2: texture and material correctness

After travel handling is corrected, compare one fresh-load and one returned
scene using the same camera. Capture diffuse/normal buffers, exact material
draws, UV/tangent samples, current fetch and header, base and all mip levels.
Distinguish incorrect coordinates, block decoding, sampler LOD, alpha/depth
state and stale resource binding. Do not apply a global UV or mip-bias tweak
based on the screenshots. Retain level1 foliage and Adventure Time lava as
separate flicker repros. LOTR terrain/minimap, missing cartoon presentation,
particles and the static hub pad remain open until explicitly verified.

## Priority 3: stable 30 FPS

Profile cache invalidation scans/retirement, header replacement and texture
hash/decode/allocation/copy subphases around travel. Optimize only measured
work while retaining content validation and fence-safe resource versions.
Counter smoothing already averages real completed-present intervals; it does
not eliminate stalls and must not hide them or clamp the display to30.

## Next authorized test and acceptance

First finish offline fixes/build/regressions and stage a reproducible candidate.
Then ask owner for a short paired route: fresh hub -> level1 -> hub, followed
by one known bad adventure world -> hub. Repeat the route to detect cumulative
corruption. Simpsons is a useful owner-reported good control scene.

Use separate bounded visual diagnostics and clean performance runs. At least
600 complete, focused, non-loading frames per marked scene; report mean,
median/p95/p99 intervals, slow-frame counts and draw counts. Explicitly measure
the first30 seconds after return. Stable30 means approximately33.3ms/frame
with transition spikes resolved; no claim based on a single counter reading.
Compare images against emulated reference where available. Do not declare
all visuals fixed or the FPS target met from offline tests alone.
