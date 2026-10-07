# Native renderer contributor handoff

This branch contains an experimental C++ native D3D12 renderer and its AOT
translation tools. `LEGODIMENSIONS_NATIVE_GPU` defaults to `OFF`. This is a
development handoff, not a completed renderer or a release.

## Linux limitations

- `rexlego/CMakeLists.txt` explicitly rejects `LEGODIMENSIONS_NATIVE_GPU=ON`
  outside Windows. The native implementation includes Win32/D3D12 APIs. No
  Linux native renderer exists. On October6, Clang22 cross-compilation with an
  xwin Windows SDK linked the modified SDK runtime and native prelink tools;
  the complete game build subsequently linked with direct COFF shader-bank
  embedding. The first local native launch selected that renderer, initialized
  D3D12 and recorded draw/present activity, but its window remained black.
  Later runs verified Vorton arrival, save reload and character actions, as
  recorded below. Full campaign validation remains outstanding.
  The published v0.1.34 build ran through Proton with visible first-run setup,
  then reproduced the reported owner-thread-0x17 GPU lock wait. Dump analysis
  found that owner polling GPU completion while a pipeline worker was inside
  `vkCreateGraphicsPipelines`; the command pointer continued advancing. This
  does not establish a leaked lock or justify bypassing synchronization.
- The tracked compiler-preparation CMake uses Windows DXC `.lib`/`.dll` files,
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
It registers 20 C++ helper tests. With Python3 and a GNU-style compiler driver
(GCC or clang++), it also registers 27 production-body fixtures, for 47
checks total. These compile real renderer functions against synthetic guest
heaps and fences. They cover shader read bounds, texture residency, submission
wait failures, transactional frame-ring creation, indexed draw bounds, texture
upload failure propagation, query retirement, completion callbacks and failed
native pipeline handles. Failed resolve pipelines are discarded so later calls
can retry; rejected handles cannot reach binding or draw operations. The build
fixture verifies fresh shader-bank generation and build fingerprint dependencies.
Python fixtures use
the compiler selected by CMake; generated fixtures stay in the build directory.
With Plume initialized, an additional production factory fixture checks command
allocation failures. Unix hosts with the SDK template also register a real
cross-build producer fixture. With dotnet available, an optional extractor
path-containment fixture checks 27 archive paths. With Clang and the host LLVM
tools, an additional `binary_cache_build` fixture checks the actual CMake
COFF-bank route, archive consumption and rebuild dependencies. This configured
Linux checkout therefore has 51 checks. Run the preparer below before those checks.
Release assertions stay enabled. The workflow is
`.github/workflows/native-gpu-checks.yml`; consult its actual run result before
reporting Linux validation.

The October6 safety follow-up passed all 29 then-current checks on Fedora
with Clang22.1.8 and GCC16.2.1. The expanded configured Clang suite
subsequently passed all 33 checks, then all 34 after adding the pipeline failure
fixture. Its texture residency and submission-wait regressions rejected
the original renderer paths; the original shader parser crossed an inaccessible
guard page in the bounded fixture. The fixes validate shader reads against
guest allocation/protection boundaries, reject unreadable 2D/cube texture
sources, and require successful D3D12 fence completion before retiring resources
or reusing a frame allocator. It also validates indexed draw intervals before
recording commands, aborts draws after failed texture uploads, and stages new
frame rings before publishing any replacement resources. These are host safety
checks, not evidence that
the reported in-game hangs or rendering artifacts are fixed. A full Windows
SDK runtime link and prelink-tool build subsequently passed. All 35 remaining
host/native translation units also passed isolated COFF compilation with the
actual Ninja Windows flags after fixing public ImGui header propagation and
the native RenderDoc header dependency. This does not establish a complete
game link or a playable scene.

The complete linked bank exposed a compiler memory problem: its 84 MB numeric
byte initializer exceeded 8 GB of Clang memory and drove this 16 GB machine
into heavy swapping; the build was interrupted.
The prelink emitter now uses adjacent escaped byte strings for Clang and GCC,
with the prior numeric representation retained for MSVC's string-size limit.
The explicit compressed size excludes the string initializer's terminal NUL.
Both representations passed exact byte checks and miniz round trips; an actual
small Windows COFF compile also passed strict overlength warnings. At full
bank size, Clang still expanded the string initializer into per-byte constant
values and failed safely under a 3 GB address-space limit. Escaped literals
therefore did not solve the complete-bank compilation problem.

`tools/gpu-aot/embed_linked_bank.py` now streams the unchanged compressed
payload into a binary file and keeps the original shader tables and size
constants in a small C++ file. Host `llvm-mc` embeds the binary directly into
a read-only Windows COFF section with the existing MSVC C++ array symbol;
the C++ compiler no longer sees the payload initializer. The optional
`LEGO_NATIVE_BINARY_CACHE` route supports Windows Clang with the MSVC ABI on
x64 or ARM64, requires Python3 and host `llvm-mc`, and defaults on when those
requirements are met. Other configurations retain C++ embedding. Small
x64/ARM64 checks passed. The full actual x64 native game build then passed
with one worker and compiler/linker address-space caps of 3/4 GiB. The
measured peak RSS of the build's process tree was 710 MiB. This is build
memory use, not a runtime memory or performance measurement.
Streaming comparison also verified that the complete bank's numeric and
escaped representations have identical payload bytes, tables and metadata.
The focused emission and worker fixtures passed after the earlier complete
34-check run. The complete game link passed; native gameplay remains unverified.
The expanded configured suite subsequently passed all 36 checks in 33.26
seconds, including `binary_cache_build`.

The Plume dependency uses public commit
`e0c8871b930dc1a6544bf457f2348c456b9d7e13`; the previously recorded
`ceaeeee0` could not be fetched from its configured public remote. Before a
native renderer build, apply the tracked compatibility patch:

```sh
git submodule update --init thirdparty/plume
python tools/gpu-aot/prepare_plume.py
python tools/gpu-aot/prepare_plume.py --check
```

This preserves the target-value fence wait, failed-resize recovery and command
allocation failure propagation needed
by this renderer. The pin-checked preparer refuses conflicting local edits.
The patch lives at `tools/gpu-aot/patches/plume-lego.patch`.

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
remain 624 bytes. The reconstructed compiler subsequently built on Linux against official DXC
1.9.2609. All 9,601 unique shaders extracted from the complete base disc and
TU23 compiled to DXIL; this is offline shader coverage, not rendering proof.
Prelink DXC must match the compiler version: the older pinned DXC failed to
link these newer libraries. Matching Windows DXC then prelinked 19,064
variants from 9,532 specialization shaders, with 19,124 microcode records.
The local runtime bootstrap contains one unchanged duplicate static shader
and adds no captured-runtime coverage. Native launch must capture genuine
missing runtime shaders before that coverage can be claimed.

The complete disc, TU23 and all 30 DLC packages subsequently produced an
expanded candidate with 12,141 unique compiled shaders, 12,064 specialization
shaders, 24,128 linked variants and 24,194 microcode records. Every identity
from the earlier bank was retained. All 12,141 HLSL dumps were verified and
the production prelinker's deflate round trip passed. One DLC archive has no
shader candidates; its 8,422 named entries were still parsed successfully.
These are offline validation results; native gameplay remains unverified.

The first local native launch captured 33 complete missing shader containers.
Their section and microcode bounds were validated separately from the raw
placement fragments. One runtime vertex container has valid flags
`0x102A1111`; its low-byte flags are accepted by the actual compiler, so the
capture helper was adjusted to match that parser. The compiler rejected this
container's memexport operation. The renderer already handles the observed
copy through CPU emulation; the compiler rejection alone does not establish
that a general GPU memexport path is needed. A fresh diagnostic candidate
explicitly excluded that named container with its hash and reason, compiled
all 32 included containers, and prelinked 44 variants. The next native run
showed a purple brick landscape and cloudy sky after confirm input, then
returned to black output after scene progression. That run captured 36
additional complete containers. The expanded candidate compiled all 68
included shaders and prelinked 94 variants from 47 specialization shaders;
its union index contains 24,316 records. Every prior runtime shader hash and
index row was retained. Those source artifacts were installed with rollback
copies, and the rebuilt native app reached the opening cinematic and title.
Seven further menu captures produced a strict 75-shader candidate with 106
linked variants. Its rebuilt app advanced through safe-zone settings, DLC
management and New Game, then wrote fresh Game 1 save files. Save reload and
character movement have not yet been verified. Missing controller icons and
broken-looking dark geometry remain visible; cinematic output is not renderer
correctness or playable-game proof.

The menu compiler also encountered four valid containers without an optional
CTAB reflection table. The tracked `xenosrecomp-empty-reflection.patch` selects
a static empty table instead of aliasing the container header or failing an
assertion. Four absent-table fixtures and one present-table fixture compiled
successfully, with byte-identical outputs to the previous release compiler.
`prepare_native_compiler.py` applies this patch to the reconstructed source.

An audited native run completed 3.3 million unchanged-texture checks and
2.98 million unchanged-buffer checks with zero mismatches before shutdown.
These checks still hash every audited cache hit and do not measure the fast
path's performance. That run captured three more complete shaders during New
Game progression. All 78 included shaders compiled and prelinked successfully;
the installed source candidate retains every previous identity and the same
explicit startup-copy exclusion. Its native rebuild passed at 759 MiB peak
build-tree RSS. The rebuilt game loaded Game 1 and accepted Batman, Gandalf
and Wyldstyle on the requested pad regions, progressing through the Gotham,
Middle-earth and LEGO Movie cinematics toward Vorton. Missing scene shaders,
dark output and high frame times remain; character movement is unverified.
No later save checkpoint was observed. That capture run produced 111 complete
containers. All 110 included shaders compiled, with 172 linked variants and
24,395 union-index records; the native rebuild passed at 754 MiB. The next
controlled run enables the existing stencil candidate and captures late UI
draws. Coverage remains partial. The failed 33-container candidate is preserved.

CPU-only draw diagnostics can be triggered with
`LEGO_NATIVE_DRAW_TRACE_TRIGGER`, without GPU readback. An empty trigger selects
the first 64 draw attempts in the next frame; `skip=160` selects later draws.
Output is limited to one frame and 1 MiB. Reads of device fields, declarations,
indices and vertex samples validate committed readable guest spans. An optional
`LEGO_DUMP_TEXTURE_UPLOADS_PIXEL_SHADER` accepts exactly 16 hexadecimal digits
and restricts the existing bounded texture-upload dump to that shader, allowing
UI atlases to be inspected without filling the quota with background textures.

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

The detached native app currently has no `OnCreateImmediateDrawer` override.
The SDK consequently does not initialize host overlays in this route: settings,
console, achievements, mods, cheats, and the host Credits/Quit prompts are
unavailable. Use `main_menu_extras = false` to keep those prompt-dependent entries
out of the guest main menu. Guest title/story rendering and controller input
remain separate; Alt+Enter and the window close action still work.
SDK message boxes automatically choose the focused button without a drawer,
and SDK keyboard prompts return default text instead of displaying a prompt.

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


### October 7: TU23 alpha-reference repair, before live verification

Title-menu controller glyphs use an intact BC1 atlas, but the native renderer
read the unrelated float shadow at device `+10620` (default `1.0`) as ALPHAREF.
The actual patched TU23 image proves render-state index 25 uses setter
`83FB8260`, default integer `0`, and stores a normalized float at `+10500`. Its
constant at `82005D78` is `3B808081`, float32 `1/255`; descriptor index 59 and
setter `83FB92B8` belong to the separate `+10620` state. Both shader-constant
and pipeline reads now use `+10500`, and native initialization invokes the
verified integer-reference setter. Mesh and bounded draw diagnostics agree.

The headless loader read only selected words, without guest execution. Actual
XEX/XEXP, loader, selected-word and generated-source provenance is recorded in
`.local-testing/reports/TU23-alpha-reference-byte-proof.json`. The production
regression executes original leaf setters, native default initialization,
original gameplay alpha setup, both renderer read expressions, all compares
and opaque/transparent icon cases. Correct reads pass; an isolated fixture
using the original wrong offset compiles and fails the intended reference
assertion. Evidence: `.local-testing/alpha-reference-state/verification.json`.
The portable GPU suite passes 38/38 in 34.06 seconds. Live icon validation,
dark geometry, performance, controlled character movement and campaign
completion remain outstanding. The pre-fix viewport run reached the Gandalf
cinematic and is preserved in `native-runtime110-viewport-evidence`; it does
not establish a new story checkpoint.

Live follow-up: the bounded native rebuild passed at 752.8 MiB peak owned-tree
RSS. After staging and relaunching with the same watch/stencil/viewport flags,
`native110-alpha-select.png` shows the previously missing green A and red B
title-menu glyphs. This verifies that specific repair. Dark planet debris
remains; the first 64 scene draws were captured separately for diagnosis.


### October 7: controlled culling, timing and resource profiling

The next native build passed with 814.7 MiB peak owned-tree RSS. The
`LEGO_NATIVE_CULL` candidate honors TU23 low-three-bit face/cull state, includes
it in pipeline identity, preserves draw/query submission with empty coverage
when both polygon faces are culled, and selects the surviving stencil masks and
polygon-offset face. Actual setter/Plume/production-body fixtures pass. Live
title icons remain correct, but culling has not established a repair for dark
title debris. Five opt-in constants timing phases distinguish shadow/shared,
upload, descriptor/root binding, textures and framebuffer restoration.

An owned five-second kernel-perf profile found framebuffer invalidation and
vertex upload costs on the renderer thread. It is confounded by CTest overlap
and verbose Proton exception/unwind tracing; it does not explain the entire
frame gap. The launch helper no longer forces verbose Proton tracing: one run
produced an 808 MiB trace. Native logs remain enabled, and
`LEGO_NATIVE_PROTON_LOG=1` restores that explicit diagnostic.

Framebuffer invalidation now maintains a weak index of current framebuffer
owners instead of locking every sampled texture. Recording serialization
prevents creation/invalidation races; attachment matching and fence retirement
are preserved. The actual production fixture passes, including 10,000
non-owners, all color/depth dependencies, stale generations, failure, reset,
retirement and concurrent creation. Removing the invalidation recording lock
fails the intended race assertion. The GPU suite passes 41/41 in 51.17 seconds.

SDK TimerQueue has a private restart-only blocking-wait candidate, enabled by
`REX_TIMER_WAIT_BLOCKING` presence. Default spinning remains unchanged. The real
TimerQueue/WaitItem/Disruptor fixture covers publication races, deadlines, empty
shutdown, recurring timers, backpressure and cancellation/callback lifetimes.
The SDK production regression suite passes 10/10 in 36.18 seconds. Wakeup
lateness and actual installed candidate behavior remain unverified.

The Vorton placement gate was resolved by moving Batman to the center,
Gandalf to the left and Wyldstyle to the right. The arrival cinematic advanced,
the player HUD appeared, and bounded keyboard input moved Batman forward.
The real save files changed immediately after placement; persistence through
a later reload remains unverified. This is the first verified native player
control, not campaign completion. The scene produced 13 additional complete
shader captures. The strict candidate compiled all 123 included containers
from 124 observed files, preserving prior coverage, with 196 linked variants
and 24,421 union-index entries. Only the previously proven CPU pool-copy
container is excluded. The candidate was promoted while the owned game and
Ninja were idle; the next app build and live staging remain pending.

The owners/timer native build passed with 944.5 MiB peak owned-tree RSS. The
current Vorton run still used the older staged binaries. Its exact 600-frame
scope includes arrival and gameplay, with 52.79 ms median intervals and zero
resource synchronization time. That scope does not justify an ASYNC speedup
claim; buffer-window benefits need a matched gameplay measurement. Full
campaign, visual correctness and performance goals remain outstanding.

### October 7: Vorton rebuild and failed shader bindings

The 123-container native build passed with 744.0 MiB peak owned-tree RSS and
was staged with the newly built SDK runtime. The focused culling, diagnostic,
pipeline-failure and new shader-binding production fixtures pass (4/4).
Failed nonzero vertex or pixel bindings now preserve the requested address and
reject draws before pipeline-cache lookup; genuinely null pixel bindings still
permit depth-only passes. Removing either guard reproduces stale cached
pipeline submission in the fixture. Live Vorton reload remains pending.

The actual TimerQueue latency fixture measured 1,000 one-millisecond recurring
callbacks per mode on both Linux and Windows/Proton. Linux blocking waits used
1.1% timer-thread CPU versus 65.0% spinning, with lower observed lateness.
Windows/Proton used 10 ms process CPU versus 540 ms for spinning, but blocking
p99 lateness was 1.15 ms versus 1.06 ms, and maximum 2.24 ms versus 1.21 ms.
No callback ran early. Windows measurements overlapped game startup/menu load;
matching MSVC/UCRT DLL hashes were verified. Zero reported blocking thread CPU
reflects accounting precision, not literal zero work. This supports testing the
opt-in, not a frame-rate or default-on claim.

### October 7: tutorial coverage, first-use depth and capture reuse

Native123 restored the Vorton arrival save and verified more tutorial actions:
Batman used the grapple and Batarang, Gandalf raised the stairs and removed the
crystal, and Wyldstyle climbed the bar wall and broke the object. These actions
and their screenshots are recorded in
`.local-testing/reports/campaign-e2e-progress.json`. The gateway has not been
rebuilt, no subsequent checkpoint has been verified as persisted, and Wizard
of Oz has not been entered. Campaign completion, crash-free play, visual
correctness and acceptable performance remain unverified.

The buffer-window trial is not a valid performance comparison. Its startup
skipped the introduction earlier, and the depth atlas at `AC585174` had never
received a host resolve. The baseline had four earlier host resolves. In the
selected 300-frame scopes, the trial rejected 27,522 texture uploads at slot15;
the baseline rejected none. Attempted draw counts consequently hide different
submitted work. This establishes different depth authority and startup order,
not a speedup or regression caused by `LEGO_NATIVE_BUFFER_WINDOWS`. Evidence:
`.local-testing/reports/native123-depth-atlas-first-use-investigation.json`.

Previously, a borrowed guest depth texture could only be sampled after a host
resolve. The renderer now uploads supported first-use `k_24_8` guest depth into
an R32_FLOAT sampling mirror, using the SDK's endian conversion, UNORM24 unpack
and intrinsic RRRR mapping. It refreshes the descriptor after successful upload,
retains host-resolve precedence, and refreshes guest content after invalidation.
Unsupported depth formats remain rejected. The production-body fixture covers
the actual 960x3840 tiled rectangle, generation changes, source bounds,
allocation/map retries and descriptor refresh. Its host retirement and
invalidation notifications are simulated boundaries; it does not prove live
image correctness.

Missing placement-shader captures now deduplicate the validated complete
container and stage instead of the guest object address. Reusing an address
for different valid shader bytes therefore produces a new capture. Unchanged
contents remain deduplicated, malformed containers do not reserve a slot, and
the process-wide placement budget remains 256 containers. The production-body
fixture checks exact section bytes and archive markers, stage separation,
concurrent duplicate requests and the budget. Restoring address-based dedup
fails the recycled-address regression. Dump I/O is simulated in this fixture;
file persistence is not its claim.

The Vorton tutorial candidate includes all 136 selected runtime containers:
57 vertex and 79 pixel shaders, with 111 specialization shaders, 222 linked
variants and 24,447 union-index records. It preserves every included identity
from the prior 123 candidate and adds 13. The only excluded observed container
remains the proven startup CPU pool copy; the snapshot contains 137 observed
containers. This is partial captured coverage, not complete campaign coverage.
Candidate provenance is
`.local-testing/game-launch/native-compiler/runtime-candidates/vorton-tutorial-136/provenance.json`.

The complete native build with the depth and capture fixes passed with one
worker, 3/4 GiB compiler/linker address-space caps and 734.0 MiB peak owned-tree RSS.
Five focused production fixtures passed in 6.79 seconds: buffer allocation
diagnostics, shader binding failures, capture dedup, texture binding failures
and depth upload. Staging with `--no-seed-content` also passed, preserving the
existing game content and saves. Build evidence is
`.local-testing/reports/native-runtime136-depth-capture-build.json`; staged
binary hashes and current bank hashes are recorded in
`.local-testing/reports/native-aot-complete-manifest.json`.

The 136 retry was launched with texture/buffer watches, timing, stencil,
viewport and culling enabled; asynchronous CPU resources, buffer windows,
blocking timer waits and verbose Proton logging are disabled. Its buffer
allocation diagnostic selects guest `849A4090`. Live136 Vorton validation
remains pending at this handoff. The successful build and staging do not
establish that the depth repair is visually correct or that later missing
campaign shaders are covered.

### October 7: staged 152 and bounded missing-shader identities

The 136 fast-skip run proved the first-use depth path in the running game. Guest
`AC585174` uploaded 14,745,600 bytes of tiled depth and the following slot15
fetch used its new descriptor, with zero prior atlas resolves. The bounded
proof prefix contained no texture or submission failures; see
`.local-testing/reports/native136-depth-fastskip-live-proof.json`. Later tutorial
scenes still requested shaders absent from that bank, so this result establishes
the depth fix, rather than complete scene or campaign coverage.

The staged 152 runtime candidate contains 65 vertex and 87 pixel containers,
125 specialization shaders, 250 linked variants and 24,479 union microcode
records. Its 153 observed containers include one explicitly excluded startup
CPU-copy shader. All earlier candidate identities and index rows remain present.
The complete candidate provenance is
`.local-testing/game-launch/native-compiler/runtime-candidates/vorton-batmobile-after136-20261007/provenance.json`.
Missing placement captures now emit their exact container and physical hashes
once per newly validated stage/container identity, before an existing dump file
can suppress writing. Hashes describe the same byte snapshot. The 256-container
budget remains in force.

This build also includes the opt-in nonindexed buffer-window extension and the
diagnostic texture-authority signature cache. The latter preserves selected
views and emitted event payloads while skipping repeated global dedup checks.
These tests establish bounded behavior, not a gameplay speedup. The earlier
window trial used different depth authority after a different intro sequence;
it does not establish a causal performance or visual effect of the window flag.

Four focused production fixtures passed: nonindexed buffer windows, shader
capture deduplication, first-use depth upload and texture-authority probes.
The configured portable target now has 48 checks; this focused run is not a full
48-check result. The native152 bounded build passed at 1,245,569,024 bytes peak
owned-process RSS, with three build workers and one prelink worker. Its report
is `.local-testing/reports/native152-renderer-build.json`. The staged EXE SHA256
is `ef7f4e3b7415ffc1897cfa0b5945a5875c2780b5d03df1fe5614acbf36dab678`.

Root launched the fresh 152 baseline with windows, asynchronous uploads and the
blocking timer adapter disabled. Its live validation is pending at this record.
Tutorial progress already includes the Batman, Gandalf and Wyldstyle subtasks
recorded in `campaign-e2e-progress.json`; neither a completed gateway checkpoint
nor the campaign is claimed here.

### October 7: source-only epoch cache and development shader packs

The memory-watch epoch cache passed its actual SDK-body modes and independent
review. Its evidence is `.local-testing/reports/native-memory-watch-epoch-cache.json`.
It was not part of the staged 152 performance run, so no live gain is claimed.
The original 48 portable checks passed with two compatibility adaptations; the
subsequent memory-watch, precompiled-pack and QuadList registrations bring this
checkout to 51 checks. Those additions have focused results, rather than a new
full 51-suite result.

The optional additive shader-pack loader is disabled by default. Its actual
archive and recovery fixture passed 24 malformed packs, 672 truncated inputs,
64-pack lifetime and file-size bounds, concurrent pointer-lifetime checks,
unchanged skip-list generation refresh, failed placement/owned-resource recovery
and intentional-null bindings. A provenance-bypass negative control failed as
intended. An offline exporter compiled real generated tables and its 16-shader
152-minus136 delta passed the production parser, including real prelinked DXIL.
That smoke artifact adds no coverage to native152, which already contains those
shaders. The implementation and exporter passed independent review; no live
hot-load result is claimed. See `.local-testing/reports/native-precompiled-hotload.json`.

The actual 152 window run later captured three new pixel containers at frame 20920.
Each exact emitted container/physical identity recomputes from its captured bytes
and matches one physical-section row in the strict155 candidate. The identity
proof is `.local-testing/reports/native152-misses-to155-index.json`. Root retains
152 as the compiled baseline for the first additive-load test. Source review and
offline compilation do not establish correct rendering in those later scenes
or completion of the gateway, first Oz checkpoint or campaign.

### October 7: controlled experiments and reproducible SDK publication

The current approach freezes executable/runtime/compiler identities, loads the
same private checkpoint through normal menus, and separates diagnostics from
cadence sampling. All 18 ordered additive packs passed real export/parser
checks and were accepted by both the `00dc0ee8b130f5c1` and
`8c30655312455dbb` live native builds. Acceptance is not a visual or campaign
correctness claim.

The bounded `00dc` guest-state ledger recorded 67,842 draws over 120 contiguous
present intervals, with no dropped draws. Adjacent layout/table requests were
equal 98.70% of the time; pipeline requests were equal 34.33%. Helpers,
compute/ray work, and external command mutations are outside that ledger, so
these counts alone cannot justify suppressing backend calls.

Plume now has an opt-in, actual-handle pipeline/root-signature cache. Tests
cover graphics/compute/ray transitions, alias metadata, recording resets, and
explicit external-state invalidation. Tables, constant addresses, barriers,
fences, and retirement are not suppressed. The public dependency pin remains
unchanged; the tracked additive patch and verified preparation step define the
overlay. Cache activation requires exactly `PLUME_D3D12_STATE_CACHE=1` and the
backend reports its actual mode once per process.

Same-binary `8c` testing retained all four windows, including two rejected
cache-off windows with texture streaming. The accepted off/on windows measured
15.84/16.59 FPS, but mean draws also differed (578/545). That 4.75% cadence
difference is not an attributable cache gain. Defaults remain off. Evidence:
`.local-testing/reports/native-8c-state-cache-comparison.json` and
`native-8c-state-cache-decision.json`.

The memory-watch validator now uses the SDK's existing bounded readability
predicate instead of enumerating the remainder of an allocation. Actual-body
tests match the old validity result for aliases, holes, protections, endpoints,
overflow, and 2,000 random spans. A 16-byte validation inside a 512 MiB
allocation reads one page-table entry instead of 131,072. Existing write-watch
invalidation tests also pass. This establishes less validation work, not an
observed gameplay FPS gain.

SDK changes are published as separate local commits through `b71efb4`: atomic
GPU interrupt publication/counters, shader-cache bounds, failed-dispatch TLS,
prelaunch quit handling, vblank-before-processor shutdown, explicit interior
body entries, graph indexing, verified SIMDe preparation, platform/include
integration, and final-worker singleton lifetime. Concurrent legacy-order
controls detect the two shutdown defects. Arbitrary guest-thread stragglers
and full runtime teardown remain outside those fixtures.

The five game `body` mappings require the new codegen feature. The old native
CLI predates its later static-library/producer rebuild, so it cannot establish
current-source generation. A fresh isolated CLI and generated-code producer,
followed by genuine game codegen and matched consumer/plugin builds, are the
next integration gate. Expected vendor overlays must remain explicit in
provenance; old frozen binaries do not become clean builds when source commits
are created.

Native Linux/Vulkan infrastructure exists, but this native renderer still needs
window/surface handling, genuine SPIR-V archives, three-address push constants,
resource copies, dynamic state, and real query/readback support. An asset-free
GPU ABI prototype precedes any full backend port. A matched SDK PM4 route is
the planned local visual reference; it must preserve CPU-visible writes and
settle shader compilation. Neither route has completed the campaign. No level
completion or reloadable Oz checkpoint is established by this record.
