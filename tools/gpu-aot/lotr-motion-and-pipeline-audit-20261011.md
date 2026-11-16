# Offline LOTR, temporal-input and pipeline checkpoint

The owner confirms that the preceding resource/sampler executable fixes the
road, grass, rocks and leaves in Oz level 1. LOTR remains broken in the owner's
test. That is acceptance of one scene, not of all worlds. This continuation
used saved shader/capture data, local reference source and windowless GPU tests.
No game was started, no desktop was used, and no network research was performed.
The current request is FSR 3 upscaling with adjustable settings and **no frame
generation**. Earlier frame-generation component tests do not change that scope.

## Implemented native performance change

`PipelineKey` previously included the guest VS, PS and vertex-declaration
addresses in both equality and hashing. Separate allocations with identical
programs/layouts therefore created redundant PSOs. Saved logs contain repeated
shader hashes at different guest addresses. Equality now uses content hashes,
shader-presence flags and every pipeline-affecting state field. Addresses remain
available for diagnostics. Shader-binding failure guards still run before cache
lookup, and changed programs at reused addresses still miss.

The production-key regression puts 4,095 relocated aliases into the cache and
checks that they reuse one entry. It separately mutates every state field and
every array element, checks same-address program replacement and null shader
presence. Existing binding-failure tests cover failed nonnull bindings with
already cached entries. Final renderer CTest: **79/79 passed, 50.76 seconds**.
Native Release build/link succeeds. No gameplay FPS improvement is claimed.

Built executable: `../game-build/legodimensions.exe`, fingerprint
`1085d0ddc87daeec`, SHA256
`ff60a3ecf61d9254f20372054d325bc7a6e09db55189466d5b5b5d6d525e04a7`.
Private copy: `../session-20261011-offline/native-candidate/legodimensions-pipeline-cache-fix.exe`.
The accepted resource/sampler executable remains separately preserved.
The shader banks and FSR settings were not replaced by the LOD/motion prototypes.

## Motion companion shaders

`prepare_temporal_motion.py` wraps preprocessed translated HLSL. The normal
bank remains untouched. The companion VS evaluates the same vertex program
with current and previous float constants and emits both clip positions.
The PS preserves original discard/alpha and explicit depth behavior, then
outputs normalized previous-minus-current UV motion and a reactive mask.
Invalid previous transforms produce zero motion plus mask 1. The proposed
previous-constant CB is b4/space4, 4,352 aligned upload bytes. It is not yet
part of the native renderer's root layout or submission path.

The windowless fixture checks static, camera, object, wind, alpha cutout and
new-object cases. Covered depth is bit-identical to the original shader.
Two compiled mutations fail: using current constants for previous motion
(exit 7), and deleting alpha discard (exit 5). Eighteen captured shader
specializations compile. Ten actual Oz/LOTR vertex variants also execute
camera/object/current-position checks on a controlled quad with zero unused
attributes. Their pixel probe preserves signature register order; it does
not execute the captured game's material texture/alpha equation. Real wind
deformation is compiled; the GPU wind oracle is synthetic.

The proposed dispatcher hook needs further identity research. In TU23
`82B74FB8`, r5 is saved in r28 and used in integer stride multiplication
at `82B750B0`, not dereferenced as an object pointer. A caller loads it from
graphics-context +288. Treating it as an established persistent object pointer
would be incorrect. Previous constants must be associated with a verified
instance identity, geometry/resource generations and consecutive scene frames.
CPU-deformed vertices and vertex-texture animation also need invalidation or
previous geometry resources. A raw guest address is insufficient.

## LOTR depth findings

Saved prepass `6CB6E1C4106F4571` and materials `B881E7DFA89CF8F4` /
`AF42C59B97499AB6` have matching affine position arithmetic. All eight tested
specialization pairings pass 128 controlled transforms each, with large world
translations cancelled by the view matrix. Each pairing covers 638,432 pixels
with zero depth/coverage differences. A compiled depth-offset mutation fails
with exit 9. This does not test the actual LOTR mesh/camera, animation, texture
bindings or current failing frame. It does not support adding `precise` to
this shared prepass independently of its material partners.

The older LOTR material audit found resource-field differences against uploads
from other frames. Those differences do not establish simultaneous corruption
or a current regression. No saved LOTR RenderDoc frame is available here.

## Explicit texture LOD candidate

An instruction audit of 283 saved containers (158 PS, 125 VS) has zero decode
errors and finds 42 programs using setTexLOD/register LOD, which the current
translator skips. Local Xenia DXBC translator source independently implements
the state and adds register, fetch and instruction bias. This is a verified
translation gap; it is not attribution of LOTR's black terrain. Specifically,
the two inspected terrain PSs `963F001993939275` and `E47483B9029B5F25` use
ordinary 2D fetches without register LOD.

The isolated LOD compiler now preserves the existing gradient and Oz position
fixes and the **624-byte** shared layout. A GPU test caught double fetch bias
in the earlier 752-byte candidate: native D3D12 `SampleLevel` already applies
the sampler's MipLODBias. The corrected explicit argument adds register LOD
and instruction bias only. Native sampler fields and mip limits stay in the
existing sampler path. No new fetch-bias array or draw upload is required.

Seven synthetic microcode-to-HLSL/DXIL cases pass, including predicated state,
signed instruction bias and a refusal of unsupported relative setTexLOD.
A windowless mip-color fixture executes translated microcode with a genuine
ALU load of c0.z into r7.z before setTexLOD. Twenty-seven combinations of
register LOD, signed sampler bias and signed instruction bias pass. Removing
the register state and adding fetch bias twice both fail with exit 10.
All 42 captured affected programs compile with the corrected candidate.
Their cache stays private and has not been installed in the executable.
Instruction filter overrides, explicit gradients, denormalized coordinates
and unsupported helper dimensions remain separate limitations.

## Evidence and reproduction

Private evidence root: `../session-20261011-offline/fsr3-native-inputs/`.
`gpu-fixture/verification.json` records shader hashes, motion checks and eight
depth pairings. `lod-gpu/verification.json` records the sampling cases and both
negative controls. `lod-game-verification.json` records the 42-container cache.
`renderer-ctest-final.log` contains the final 79-test pass.

Run from this source checkout with `E:/devtools/env.ps1` loaded:

```powershell
E:/devtools/python/python.exe tools/gpu-aot/test_pipeline_identity.py ../session-20261011-offline/fsr3-native-inputs/pipeline-identity
E:/devtools/python/python.exe tools/gpu-aot/prepare_lod_candidate.py ../session-20261011-offline/fsr3-native-inputs/lod-candidate --base-source ../session-20261011-offline/moving-regression/pair-compiler/source
cmake --build ../session-20261011-offline/fsr3-native-inputs/lod-candidate/build
E:/devtools/python/python.exe tools/gpu-aot/test_texture_lod_gpu.py ../session-20261011-offline/fsr3-native-inputs/lod-candidate/build/lod_compiler.exe ../session-20261011-offline/fsr3-native-inputs/lod-candidate/source/shader_common.h ../session-20261011-offline/fsr3-native-inputs/lod-gpu --dxc 'E:/devtools/msvc/Windows Kits/10/Redist/D3D/x64/dxc.exe'
```

Native FSR 3 remains unfinished: real motion/depth submission, identity/history
lifetime, scene depth snapshots before reuse, jitter and upscaler dispatch are
not connected. The executable above is a pipeline-cache candidate, **not an
FSR-enabled build**. Frame generation remains disabled. LOTR visual acceptance,
other-world correctness and measured gameplay FPS remain open.
