# Dimensions material correctness and FSR research

October 11, 2026. The saved BC1/BC3 mip bytes are correct for the captured
allocations. Active texture fetch state and GPU resource relocation remain
the strongest concrete leads for black surfaces and flickering. The exact
visual cause is unresolved. FSR 3.1.4 works in a standalone D3D12 fixture on
the RTX 4060; native game integration and FSR 4 remain incomplete.

The owner requested offline research and no game launch. All checks used
local files or a windowless synthetic GPU fixture. No network access, game
launch, SDK rebuild, shader-bank replacement, push or release occurred.
Production rendering behavior is unchanged. Independently dirty dependency
trees remain untouched.

## Source and engine structure

The current implementation is `rexlego/out/pr28-validation/source`, branch
`codex/alen-pr28-local-test`, starting at `193e3d1`. The root checkout contains
older renderer code. Current deployed banks use the 624-byte shared ABI;
the earlier 752-byte explicit-LOD candidate was rejected and rolled back.

TT materials bind per-asset precompiled Xenos shader containers and named
material, lighting and miscellaneous constants. Avengers supplies a separate
desktop implementation of related material families, not interchangeable IDs
or bytecode. Its D3D9 backend is an engine reference; its DX11 shaders supply
reflected bindings and arithmetic for comparison.

| Console routine | Verified responsibility | Native consequence |
| --- | --- | --- |
| `82BCE7F8` | Material texture and sampler setup; vertex samplers add 16; selects a 52-byte texture header from backend offset 120 using the index at 116 | Preserve TT's per-slot address, filter and mip requests |
| `83FB58A8` | SetTexture merges resource fields with existing sampler bits, converting CPU aliases to physical addresses | `MergeTextureFetch` implements this merge; a raw header copy would erase sampler state |
| `82BD2868` | Pool relocation helper issues a POINTLIST export copy, then performs bookkeeping | Calling the original helper does not implement its GPU data transfer in the native renderer |

The device fetch bank begins at `0x480`, six dwords per slot. Native texture
bindings cover 26 slots. Draw preparation creates the sampler from the active
device fetch, while the resource registry obtains size, format and storage
from the bound texture's cached header. Inconsistent versions can therefore
produce a valid host image with the wrong sampling state.

The inspected `82BCE7F8` sampler writes cannot alter fetch word 2 (size) or
word 5 dimension bits. A wrong size/dimension needs attribution to a resource
header, another writer, reset, or stale binding. It is not explained by that
material routine's min/mag or address writes.

## Captured mip verification

`verify_captured_texture_mips.py` extracts the existing SDK's guest layout,
packed mip and tiled offset functions into a standalone CPU replay. It
compares saved guest allocations with every byte of the captured DDS mip
payload, including endian conversion and packed mip positions.

For the October 10 captures at `183436` and `185851`, **54 of 54 BC1/BC3
chains passed: 442 mip levels and 20,450,008 payload bytes**. Ten other
records were skipped because they were not BC1/BC3 or lacked saved allocations.
Changing one DDS payload byte produced exactly one difference and exit code
2. Captures and SDK source have hashes in the private verification report.

This rules out disagreement between those saved allocations and their dumped
BC payloads. It does not establish that the allocations were current, the
sampler chose the right mip, the UVs were correct, or blending was correct.

## Active fetch findings

`audit_material_capture.py` uses the third hash column of the saved corpus
index, matching native draw container IDs, and unions texture masks for each
stage. It excludes unused slots and labels unknown masks explicitly. Header
comparison masks preserve sampler differences and CPU alias conversion.

The `185851` capture contains 256 submitted draws. Of 2,368 bound slots,
1,664 are unused by these shaders and excluded. The remaining 704 contain:

- 309 non-texture fetch types: 128 type 0, 53 type 1, and 128 type 3.
- Only 11 of those lack both a resource address and size. The other cases
  cannot all be dismissed as intentional zero-fetch native headers.
- 119 requested mip ranges clamped to level zero by active resource fields.

These counts describe suspicious state, not 309 distinct visual defects.
The old capture has no simultaneous `texture_header_slot` rows. Different
frames cannot prove that a header changed during one draw.

One particularly useful binding is `ED4B2674`, slot 5 in PS
`30E7126AF4380F71`: active fetch
`BD000256,1D255448,00000000,06A84C08,00000EAB,1D255C00` describes
dimension 2 (3D), size 1x1x1 and format 8, with mip requests capped at zero.
A later saved adoption log describes the same guest address as 1024x1024,
dimension 1 (2D), format 20 (BC3), 11 levels. The address may have been reused;
this is a cross-frame discrepancy, not a simultaneous header proof.

The two color shaders `BC67385D86BABED8` and `C427F57A96315196` both use
slots 3, 5 and 6. All 128 selected color draws have type 3 in slot 3. That
slot belongs to the environment-map family, so it deserves a paired active
fetch, current header, registry generation and descriptor check.

`RefreshTextureHeader` runs on SetTexture and resolve paths. An address-only
change keeps resolved host storage authoritative. Layout equality alone does
not prove that a reused header still represents the same logical image.
GPU-authoritative pool sources remain guarded and lack a native relocation
copy. These are separate lifetime/data-transfer questions; removing the guard
would expose stale CPU bytes rather than preserve GPU content.

## Avengers material comparisons

`compare_avengers_materials_offline.py` hashes exact local Dimensions
container spans, then looks up the saved name signatures and disassembles
Avengers candidates privately. It found 1,687 container occurrences for
three selected IDs. `7F247ED809B11B6C` was absent from the older extracted
Dimensions set; no corresponding PC candidate was invented.

`30E7126AF4380F71` has an exact shared-name family. The two color IDs have
a near family differing only by the aggregate name `g_MiscGroupPS`; this
relaxation is recorded explicitly. Neither matching rule proves identical
instructions: the selected PC color shader also uses lightmap slot 2, while
the captured console color mask is `0x68`. The selected PC alpha material
uses only slot 5; the console material uses `0x27` and writes multiple targets.

The PC color candidate reads a normal/surface map at slot 6, diffuse at 5,
and a cube map at 3. Cube LOD is explicitly driven by a material parameter
multiplied by 9. The saved native color DXIL has an implicit cube Sample.
That difference is a targeted LOD investigation, not proof that these two
platform variants must have the same equation. The local 624-byte compiler
candidate contains explicit LOD helpers but omits instruction bias in its
emitted explicit level; adopting it wholesale is not justified by this audit.

The PC alpha candidate performs a texture/material/vertex alpha test and
encodes color with a quantized square-root scale. The saved native material
also contains shader-side encoding and multiple outputs. Alpha can carry
encoding data; a universal premultiplied-alpha, sRGB or UNORM change would
change unrelated passes. Compare each target's shader equation, format,
resolve conversion and blend equation together.

## FSR capability and checks

The local SDK includes FSR 3.1.4 headers, backend shaders and prebuilt DX12
libraries. Its existing presenter dispatch substitutes the color resource
for depth and motion vectors and resets every frame. It is not a valid
native temporal integration. The native renderer presents through its own
path and does not currently supply jitter, scene motion vectors or a scene
color image separated from HUD composition to an upscaler.

`test_fsr3_offline.cpp` uses separate initialized floating-point color,
depth and zero-velocity resources for a static scene. It allocates the SDK's
shared depth/velocity resources, uses the FSR jitter sequence, retains history
across eight frames and checks all RGB output pixels after a fence/readback.
There is no window or swap chain. `run_fsr3_offline.ps1` compiles with the
existing SDK libraries and records source, executable and library hashes.

| Mode | Render size | Output size | Maximum RGB error |
| --- | --- | --- | --- |
| Native AA | 1920x1080 | 1920x1080 | 0.000012994 |
| Quality | 1280x720 | 1920x1080 | 0.000013351 |
| Balanced | 1129x635 | 1920x1080 | 0.000013411 |
| Performance | 960x540 | 1920x1080 | 0.000013351 |
| Ultra Performance | 640x360 | 1920x1080 | 0.000013292 |

All five passed on the NVIDIA GeForce RTX 4060 with zero non-finite pixels.
The fixture establishes SDK dispatch and constant-scene reconstruction;
moving-object disocclusion, reactive masks and native gameplay integration
remain untested. It tests upscaling, not FSR frame generation.

No FSR 4 provider was found in either checked SDK/dependency tree. An FSR 4
option cannot be implemented honestly from the available FSR 3 libraries.
This is a local availability finding, not an offline determination of current
FSR 4 hardware support. Native FSR options remain unimplemented until real
scene inputs and a supported provider exist.

## Reproduction and remaining work

Run from `E:/Claude/LEGO Dimensions`, with the existing E: toolchain loaded.
All outputs below are private and ignored; source tools contain no game data.

```powershell
. E:/devtools/env.ps1
$src = 'rexlego/out/pr28-validation/source/tools/gpu-aot'
$out = 'rexlego/out/pr28-validation/session-20261011-offline'
E:/devtools/python/python.exe "$src/test_audit_material_capture.py"
E:/devtools/python/python.exe "$src/audit_material_capture.py" rexlego/out/pr28-validation/native1080-check/live-20261010-185851 "$out/oz-material-audit.json" --index rexlego/out/pr28-validation/offline-corpus/combined-index.cpp
E:/devtools/python/python.exe "$src/verify_captured_texture_mips.py" rexlego/out/pr28-validation/native1080-check/live-20261010-183436/texture-uploads rexlego/out/pr28-validation/native1080-check/live-20261010-185851/texture-uploads --output "$out/mip-replay"
E:/devtools/python/python.exe "$src/compare_avengers_materials_offline.py" --research research/avengers --hash-dll rexlego/out/pr28-validation/offline-corpus/hash.dll --output "$out/avengers" BC67385D86BABED8 30E7126AF4380F71 C427F57A96315196 7F247ED809B11B6C
& "$src/run_fsr3_offline.ps1" -SdkRoot rexglue-sdk -OutputDirectory "$out/fsr3-1080"
```

The audit's three new regressions passed. Nine existing focused renderer
checks passed: blend, shader usage, 2D upload, upload cache, draw diagnostics,
source residency, authority probe, pool invalidation and pool-copy hook.
Those tests do not establish visual acceptance.

Before changing sampling behavior, pair the already-prepared simultaneous
header capture with the native registry generation and the offending fetch
writer. Before changing travel copies, classify real GPU-source ownership and
preserve its content through a fence-safe copy or relocation. Native FSR needs
scene color before HUD, linear/depth convention, actual camera jitter and
motion vectors including moving geometry, reactive/composition inputs and
history resets on travel/resize. The present saved evidence does not support
claiming black textures, flicker or wrong blending fixed.
