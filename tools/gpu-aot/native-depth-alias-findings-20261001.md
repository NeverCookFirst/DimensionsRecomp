# Native missing-geometry fix: depth aliases

The missing TT logo was a depth failure, not a missing draw or incorrect alpha.
RenderDoc `oct01-native-logo-rdc1`, draw 514, showed correct clip positions and
gold/cyan pixel output, but depth 0.935637 failed against an incorrectly empty
depth buffer (0). Draw 530 contained the production text.

Two missing resource transfers caused this:

1. Depth texture header `ACEB028C` and raw RGBA8 header `ACEB0034` share allocation
   `FE4D7000` (physical `1E4D8000`). The native depth resolve populated an R32
   sampling mirror for the first header; the second header uploaded stale CPU
   bytes instead. Native now packs normalized depth into D24 bits shifted left
   by 8 when an otherwise matching RGBA8 view samples that allocation.
2. The game copies those raw bytes through PS `29E6F90BBBCE5539` into color
   surface `30172000`, EDRAM base 1328. Subsequent forward objects use depth
   surface `30210000` at the same base. Native now transfers matching color
   storage to depth with the Xenos 40-sample half-tile permutation.

The original shader already swaps 40 columns; the color-to-depth transfer must
perform the corresponding storage permutation too. Reference implementation:
`rexglue-sdk/src/graphics/d3d12/render_target_cache.cpp`, transfer column swap
around line 2741 and D24 packing around line 5344. No game shader hash is replaced,
and normal depth tests remain enabled.

The native path is enabled by default. `LEGO_NATIVE_DEPTH_ALIAS=0`, or probe
`-DisableDepthAlias`, provides an explicit comparison with the broken path.
The ordinary launch's renderer selection is separately controlled by
`gpu_native_pm4`: false = native, true = full SDK GPU. These are not live switches.

## Evidence

- Full build succeeds. `test_depth_alias.py` checks all 16,777,216 depth values
  for monotonicity and half-LSB error, all 921,600 image coordinates against
  EDRAM addressing, and executes the actual resource tracker with mocked GPU
  transfers to test write order, failed-copy retry, expiry and rejection cases.
- Native `oct01-native-depth-alias1`, RenderDoc draw 565: stored depth
  0.99890226, logo depth 0.9356377, depthTestFailed=false. Logo and production
  text are visible in the final swap-chain image.
- That same native run visibly restores translucent Vortech arms, body and
  legs in the observed minifigure shots, plus portal effects. Its title screen
  has the central portal, glowing spheres, orbit lines and complete characters.
- Evidence lives under `rexlego/out/native-gpu/session-20261001/`:
  `native-depth-alias1/`, `native-depth-alias-replay1/`, `depth-alias-tests/`.
  Original GPU captures are under `session-20260930/renderdoc-oct01-native-*`.

## Limits

The implemented EDRAM transfer covers equal-base, equal-extent single-sample
RGBA8/D24 surfaces with complete 80-pixel tiles. This is not a general EDRAM
overlap/MSAA/pitch conversion implementation. The packed sampling mirror carries
depth; stencil bytes are not propagated, and restore preserves destination stencil.
Float24 depth and arbitrary texture reinterpretations are not covered.

No gameplay-wide correctness or FPS claim is made. Giant brick-form closeups
were not independently captured. F3/F4 overlays still belong to the SDK GPU path;
native has no immediate drawer implementation. The separate bulk attachment hook
passes offline tests but was not called in the observed intro, and did not itself
fix the logo.
