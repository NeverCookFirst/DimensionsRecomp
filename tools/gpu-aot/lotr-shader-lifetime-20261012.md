# LOTR shader lifetime candidate, 2026-10-12

User requested an offline LOTR texture fix attempt followed by a build for their
own test. No game launch, desktop capture, network access or frame generation.

## Finding and change

Borrowed XDK placement shader objects were cached by guest address indefinitely.
The initializer and pool-copy hooks invalidate known writes, but a header or
microcode change outside those hooks could retain an old program, PSO identity
and texture-slot mask. A failed binding also remained unavailable without a new
SetShader call or a precompiled-pack reload.

Before draw filtering and PSO/texture selection, validate each borrowed program's
header, backing address, readable extent and full physical-code XXH3 identity.
Validation is bounded to once per resource per presented frame. Changed objects
are invalidated and re-adopted; the existing fence retirement retains old owners.
Retry unavailable requested bindings once per stage/address/frame. Intentional
null pixel shaders remain null. Owned shader objects bypass these checks.

This reproduces a correctness gap, not proof that the saved LOTR run encountered
it. Unhooked writes after validation within a frame can wait until the next frame;
known initializer/pool-copy invalidation remains immediate. Actual performance
cost and visible LOTR recovery still require the user's game test.

## Offline evidence

- Release executable built successfully, fingerprint `4b4229b8e471029f`.
- Renderer suite: 85/86 initially passed; texture_depth_upload failed to compile
  because its extracted draw fixture lacked the previously added diagnostic
  helper. Added a no-op logging stub (logging tested separately); rerun passed.
  All 86 tests therefore passed across the full run and targeted rerun.
- New shader_use_refresh test executes extracted production bodies. It covers
  changed code and texture masks, relocation, unavailable/readable transitions,
  binding retry without another setter, null PS, old-generation retention and
  bounded reads/lookups. Removing cache validation fails with exit 23; removing
  retry fails with exit 24; fixed implementation exits 0.
- Captured LOTR containers `963F001993939275` (1072 physical bytes) and
  `E47483B9029B5F25` (484 bytes) pass unchanged identity validation and reject
  modified first, interior and final bytes. This tests identity, not rendered
  material pixels or real guest streaming timing.
- Evidence under `../session-20261012-offline`: lotr-build.log, lotr-ctest.log,
  lotr-ctest-repaired.log, lotr-shader-captured/verification.json.

## Delivery

`../session-20261012-offline/native-lotr-material-candidate/legodimensions.exe`

SHA256: `bb02c6d96febe6cac267f76fcb80531d8e1183ade2e96c9723490e4ea159a0ec`.

FSR 3 Quality enabled, sharpening 0.2, no frame generation. Graphics settings
remain adjustable. Accepted Oz material/shader bank and previous FSR stability
changes are preserved. Previous package retained. No visual fix, first-visit
loading fix or FPS improvement claimed. Test affected LOTR terrain, foliage and
rocks and retain the candidate's adjacent game.log.
