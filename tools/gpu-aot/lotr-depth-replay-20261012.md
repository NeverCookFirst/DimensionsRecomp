# LOTR captured geometry replay, 2026-10-12

User closed the game. No game launch or network access during this investigation.
FSR and frame generation remain disabled. No production rendering fix or new
game executable is claimed.

Input: `session-20261012-offline/native-lotr-geometry-diagnostic/capture-draws.2.draws.tsv`,
frame 11451, 24 submitted B881E7DFA89CF8F4 / 963F001993939275 terrain draws.
All 72 raw geometry references are present (three unique blobs). The earlier
mixed prepass/material capture exhausted its read budget; do not treat it as
complete geometry.

CPU reconstruction found no out-of-bounds position indices or nonfinite
positions, across 12,369 unique vertices counted separately per draw. Input
indices are big-endian uint16; positions are big-endian FLOAT3 at stride 40.

Windowless D3D12 replay expanded the captured indexed positions and uploaded
all 256 captured VS float constants. It compared the actual compiled prepass
6CB6E1C4106F4571 and terrain B881E7DFA89CF8F4 vertex shaders at 1280x720.
All 24 draws produced bit-identical depth and identical coverage. Four draws
had no visible pixels; visible draws provide the meaningful comparison.
A separately compiled shader with a 0.02*w horizontal displacement was rejected:
153,461 depth differences and 6,768 coverage differences on the first draw.
The existing synthetic 128-transform pair test still passes (638,432 samples).

Seven geometry-key matches in the earlier same-frame mixed capture have
identical C0..3 camera and C48..51 world transforms. Other constant differences
do not feed the prepass position calculation.

Reproduction tool: `tools/gpu-aot/replay_lotr_depth_capture.py`. Use arguments
`--capture`, `--bank` (existing fsr3-native-inputs/gpu-fixture), `--output` and
`--dxc` in the configured compiler environment. Verified outputs are under
`session-20261012-offline/lotr-depth-replay-verified/verification.json`.

## Limits and next evidence

This compares position/depth only, using signature-matched probe pixel shaders,
zero auxiliary vertex inputs, and an isolated D32 depth target. It does not
replay original material shading, textures, full render state, native buffer
window conversion, or live resource mutations. It narrows the investigation;
it does not rule out all depth/buffer/lifetime faults or identify the black
strips in the screenshot as these terrain draws.

The capture contains fetch words and texture headers but no texture pixels.
Consequently black lightmap contents, incorrect decoding/sampling and material
shader output cannot yet be distinguished by replay. Existing texture upload
capture supports raw base/mip bytes plus decoded DDS and a pixel-shader filter;
that evidence needs to accompany the geometry before claiming a material fix.
Do not ship another speculative precision, mip-clamp or cache patch on the
basis of this successful geometry test.
