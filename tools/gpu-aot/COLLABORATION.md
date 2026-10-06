# Native renderer contributor handoff

This branch contains an experimental C++ native D3D12 renderer and its AOT
translation tools. `LEGODIMENSIONS_NATIVE_GPU` defaults to `OFF`. This is a
development handoff, not a completed renderer or a release.

## Linux limitations

- `rexlego/CMakeLists.txt` explicitly rejects `LEGODIMENSIONS_NATIVE_GPU=ON`
  outside Windows. The native implementation includes Win32/D3D12 APIs. No
  Linux native renderer, Wine, or cross-compilation run was validated here.
- The current compiler-preparation CMake uses Windows DXC `.lib`/`.dll` files,
  MSVC runtime options, and Windows dependency output names. The runtime probe
  scripts are PowerShell/Windows tools. They are not Linux build instructions.
- A clean clone lacks `rexlego/generated/rexglue.cmake`, translated game C++,
  game data, and AOT shader archives. These are deliberately ignored. The game
  CMake includes generated/rexglue.cmake before its native-platform check.
  Follow `README-dev.md` using your own game inputs for game-code generation.
- The asset-free CMake checks below do not configure the game or require its
  generated C++, SDK submodules, DXC, a GPU, or game data. Ubuntu CI runs this
  target. It tests helpers, not image correctness or game performance.

## Clone and portable checks

```sh
git clone --branch codex/native-renderer --recurse-submodules https://github.com/NeverCookFirst/DimensionsRecomp.git
cd DimensionsRecomp
cmake -S tools/gpu-aot -B native-checks -DCMAKE_BUILD_TYPE=Release
cmake --build native-checks --parallel 2
ctest --test-dir native-checks --output-on-failure
```

Requirements for this target: Git, CMake >=3.25 and a C++20 compiler.
It registers 19 tests. Release assertions stay enabled. The workflow is
`.github/workflows/native-gpu-checks.yml`; consult its actual run result before
reporting Linux validation.

The SDK gitlink is `17e3675767067339581018c7e722e1d8d65de201`, published on
the fork's `codex/native-renderer` branch. Its dependency draft is
https://github.com/NeverCookFirst/rexglue-sdk/pull/1. Use the gitlink with
`git submodule update --init --recursive`; `git submodule update --remote`
selects branch tips instead of the recorded dependency revision.

## Reconstruct current shader compiler inputs

The compiler source is reconstructed from an external pinned checkout plus
tracked adaptations. It is not contained in the ignored local session trees.
In a fresh checkout, initialize the external compiler:

```sh
git clone https://github.com/zolaware/reblue-XenosRecomp.git research/reblue/thirdparty/XenosRecomp
git -C research/reblue/thirdparty/XenosRecomp checkout 339af41df2c23dbe3256c1c377716b81a0e0fe6b
git -C research/reblue/thirdparty/XenosRecomp submodule update --init --recursive
git -C research/reblue/thirdparty/XenosRecomp apply ../../../../tools/gpu-aot/patches/xenosrecomp-lego.patch
python tools/gpu-aot/prepare_native_compiler.py research/reblue/thirdparty/XenosRecomp rexlego/out/native-gpu/current-compiler
```

The wrapper checks the revision and applied patch, then invokes alpha,
viewport and gradient preparations in that order. It requires a fresh output
directory, does not modify the compiler checkout, and does not install banks.
The resulting ten C++/header files were compared with the October4 installed
compiler source and matched after newline normalization. Shared constants
remain 624 bytes. This source comparison is not a Linux compiler-build test.

For Windows compiler builds, `prepare_alpha_candidate.py` currently expects
the pinned compiler's dependencies already built in `out/build/lego-release`:
`xxhash.lib`, `zstd_static.lib`, `fmt.lib` and `fmt.dll`, plus the pinned DXC
distribution. Its generated CMake lists the exact paths. Build those upstream
dependencies in an MSVC/Windows SDK environment before configuring
`current-compiler/gradients/source`; the executable target is
`gradient_compiler`. The wrapper accepts a checkout at any filesystem path.

Use this compiler for both main and captured runtime containers. Rebuild both
raw archives, prelinked variants and the placement index together. Never pair
624-byte host uploads with the rejected 752-byte explicit-LOD archives. The
extractor/index/prelink commands and input layouts are described in README.md.

## Local inputs and collaboration

Git contains project-owned source, tools, patches and investigation notes.
It does not contain game executables/data, generated game C++, shader
containers/DXIL/HLSL banks, saves, captures, or the Avengers reference install.
No Avengers game payload is embedded. Contributors generate game-derived
inputs from their own copies; private owner-to-owner transfers are separate
from this PR. A clone cannot reproduce the exact 255-container runtime corpus
without those local captured inputs.

For a private workspace handoff, inventory `rexlego/generated`, selected
`rexlego/out/native-gpu` inputs/banks/rollback proofs, save/config files and
captures. Keep compiler/runtime hashes with them. Do not copy CMake build trees
as a substitute for configuring the new machine: their cache stores absolute
paths. The ignored root PROJECT-MEMORY.md and original task prompt are local
context; this tracked document and dated notes provide the public handoff.

Use personal branches and PRs against `codex/native-renderer`. Record the
game and SDK commits, shader compiler input revision, constants ABI, archive
hashes, actual render-target dimensions and capture/test identifier with each
result. Do not report diagnostic tracing as a clean FPS benchmark.

## Verified state and remaining checks

On October6, the 19 asset-free checks passed on Windows with Clang20.1.8
and on Ubuntu CI with GCC13.3.0. The successful Linux run is
https://github.com/NeverCookFirst/DimensionsRecomp/actions/runs/37485760314.
The Windows native game target also built successfully; no game was launched
for this publication. The depth-bias addition is included in pipeline identity
and rasterizer configuration and has helper checks for face selection,
positive/negative offsets and non-finite inputs. Its effect on gameplay images
has not been tested. `LEGO_NATIVE_NO_DEPTH_BIAS` disables it when present.

The October4 installed archives had main9290/18444 and runtime255/458
shader/linked-variant coverage, with18993 placement records. These are local
artifact counts, not files shipped in this PR. The last captured hub/LOTR
images still showed blank portraits and rendering artifacts. Visual checks
of the current compiler/bias combination, actual internal1920x1080, stable60FPS,
three story levels/cutscenes and AMD hardware remain outstanding. The latest
saved owner configuration has depth of field disabled.
