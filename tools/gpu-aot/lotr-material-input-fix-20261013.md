# LOTR normalized material input correction

The owner accepted the vertex-fetch guard: the large black streaks were gone,
but black edges and white terrain blocks remained. Inspected the existing
`LOTR Vertex Guard Candidate` run, saved `lotr-snow-live-0.jpg`, closed it with
Alt+F4 and confirmed the process exited. No game restart, network, FSR or frame
generation was used. Evidence is in `../session-20261013-offline` relative to
the source checkout, including the accepted build's log.

## Attribution

Offline replay uses the complete frame `lotr_frame7101.rdc` from
`E:/Claude/LEGO Dimensions/LOTR GPU Frame Capture/captures/20261012-211831-4eb17971`.
Terrain VS `BBC50C6BCE460741` / PS `F6FB1D25A5E22FDE` at event12317 and
VS `BC47CC8592FDC286` / PS `D00F28537220E4E1` at event12623 supply the remaining
patches. Identity was checked against the embedded bank's DXIL program chunks.
Their vertex declarations bind `BLENDINDICES0` as `R8G8B8A8_UNORM`, but their
generated VS signatures declare `uint4`. These TT shaders use that semantic
for material colors/weights, rather than exclusively integer bone indices.

The pixel debugger sees COLOR0 around 1.04e9..1.06e9: the float register bits
become integers and then convert numerically back to floats. They damage
material blending and lighting. This is separate from the previously attributed
out-of-range vertex streams. Texture inputs contain the expected material and
lightmap images; fresh compilation of unchanged shader sources gives an
identical broken final image.

Changing only the two VS input declarations to float4 removes the white blocks
and black edges in the complete replay. Restoring the original shaders restores
the broken frame. The output now shows textured grass and rock at those points;
the apparent snow blocks were largely a rendering artifact.

## Production behavior

New build-time bank supplies float BLENDINDICES variants for 25 available
native shader families, both normal specialization values (50 variants).
`build_float_blend_indices_bank.py` changes only the input declaration type;
the translated math, fetches and explicit integer conversions remain intact.
It checks ABI624 source, identity, compilation and reflected float signature.
Private inputs/bytecode and their source hashes remain in the generated bank.

PSO creation selects this bank only when the current declaration supplies a
float/normalized BLENDINDICES0 format and an appropriate variant exists.
Integer declarations retain the original shader. Declaration content already
participates in PSO identity. Host variants remain owned until renderer reset;
their reset follows pipeline destruction and precedes host-device destruction.
Failed variant creation rejects the pass instead of falling back to the wrong
integer signature. No existing shader bank entries were replaced.

Temporal fallback also selects the corrected vertex variant. Float BLENDINDICES
draws use the reactive fallback rather than an uncorrected companion. FSR remains
disabled in this build; this work does not claim to repair FSR quality/performance.

The 25 families cover the prepared native source corpus, not every game shader.
Uncaptured families without a variant still follow their previous route. Valid
source geometry is not reconstructed by yesterday's range guard; the underlying
bad stream binding remains unattributed. No all-world visual acceptance claimed.

## Validation

- Windowless D3D12 fixture calls the production declaration selector. UNORM
  bytes (64,128,192,255) become (.2509804,.5019608,.7529412,1). Integer bytes
  stay (64,128,192,255). Wrong-signature controls reproduce billion-scale
  normalized values and zero integer results, respectively.
- The exact production bank's two terrain variants reproduce the float-input
  control image byte-for-byte. Restoring original shaders reproduces baseline
  byte-for-byte. 530,037 pixels change; Sonic control (960,700) stays identical.
  Black point (1320,578) changes RGB (1,0,0) to (137,105,38); white point
  (700,360) changes (255,255,255) to (171,125,67).
- This replay isolates the earlier streak family with a diagnostic discard PS.
  That discard is broader than the production range guard and is NOT shipped.
  It establishes the separate material correction, not exact live equivalence.
- 84/86 existing regressions passed on the first full run. Two extracted fixtures
  needed new bank/input stubs. Both repaired tests pass, giving 86/86 validated
  checks. PSO fixture additionally verifies corrected routing, integer routing
  and rejection of failed float shader creation. Fingerprint fixture verifies
  typed bank bytes affect executable provenance.
- Native Release build succeeds. New package preserves the accepted build,
  forces FSR off, keeps frame generation off, monitor1 and fullscreen enabled.
  Exact executable fingerprint/hash and embedded-bank check are recorded in
  the package build manifest. No game launch during offline validation.

Reproduce the GPU fixture from a compiler environment:
`python tools/gpu-aot/test_float_blend_indices_gpu.py OUTPUT --compiler clang++ --dxc PATH_TO_DXC`.
The saved replay scripts, histories, shaders, inputs, pixel debugger steps and
`snow-replay-verification.json` are beside today's evidence.
