# Offline texture format investigation, October4

Latest owner instruction: keep working offline; no game launch. Future authorized
probes use monitor3. The rejected explicit-LOD candidate remains rolled back,
with the production624-byte shared ABI and the existing250 runtime shaders.

Two concrete mismatches were found against the read-only SDK texture code:

- Scalar formats expand to RRRR and two-channel formats to RGGG before the
  guest fetch swizzle. Desktop R/RG and BC4/BC5 defaults instead fill absent
  channels with0/1. Adopted texture views now compose format replication with
  guest swizzling, retaining the separate logical-RGBA color-resolve view.
- DXT3A contains sixteen independent four-bit alpha values in each eight-byte
  block. Treating it as BC4 incorrectly interprets those bytes as palette
  endpoints/indices. It now expands into R8 storage, including packed mips,
  endian conversion and volume slices. Copy footprints use host R8 sizes.

These are format fixes, not an established explanation for the owner's visual
failures. In particular, common BC1/BC3 four-channel scenery is unaffected by
channel replication, and DXT3A was not observed in the latest manual-run log.

Verification on RTX4060, without a game, window or swap chain:

- `test_texture_swizzle.py OUTPUT`:102400 compositions across25 formats
  against the actual SDK format table and GuestToHostSwizzle function body.
- `test_texture_alpha4_upload.cpp`:6144 cases, all four endian modes, odd
  edges, padded rows/slices, packed/permuted offsets and invalid bounds.
- `test_texture_load_gpu.py OUTPUT`:48 comparisons against the SDK's actual
  D3D12 texture_load_64bpb and texture_load_dxt3a compute bytecode, covering
  linear/tiled32x32,128x64,1024x512 and four endian modes. Packed offset testing
  uses CPU extraction; the GPU tiled loader decodes the tile from its origin.
  This verifies byte decoding, not material sampling or game pixels.
- Native Release build passes. Build proof and all local test payloads stay
  under ignored `session-20261004`; SDK/runtime/plugin/config were preserved.

Instruction audit uses actual EXEC sequences rather than byte-pattern matches.
Runtime250 contains563 texture fetches,41 SetLOD and5 GetWeights operations.
The scanned main asset corpus9289 contains31847 texture fetches,1110 SetLOD,
100 GetGradients and702 GetWeights. All audited texture fetches use normalized
coordinates, fetch-constant filters and no register-gradient flag. This does
not establish that the omitted non-fetch operations are harmless, and does
not justify reinstalling the rejected LOD candidate. The EXEC fixture checks
paired/conditional sequences, deduplication, ALU/vertex exclusion and extents.

## Prepared scene-specific evidence

`run_native_probe.ps1 -LongProbe -PortraitTrace -TextureForensics -CaptureMissing`
prepares a disabled-until-triggered capture. It does not arm at startup. Once
the owner reaches the affected scene in an authorized monitor3 run, create
the fresh process JSON's textureCaptureTrigger file. The renderer will decode
already-cached textures once for capture without forcing an unchanged GPU
upload. Each record contains all mip DDS pixels, exact base/mip guest bytes,
six fetch words, shader identities and frame. Captures are restricted to2D,
32 textures and128MiB; no filesystem checks occur when capture is disabled.
The current DDS writer supports BC1, BC3 and RGBA8. It does not dump every
possible texture format. Existing first-eight startup captures were inadequate
to identify the LOTR striped texture's mip bytes.

`test_texture_upload_capture.py OUTPUT` executes the actual capture functions
and checks late arming, cached capture eligibility, seven mip levels, stripped
row/mip padding, exact guest backing, legacy logo filtering and capture limits.

Portraits, vehicle icons, button prompts, LOTR artifacts, missing shaders and
three-level/cutscene correctness remain open. Internal rendering remains720p;
actual1080p and stable60FPS are not achieved or measured by these tests.

## Isolated gradient instruction correction

The audit now records non-sampling texture operations with their asset paths.
The production compiler silently skips GetTextureGradients (opcode18). The
read-only SDK D3D12 translator computes coarse derivatives in the order
dx(source.x),dy(source.x),dx(source.y),dy(source.y), independent of a texture.
An isolated compiler prepared by `prepare_gradients_candidate.py` adds this
operation with source/destination swizzles, destination0/1/keep and instruction
predication. It refuses vertex and relative-address forms rather than silently
emitting an incorrect result. Shared ABI remains624; no LOD change is included.

`test_texture_gradients.py COMPILER COMMON OUTPUT` passes50 actual synthetic
microcode/HLSL/validated-DXIL fixtures and three unsupported-mode refusals.
Source r7 is initialized from SV_Position so XY derivatives vary. All89
affected actual asset containers compile, with all100 gradient assignments
present in emitted HLSL. Four belong to hub_lotr_05; this does not prove these
are the draws in the owner's failed screenshot. Avengers avgref has no PC
signature twin for the queried LOTR representative157; no substitute shader
was copied or treated as ground truth. Candidate outputs remain ignored under
session-20261004/gradients-candidate and are **not installed** into production.
All250 captured runtime shaders also compile with byte-identical emitted HLSL
against the prior production compiler. None uses GetTextureGradients, so this
is a useful check that the isolated change leaves those existing draws alone.

## Installed gradient-query and captured-shader build

Final offline build fingerprint60c62465f26a1152, exe SHA256
19b93e7c54978cee515f8b0ad8417088a4c7d925e2089fb6d641cf4198ef61c3.
Matching main9290/18444 and runtime255/458 archives are installed; every
specialization subset is present. Exact main DXIL comparison changes89/9290
payloads, matching all89 gradient-query containers; other9201 unchanged.
All old250 runtime identities/masks,448 linked keys and HLSL bytes preserved.
Five nested placement containers from the closed diagnostic add ten index
records:18983->18993 with every old record retained. Corrects the earlier
nonrecursive missing-container count. No reference-game payloads embedded.

Native build passes with624 shared bytes. Runtime DLL pair and configuration
match the before-installed SHA256 records, DoF stays OFF, SDK/Vulkan/Plume are
untouched. Tests include50 gradient translation/DXIL fixtures plus3 refusals,
100 actual gradient instructions, captured-shader compilation and complete
prelinked variant coverage. Proofs are under
session-20261004/gradients-candidate:payload-differential-proof.json,
runtime255-coverage-proof.json,index-proof.json,installed-build-proof.json.
Coupled rollback files remain in before-installed.

Game remains closed, no new launch. The captured hub/LOTR images still fail
portrait/texture checks; this build has not been visually validated. Actual
1080p rendering, stable60FPS, AMD and three story-level/cutscene checks remain
pending. Do not count diagnostic traces as clean performance benchmarks.
