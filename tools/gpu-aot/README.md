# LEGO Dimensions GPU AOT pipeline

This directory keeps the reproducible, project-owned part of the native D3D12
shader pipeline. Generated archives, HLSL, DXIL, and build trees stay under
`rexlego/out/native-gpu` and are intentionally not committed.

Start with [the contributor handoff](COLLABORATION.md) for clone instructions,
the Linux restrictions, asset-free checks and current compiler reconstruction.

## Current compiler and constants ABI (October 4, after owner visual test)

The owner rejected the combined explicit-LOD build: LOTR looked worse, with
vertical artifacts, while texture stripes and blank portraits remained. It has
been rolled back as a coupled host/constants/shader-bank change. Production
uses the gradient compiler in
`session-20261004/gradients-candidate/build/gradient_compiler.exe`, prepared from
the exact October1 alpha/viewport source with `prepare_gradients_candidate.py`.
The matching `source/shader_common.h` retains **624 shared bytes**. Main9290
shaders/18444 linked variants and runtime255/458 are installed; placement index
18993 preserves every previous record. The runtime additions are five complete
containers captured in the October4 diagnostic, not reference-game content.
Only89 main DXIL payloads change, exactly the89 gradient-query shaders; all
other9201 payloads and old250 runtime HLSL files are byte-identical.
The Enter transition correction and Alt+Enter handling are retained.
Build proof: `session-20261004/gradients-candidate/installed-build-proof.json`.
This is offline verification; portraits, prompts, texture stripes and gameplay
performance remain unverified. DoF remains OFF; no additional game launch.

The rejected candidate compiler is the combined alpha/viewport/explicit-LOD
source in `session-20261004/lod-combined/source`, prepared reproducibly with:

```powershell
python tools/gpu-aot/prepare_lod_candidate.py rexlego/out/native-gpu/session-20261004/lod-combined --base-source rexlego/out/native-gpu/session-20261001/viewport-candidate/source
```

That candidate requires `XENOS_RECOMP_LEGO_NATIVE_SCALE=1` and **752 shared bytes**: the
existing 624-byte layout plus 32 fetch LOD biases at offset624. Regenerate both
main and runtime archives and their linked variants together with the matching
host upload. Do not reinstall this candidate without isolating its regression.
The original isolated September30 LOD candidate lacks the later alpha/viewport
fixes and must not replace this combined compiler.

Main coverage remains9290 (known A58 memexport exclusion), runtime255; all prior
hashes and specialization masks are preserved. The candidate implements ignored
explicit mip-level instructions but failed the owner's image check. Synthetic
fixture success did not establish the game's complete sampling correctness.
No game launch was authorized for this integration. See `native-work-20261004.md`.

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

The byte-aligned scanner finds **9,291 unique shader containers**, including
the captured runtime shaders. The 2026-09-29 expanded build compiled **9,290**;
runtime vertex shader `A58EBA123EFB541C` is rejected because its memory export
has no implemented writeback path. The earlier 3,481/3,481 figure counted only
containers found by the incorrect four-byte-aligned scanner, not full coverage.
Keep `lego-dxil-cache.cpp` for rollback; the expanded output is
`lego-expanded-cache.cpp`. Select it with CMake's `LEGO_NATIVE_SHADER_CACHE`.
The placement index now has 18,504 signatures.

### Placement capture update (2026-09-29)

The current local index has 18,559 signatures after adding 32 exact runtime
placement containers. All 32 compile successfully. The first pair was not found in
the extracted disc/TU23 corpus; do not bind a similar shader by prefix alone.
With `LEGO_DUMP_MISSING_SHADERS=<output-directory>`, native diagnostics capture
up to 256 distinct missing placement objects per process. XDK retains the
virtual header at object+872 (VS) / object+40 (PS), while its physical section
is a separate allocation. Both ranges are checked before reconstruction.
Only the `placement-containers` child directory is a compiler/indexer input;
`placement-raw` contains bounded diagnostic fragments, NOT containers.
Address reuse and the capture cap mean this is not an exhaustive inventory.

The merged local input is `rexlego/out/native-gpu/placement-input`. It currently
generates `runtime-cache.cpp`, followed by CMake's runtime prelink target.
The old two-entry runtime cache is backed up as `runtime-cache.pre-placement.cpp`:
its valid PS already exists in the main cache, while its old memexport VS
`A58EBA123EFB541C` is intentionally no longer shipped (current compiler rejects
its unsupported path). Native memexport/writeback is still not implemented.

### Intermediate image diagnostics

The September 30 continuation merges every validated placement capture into
`placement-input-all-captures`: 91 unique containers, 91/91 DXIL compilations
with `XENOS_RECOMP_LEGO_NATIVE_SCALE=1`, 18,670 indexed signatures and 146
runtime prelinked variants. The previous 73-entry and 32-entry caches are
backed up in `session-20260930`. A zero physical prefix is valid when its
instruction stream starts later in the section; inspect the declared offset.
Use `run_native_probe.ps1 -Label <fresh-label> -CaptureMissing` to collect
additional actual containers, then close the recorded process with `-Close`.

Snapshot probes use wall-time windows at 0/8/16/24/32/48 seconds after the
first tiled pass (`LEGO_GPU_SNAPSHOT_TIME_WINDOWS=1`). This includes later
geometry passes when fixed frame indices would all sample the fast starfield.
The setting remains opt-in; regular gameplay has no snapshot readback.
Create `capture-next-frame` inside the active capture directory to capture a
later menu or hub frame without restarting the game. At most 16 mesh draws are
captured per frame, reserving room for the final resolve and postprocessing.

`test_resolve_region.cpp` checks padded extents, four atlas placements, cropped
source offsets and rejection of out-of-bounds rectangles. Resolve now consumes
SourceRect/DestPoint; k_24_8 depth sampling uses an R32_FLOAT atlas mirror.
Stencil and CPU readback are not supplied by this mirror.
The native draw path now honors `skip_pixel_shaders`, including the existing
Graphics depth-of-field toggle. It maps exact indexed microcode hashes to AOT
containers and refreshes when the setting changes. `test_pixel_shader_filter.cpp`
checks valid lists, duplicate hashes, whitespace, optional prefixes and errors.

`test_descriptor_retirement.cpp` checks descriptor reuse against all three
live frame fences, out-of-order completion, repeated release and reuse.
Native texture Release now reserves its SRV slot through those fences as well
as retaining the underlying texture/view; keeping just the texture alive does
not protect recorded draws from a descriptor being overwritten.

Set `LEGO_GPU_SNAPSHOT_DIR` only for a short native-renderer test. The opt-in
readback records HDR draws, color resolve sources and the final present input
on bounded frames relative to the first HDR pass. Completed fences gate DDS
mapping; no intermediate images are read back during normal gameplay.
Set `LEGO_GPU_SNAPSHOT_START_AT_TILING=1` to anchor these frames at the first
successful BeginTiling instead of startup HDR work. Captures include expanded
color resolves and float sampling views. Keep capture runs separate from FPS runs.

The 2026-09-30 fixed16 bank additionally requires
`XENOS_RECOMP_LEGO_NATIVE_SCALE=1` during both main and runtime DXIL generation.
Its pixel-shader epilogue reads four output scales at SharedConstants byte 608
(total host layout 624 bytes), after alpha testing. These scales come from
each surface's RB_COLOR_INFO format and signed exponent, including the fixed16
division by 32. Use `lego-fixed16-cache.cpp` with the matching runtime cache;
the previous banks and executables are retained for rollback.

The placement index now stores conservative texture-slot masks from all Xenos
exec clauses. Unknown input retains all 32 slots. Native draws check uploads only
for slots referenced by either shader; hashes and updates for those slots remain
enabled. `LEGO_NATIVE_ALL_TEXTURES=1` restores the previous all-binding behavior
for comparison. `test_shader_texture_usage.cpp` covers conditional END clauses,
slot 31, vertex fetches, ALU-only programs and malformed input.

```powershell
E:/devtools/python/python.exe tools/gpu-aot/inspect_gpu_snapshot.py <capture-directory>
E:/devtools/python/python.exe tools/gpu-aot/inspect_gpu_snapshot.py <capture.dds> --image
```

`--image` creates an RGB PPM, and optional `--scale` changes diagnostic exposure
only. Inspect the PPM or convert it to PNG; avoid relying on another decoder's
implicit channel interpretation for DX10 DDS. Native stage logs identify shader
addresses, targets, viewport, scissor, textures, write mask and blend state.

Current local user preference (2026-09-30): launch tests on `monitor=3` and
scope `SDL_WINDOW_ACTIVATE_WHEN_SHOWN=0` to the child process. The assistant
handles loading up to the initial cutscene; later gameplay tests are coordinated
with the user. Do not use an automatic close timer during a user-assisted test.
Close only the exact assistant-launched test process after a diagnostic check;
do not interact with the user's other game.

Standalone state regressions (compile with the existing C++20 toolchain):
`test_vertex_byte_order.cpp`, `test_blend_state.cpp`, `test_texture_number.cpp`.
These are correctness tests, not performance benchmarks or gameplay coverage.

`test_texture_upload_cache.cpp` additionally needs
`-I rexglue-sdk/thirdparty/xxHash`. It checks that source-cache keys change for
base data, the last cube face, mip tails, fetch semantics and range sizes.
The runtime hashes readable guest base/mip spans BEFORE allocating/untile;
unreadable conservative spans fall back to the previous per-block path.
This avoids repeated conversion, but it still scans bytes on every binding:
it is not page-dirty tracking, and CPU texture edits are not ignored.

For a bounded timing check, set only `LEGO_NATIVE_TIMING=1` (unset GPU snapshot
and texture/missing-shader dump variables). Slow-frame lines report intervals
between presents and total CPU time/calls/source-cache hits/conversion bytes
in texture upload. This adds no GPU readback or extra fence wait. These are
startup diagnostics, not standalone/comparable performance benchmarks while
the user is running another game.

Depth-output regression (requires the built XenosRecomp executable):

```powershell
E:/devtools/python/python.exe tools/gpu-aot/test_depth_export.py `
  research/reblue/thirdparty/XenosRecomp/out/build/lego-release/XenosRecomp/XenosRecomp.exe `
  research/reblue/thirdparty/XenosRecomp/XenosRecomp/shader_common.h `
  rexlego/out/native-gpu/depth-regression
```

The six synthetic containers test color-only shaders, inaccurate reflection,
and real vector/scalar/constant/predicated depth exports through HLSL and DXC.
Only actual decoded depth exports add `SV_Depth`; declaring it unconditionally
and initializing it to zero breaks ordinary raster depth. Both the main and
32-entry runtime archives (and their prelinked variants) must be regenerated
after changing this codegen, not just the executable or dumped HLSL.

`lego_gpu_microcode_index` separately records the physical Xenos microcode
identity needed to adopt placement shader objects. It does not run DXC or
change the compiled archive.

## Reproduction

**Current native alpha support needs the compiler produced by
`prepare_alpha_candidate.py`, with its generated shader_common.h.** The older
generic XenosRecomp executable below still emits only the legacy >= test.
Regenerate both banks and all linked variants together; do not replace one
installed alpha bank with output from that older compiler. See the offline
alpha section and coverage proof in session-20260930/alpha-candidate.

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

Then build and run the placement indexer over both the archive corpus and any
captured runtime containers:

```powershell
cmake --build rexlego/out/build/win-amd64-release --target lego_gpu_microcode_index
rexlego/out/build/win-amd64-release/lego_gpu_microcode_index.exe `
  rexlego/out/native-gpu/lego-microcode-index.cpp `
  rexlego/out/native-gpu/shader-assets `
  rexlego/out/native-gpu/runtime-missing
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

## TU23 sampler regression check

`test_sampler_state.cpp` compares the native fetch-header merge with the actual
generated `83FB58A8` body, including all 26 slots, NULL unbind, physical address
fixups and dirty-mask writes. Extract the oracle into the session scratchpad
before compiling (the test does not start the game):

```powershell
python -B tools/gpu-aot/extract_sampler_oracle.py --output rexlego/out/native-gpu/session-20260930/tu23_set_texture_oracle.inc
clang++ -std=c++20 -O2 -DNOMINMAX -Irexlego/src -Ithirdparty/plume -Irexlego/out/native-gpu/session-20260930 tools/gpu-aot/test_sampler_state.cpp -o rexlego/out/native-gpu/session-20260930/test-sampler-state.exe
rexlego/out/native-gpu/session-20260930/test-sampler-state.exe
```

This also checks host sampler address/filter/anisotropy/LOD modes and cache
identity. It cannot establish visual correctness, actual guest default table
contents or descriptor behavior on a live D3D12 device. Native startup logs the
20 defaults and rejects initializer functions outside the audited TU23 set.
Runtime sampler/VS-slot diagnostics are bounded; no descriptor is overwritten
or recycled while the device is active. Explicit register LOD and unusual
border colors remain separate shader/backend limitations.

`audit_sampler_defaults.py --output <scratch.json>` reads the 20 getter/setter/
default triples from the mapped TU23 image and checks their exact whitelist,
generated leaf bodies and direct/conditional branch targets. The float
constants are 32 for MipMapLodBias and -8 for AnisotropyBias. The September 30
sampler-fetch run independently logged the same 20 defaults and initialization
of all 26 slots. Static table checks alone cannot establish a live device state.

Snapshot diagnostics also report changed borrowed texture headers and changed
VS texture fetch words. A draw does not automatically replace a texture from
its latest header: an existing draw uses the active device fetch shadow.
At SetTexture and Resolve, the new binding comes from the header, so refresh
borrowed resources there. An address-only change updates the raw CPU alias and
invalidates the upload key; a layout change retires the old storage and SRVs at
their GPU fences. Keep host-resolved contents authoritative during relocation.
The manual `capture-next-frame` trigger selects the
next frame so noticing the sentinel halfway through a pass cannot produce a
misleading partial-frame capture. Existing time-window captures remain bounded.

## Isolated explicit-LOD candidate

The captured TU23 corpus contains 14 pixel shaders that set a register LOD
and then fetch a cube texture on slot 3 with `use_register_lod`. The current
compiler skips `SetTextureLod` and uses implicit sampling. Local Xenia's
`dxbc_translator_fetch.cpp` preserves the scalar and adds the signed fetch
bias divided by 32 plus the instruction bias divided by 16 before explicit
sampling. This mismatch is established from code; its visual impact has not
been tested.

`prepare_lod_candidate.py` copies and patches the compiler into a scratch
directory, leaving the production compiler, shader banks and native executable
unchanged. It also writes an isolated `draw-candidate.cpp` showing the matching
constant upload. Build with the existing local compiler dependencies:

```powershell
. E:/devtools/env.ps1
python -B tools/gpu-aot/prepare_lod_candidate.py rexlego/out/native-gpu/session-20260930/lod-candidate
cmake -S rexlego/out/native-gpu/session-20260930/lod-candidate/source -B rexlego/out/native-gpu/session-20260930/lod-candidate/build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=clang++
cmake --build rexlego/out/native-gpu/session-20260930/lod-candidate/build --target lod_compiler -j6
python -B tools/gpu-aot/test_texture_lod.py rexlego/out/native-gpu/session-20260930/lod-candidate/build/lod_compiler.exe rexlego/out/native-gpu/session-20260930/lod-candidate/source/shader_common.h rexlego/out/native-gpu/session-20260930/lod-candidate/fixtures
```

The seven end-to-end fixtures cover register/zero LOD, instruction predication,
positive/negative instruction bias, computed pixel LOD and vertex LOD behavior.
DXC validates their output; relative `SetTextureLod` addressing is rejected.
The isolated candidate also compiled all 14 actual containers to nonempty DXIL
entries. Their generated HLSL preserves each decoded LOD source and uses
`SampleLevel`; hashes and checks are saved in the session's
`lod-candidate/verification.json`. Neither result establishes correct game
pixels or frame timing.

**Do not install these candidate shaders in the current runtime bank.** They
require `XENOS_RECOMP_LEGO_NATIVE_SCALE=1` and a SharedConstants ABI of 752 bytes,
with 32 fetch LOD biases beginning at byte 624. The current sampler build still
uploads 624 bytes. A later integration must update both shader archives and
their prelinked variants together with the host upload. Observe the sampler
build before combining this separate change. Explicit gradients, instruction
filter overrides, denormalized coordinates and 1D helper coverage remain
separate limitations.

## Bank118 and menu idle reproduction, September 30

### October 3 hub continuation

`oct03-hub-diagnostics` captured 57 additional validated placement containers
in Vorton. The local candidate combines these with all 118 previous runtime
containers: 175/175 compile with the October 1 compiler and its existing
624-byte constants ABI. The merged index contains 18836 signatures and preserves
all 18722 earlier records; runtime prelink produces 304 variants. Inputs,
rollback archives and build proof are in
`rexlego/out/native-gpu/session-20261003/hub-bank-candidate`.

`test_placement_capture_lookup.py <placement-containers> <index.cpp>
<runtime-cache.cpp> <output-directory>` exercises the production lookup against
both the physical and instruction sections of each actual capture, requiring
the exact compiled container hash. All 57 new containers pass. Raw placement
fragments remain diagnostic-only and must never be compiler inputs.

The second native capture (`oct03-hub-bank175`) shows restored HUD icons and
additional hub geometry, and captured no further placement containers during
this short run. The character portrait remains a blank blue circle. This is
a partial improvement, not completed hub correctness or performance validation.
Both native probes were closed; further probes use monitor 2 per the user's
October 3 instruction. Do not infer a performance result from snapshot/trace
runs. The subsequent emulated Vorton comparison shows Sonic's portrait; native
Sonic and Batman both have the shared empty portrait. After the Enter burst
test the user prohibited further game launches until explicitly authorized.
Offline follow-ups: `native-transition-findings-20261003.md`,
`native-buffer-watch-20261003.md`, `native-vertex-upload-20261003.md`, and
`avengers-reference-findings-20261003.md`. The fused vertex optimization is
built; buffer watches remain opt-in. No post-change gameplay FPS claim.

The September30 production runtime bank contained 118 validated containers, 18722
microcode signatures and 192 linked runtime variants. It retains the current
624-byte constants ABI. The six late containers had valid nonzero instruction
streams despite leading zero words. `merge_microcode_index.py` preserves the
previous index and rejects conflicting rows. Build proof and original inputs
are in `session-20260930/bank118-candidate/`.

The user confirmed substantially better late cutscene geometry and correct
characters in build `cf7383e12e047c83`. The title screen and main menu initially
looked correct, then idle reproduced corrupt logo/UI textures. Archive:
`bank118-user-menu-good.png`, `bank118-user-menu-idle-corrupt.png`,
`bank118-header-menu-idle-corrupt.png`, and the complete manual GPU frame2386.
The game was closed normally at16:55:41. It is not a stable image baseline.

The log contains 32 distinct stale-header pairs: 30 relocations and two layout
changes. `test_texture_header_refresh.py` extracts the production refresh body
and exercises those actual pairs with CPU and host-resolved authority, owned
resources, unchanged headers, mip-allocation transitions and type reuse (131
cases). The harness and descriptor-retirement test pass. With the refresh and
opt-in diagnostics, the user confirmed stable main-menu image for 390.8 seconds
in `cutscene-menu-long2`, then requested a restart for the full cutscene. This
exceeds the previous >3-minute reproduction, but is not a completed 10-minute
test. The cutscene was skipped in that run. Retain
`before-texture-header-refresh/` as rollback.
Next visual check: late cutscene, title-to-menu transition, then menu idle past
the prior corruption point before settings/hub. Excessive late blur and FPS
remain separate, unresolved checks.

## Long cutscene and menu idle probe

Launch with `run_native_probe.ps1 -Label <fresh-label> -Mode snapshot
-CaptureMissing -LongProbe`. The game keeps its normal config and
no-focus launch. The opt-in trace writes `trace-<label>/events.tsv`: every frame's
CPU timings, texture upload hash changes, header relocations/layout changes,
fetch changes, newly seen draw stages, resolve regions and failed bindings.
Anomaly flags are candidates, including legitimate fades and resource reuse;
they need screenshot review. They do not imply a diagnosed fault.

The final image is captured every5seconds and full frames every30seconds,
with extra next-frame captures on candidates (10second renderer cooldown).
GPU readbacks are bounded at12GiB; reaching the budget emits an event while
event/timing logging continues. Whole-frame captures include actual8816-byte
constant uploads from draws with at most6vertices, capped at128files per frame:
VS float constants at0, PS at4096, SharedConstants at8192
(624bytes). `draw_constants` rows associate files with shader hashes. During
long runs draw-stage snapshots focus on draws with at most6vertices; resolves
still capture the HDR and intermediate images. `snapshot_saved` records the
absolute captured frame because DDS filenames use the relative tiling frame.

The hidden watcher verifies the game's PID/path/start time, records process
memory and log liveness in heartbeat.json, and requests a frame on errors.
It exits when the game exits; it never closes the game. To start the actual
menu timer, write `trace-<label>/menu-start.json` with `{"utc":"<UTC ISO time>"}`
only after main-menu entry is observed or confirmed by the user. At600seconds it logs the elapsed
interval and requests another complete frame. Cutscene/title time is excluded.
`summarize_long_probe.py trace-<label>` generates summary.json. FPS data include
capture/logging overhead and are not a clean performance benchmark.

`test_long_probe.py` checks disabled mode, a shared writer across translation
units, concurrent event producers, anomaly requests and exact binary payloads.
Build identity/config/bank proof: session-20260930/long-probe-build-proof.json.

## FPS experiments and placement resource types (30 September)

The launcher now defaults to primary monitor1 while the user is away; `-Monitor N`
overrides it and records the actual number. `-Mode timing` writes lightweight
per-present `LABEL-frames.csv`, without GPU readbacks. Summarize with
`summarize_frame_metrics.py PATH`. Scene identity still needs a screenshot.
Stable30 has not been reached; compare matching scenes rather than whole-run
averages from different cutscene/menu segments.

Full page-watch reuse accelerated rendering but regressed Vortech geometry.
The watch is now off by default. `-StaticTextureWatch` enables experimental
reuse for CPU textures sampled only by PS; VS animation textures and all
buffers still hash current bytes on every use. `-NoMemoryWatch` disables it;
`-NoDrawArena` uses separate constant uploads for a controlled comparison.
Tests: `test_memory_watch.py`, `test_draw_upload.py`, `test_buffer_variants.py`.
Their models cannot prove real page-fault/GPU behavior.

Title idle also corrupted with the watch disabled, and with the draw arena
disabled. Found a separate TU23 contract error: the GetResourceType hook
returned host buffer kinds6/7 for borrowed placement headers of type1/2.
XGOffsetResource83F9B640 consequently wrote a VB's header+32 instead of
updating its base at+24. Registry overrides now apply only to owned simplified
headers; borrowed buffers/textures use the original leaf GetResourceType.
`test_placement_resource_type.py OUTPUT` executes the actual generated
GetResourceType/XGOffsetResource bodies and native hook, reproduces the old
wrong-field write, checks borrowed VB/IB and texture dimension distinctions,
and preserves owned-header behavior. The user subsequently confirmed five
minutes on the title before Enter without the old giant polygons. This is a
narrow regression check; the cutscene still has missing arms and strong blur.

`XXH_INLINE_ALL` uses the application's existing x86-64-v3 target for full
content hashes. `test_inline_hash.py` compares 5000 seeded/unseeded, unaligned
cases against the actual SDK library. The 1.87x hash benchmark is not a game
FPS claim. Buffers and VS textures still hash every use.

`-AsyncCpuResources` is an experimental opt-in for BlockUntilNotBusy on known
CPU snapshot resources. Unknown/GPU-produced resources keep their waits;
completion callbacks use the deferred queue described below.
`test_cpu_resource_wait.py` verifies those
exclusions and callback ordering; real GPU validation is separate. The first
runtime probe skipped almost none of the expensive waits, so it is not a
verified FPS improvement. Per-frame CSV now attributes sync calls and time
to idle, fence, resource, callback, query begin/release, and other callers.
This attribution changes no synchronization behavior.

## Deferred completion callbacks

Addendum14 identifies TU23 callbacks82BCDE68/82BCDF30 as profiler markers.
Original InsertCallback records them without blocking the CPU. Native now
enqueues all callbacks in FIFO order and executes them only after the containing
host submission's fence completes. InsertCallback neither drains nor forces a
submission. This also retains completion ordering for unknown/UI callbacks;
there is no immediate profiler shortcut that assumes timing readers are absent.

Completion is conservative at a submission boundary. Profiler timestamps have
the granularity of the completion poll, not exact GPU marker positions. Polls
run on guest hook threads at InsertCallback, KickOff, explicit waits and Swap.
Guest callbacks execute outside the device and recording mutexes; a nonblocking
execution gate preserves FIFO across reentrant/concurrent polls. Pending
callbacks are cancelled at title shutdown instead of entering guest code on
the host UI thread. D3D12 polls compare against Plume's post-signal
`fenceValue - 1`; device-removal UINT64_MAX never counts as completion.

`test_completion_callbacks.py OUTPUT` executes actual enqueue/poll and
submission attribution functions against deterministic fences, including1000
slot generations, FIFO, reentrancy, mutex release and removal. The CSV includes
callbacks_enqueued/executed/pending. These tests and a successful native build
do not establish gameplay correctness or a30FPS result; a controlled run is
still required.

## Query storage retirement

Occlusion query BEGIN now creates a fresh host readback generation, retaining
the preceding generation through every submitted/open frame slot that may
reference it. Final Release removes the CPU lookup/header immediately and
retires host query heaps, readbacks and their completion fence the same way.
Neither operation drains all GPU work. GetData still requires completion of
that query's fence and returns the real saturating sample sum; device removal
and failed draws/readbacks remain failures. No visibility result is fabricated.

`test_query_retirement.py OUTPUT` executes the actual IssueQuery, GetQueryData,
ReleaseQuery and HostDevice::RetireResource bodies against fake fences. It checks
reuse, release before END, all-live-slot retention, refcounts/header reuse,
readiness, sums, and allocation/signal/readback failures. Real GPU validation
and its effect on portal visibility/FPS remain separate.

## Disabled DoF in the final tonemap

Filtering CoC shader CFEAC7ADB912F8A9 alone did not disable the final sharp/mip
mix. Translated PS3A47E5DDE66B42C6 blends slot0 with downsampled slot4 using
`saturate(2*(mip.a*c12.x+c12.y)-1)`; slot3 contributes squared additive glow.
When the existing depth_of_field setting is false, only that shader's immutable
host upload changes c12.xy to zero. Guest constants, exposure, bloom, LUT and
c12.zw are preserved, and enabling DoF uses the original values again.

`test_tonemap_dof.cpp` checks shader/setting/bounds gating, untouched constants,
and zero mixing over the finite fixed16 alpha range. This makes the disabled
setting deterministic; it does not prove why mip alpha previously caused blur
or repair absent character parts, portal draws, or alpha-test state.

## Offline alpha/defaults and resize repair

Native now uploads raw FLOAT RB_ALPHA_REF at shared offset556 and raw Xenos
compare at offset600, keeping the shared ABI624. RB_COLORCONTROL bit3 selects
alpha specialization bit1. The isolated compiler prepared by
`prepare_alpha_candidate.py` emits all eight compares before EDRAM scaling,
gating alpha test for shaders which actually export RT0. It preserves the
upstream behavior outside LEGO_NATIVE_GPU. Both the9290-entry corpus and118
runtime containers were regenerated, then every specialization was prelinked.
The known memexport A58EBA123EFB541C remains excluded; hash/mask coverage is
checked by `verify_alpha_archives.py`. The LOD752 candidate remains separate.

CreateDevice applies15 audited CPU-only TU23 leaf setters for alpha, blending,
and the four color masks, in descriptor-table order. Defaults come from the
actual table, with setter-address validation before any writes. In particular,
the alpha-reference default is raw0x3F800000 (1.0), not the peer's suggested0.0;
blending defaults to copy and every color write mask to15. Full Xbox queue
initialization is still bypassed. `test_alpha_test.py` executes those actual
generated setters plus the native initializer, checks968 compare cases and
five microcode/HLSL/DXIL fixtures. No claim that alpha fixes hands/portal is made.

Resize/shutdown now drain every submitted frame fence before releasing the
ring. DXGI's frame-latency wait is not GPU completion. Plume's auto-reset event
is treated only as a wakeup, always checked against the current fence target;
consumed or stale events cannot cause a hang or premature completion. Failed
ResizeBuffers restores its old references and preserves dimensions; minimized
or failed resize is retried even when native framebuffers are empty.
`test_resize_fences.py` executes the actual wait/resize bodies with deterministic
fences/events and checks all slots, removal, partial retries and backbuffer
restoration. These offline checks do not confirm real window-mode recovery.

## Offline format and scene capture verification (October4)

See native-texture-format-20261004.md. Adopted scalar/RG views now compose Xenos channel replication; DXT3A expands its four-bit alpha blocks to R8. Tests include102400 swizzle cases,6144 alpha cases and48 comparisons against actual SDK D3D12 loader bytecode without launching a game. TextureForensics prepares bounded, late-triggered full-mip/raw-backing captures for a future authorized scene. These checks do not establish a repair for LOTR stripes or blank icons. Production remains624-byte ABI; the rejected LOD candidate stays rolled back.
