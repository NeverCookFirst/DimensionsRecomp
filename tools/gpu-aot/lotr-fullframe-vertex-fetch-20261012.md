# LOTR full-frame attribution and vertex fetch guard

Game stayed closed; FSR remains disabled. Full capture:
`E:/Claude/LEGO Dimensions/LOTR GPU Frame Capture/captures/20261012-211831-4eb17971/lotr_frame7101.rdc`.
Replay reproduces the visible black streaks and white terrain.

## Attributed streak

At scene target ResourceId::7141, pixel history at (1400,550) shows event 12317
writing valid color. Event 15768 replaces it with (0,0,0,0). At (1300,400),
event 12028 writes valid terrain before the same event 15768 replaces it.
The black result survives lighting, tonemapping, copies and presentation.

Event 15768: 612 uint16 indices, vertex indices 0..254, base vertex 0.
VS A6B647C1BCEB33F9 / PS 80C9AB1591F4C8A8, specialization 0, identified by
DXIL program chunks in the embedded runtime linked bank.
Stream 0: stride 24, 8192 bytes. Stream 1: stride 36, offset 396, only 3700
bytes available. The shader reads secondary tangent/color/UV/blend attributes;
its required indexed range cannot fit. Post-VS positions reach approximately
10^35 with three nonfinite vertices. Primitive 184 connects sane vertices to
a vertex with clip coordinates around 10^19, making the giant streak.

An offline diagnostic replacement that discards the entire affected pixel
shader family removes the streaks in the final frame. Restoring the shader
restores the original broken frame. This replacement is NOT shipped. It may
remove valid draws in the same family and does not prove a complete fix.
White terrain remains visible in the control image.

Late-scene audit (events 15000..15900) found 50 draws using this pixel shader;
40 have a stream range shorter than their indexed fetch range, while ten have
adequate stream extents. The blanket discard control therefore exceeds the
production guard's scope. A gameplay test is still needed to assess the guard's
exact visual result and whether valid-range draws also contain corrupt data.

## Production change

Previously, an invalid stream window disabled the range-cache optimization
and then drew from the whole buffer anyway. Window eligibility is not a
validity check. Added independent attribute-range validation for indexed and
nonindexed draws, enabled whether buffer windows are on or off. It rejects
negative/overflowing/out-of-range effective vertex accesses before issuing
the GPU draw. It checks the actual attribute end rather than unused trailing
stride padding.

A bounded DXBC ISGN/ISG1 input-signature reader ignores attributes this shader
specialization does not read. This preserves legal stale unused streams and
the intentional synthetic zero stream. Rejections produce bounded logs with
shader IDs, buffer, stride, offset and index range.

This contains the invalid draw. It does not reconstruct corrupted geometry
or establish why the original guest binding/data became invalid. Missing
geometry is possible where a formerly corrupt draw is rejected. Continue
investigating the live buffer/declaration/shader binding lifetime and the
separate white terrain/depth issue rather than declaring LOTR complete.

## Validation and candidate

- Captured VS input signature validates required and unused semantic cases.
- Captured bad stream fails the production range helper; valid stream/padding,
  signed base, truncation and overflow cases pass/fail as expected.
- Production extracted nonindexed/QuadList fixtures pass, including compiled
  negative controls. Their assertions now fail via stderr/exit rather than a
  Windows assertion dialog.
- All 86 CTest regressions passed (183.64 seconds).
- Native Release executable built, fingerprint `abedfc9cd8737266`.

Candidate: `E:/Claude/LEGO Dimensions/LOTR Vertex Guard Candidate/`.
Executable SHA256: `33c23c57cf4f227ed657616d0f0f0ac0209485226050b0008aa98f4e1e7dfe8a`.
FSR off, no frame generation, monitor 1 borderless fullscreen. No game launch
or gameplay validation. Use its `START LOTR TEST.cmd` when a test is authorized.

Private replay scripts, histories, buffers, shader identities and before/after
images remain beside the RDC. Range/signature fixture source:
`tools/gpu-aot/test_vertex_fetch_range.cpp` (takes the captured bad VS DXIL path).
