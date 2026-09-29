# LEGO Dimensions GPU AOT pipeline

This directory keeps the reproducible, project-owned part of the native D3D12
shader pipeline. Generated archives, HLSL, DXIL, and build trees stay under
`rexlego/out/native-gpu` and are intentionally not committed.

## Inputs

- Read-only game archives from the complete Xbox 360 disc set.
- `tools/gpu-shader-extract`, which extracts likely shader assets without
  changing the archives.
- The pinned XenosRecomp checkout under `research/reblue/thirdparty`.
- `patches/xenosrecomp-lego.patch`, applied on top of that checkout.

The patch is required because the upstream fork assumes Blue Dragon's narrower
resource layout. The LEGO adaptation currently includes:

- the four-byte separator between virtual and physical shader-container data;
- bounds-checked microcode decoding and deterministic sequential DXC calls;
- the complete Xenos float, integer, temporary-register, boolean, MRT, and
  32-slot texture/sampler banks;
- vertex-stage texture fetches using explicit LOD;
- D3D12-only cache generation and partial-cache output on failures;
- bounded diagnostics plus single-hash repro modes.

The verified D3D12-only corpus result is **3,481 / 3,481 unique shader
containers (100.00%)**. The generated `lego-dxil-cache.cpp` is about 13 MiB
compressed C++ source in the current build tree.

## Reproduction

Build the extractor with its local project, then:

```powershell
gpu-shader-extract inventory <game-directory>
gpu-shader-extract extract <game-directory> rexlego/out/native-gpu/shader-assets
```

Apply the adaptation to the pinned XenosRecomp checkout and build it in an
x64 MSVC/Windows SDK environment. Generate the shipping D3D12 cache with:

```powershell
$env:XENOS_RECOMP_DXIL_ONLY = '1'
XenosRecomp.exe `
  rexlego/out/native-gpu/shader-assets `
  rexlego/out/native-gpu/lego-dxil-cache.cpp `
  research/reblue/thirdparty/XenosRecomp/XenosRecomp/shader_common.h
```

For a failing shader, set `XENOS_RECOMP_ONLY_HASH` to its `0x...` hash and
`XENOS_RECOMP_VERBOSE_DXC=1`. `XENOS_RECOMP_HLSL_DUMP` may name a directory
for generated HLSL.

## Integration boundary

The generated cache is only the shader half of the native renderer. Shipping
still requires the D3D12 host device, guest resource wrappers, draw/state
hooks, constant and descriptor upload, render-target/resolve handling, and a
safe fallback to the existing `xenos` plugin until native coverage is proven
in gameplay.
