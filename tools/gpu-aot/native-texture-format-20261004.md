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
