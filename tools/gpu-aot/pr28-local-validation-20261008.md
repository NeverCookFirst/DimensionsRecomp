# Local PR28 validation, October 8, 2026

This is an isolated Windows native D3D12 candidate. The owner tested the first
build and found an overexposed menu; the corrected build has not been launched.
No changes were pushed and no pull requests merged during this validation.

## Exact inputs

- DimensionsRecomp PR28: `674975c60901147b07bd0f542453c9552a4699d2`.
- Its SDK integration pin: `2897c31b1407dd96480dc25965f8ee7807f00e0f`.
- SDK PR2/3/4/5 changed production files match the integration pin byte for byte
  as Git blobs. Their independent test registration lists differ.
- Windows x64 Release, Clang 20.1.8, MSVC 14.44.35207,
  Windows SDK 10.0.26100.0, Ninja 1.13.2.
- TU23 is applied by the new SDK loader to the owner's local Default.xex input.
- The PR's shader compiler patch was rebuilt against XenosRecomp
  `339af41df2c23dbe3256c1c377716b81a0e0fe6b`.
- Shader inputs are the owner's existing local main bank and 255 runtime
  containers. AlenHay's additional private capture corpus is absent here.

## Local changes needed for this Windows candidate

These changes are additional to the PR head; this candidate is not an unchanged
build of PR28.

- Link the installed SDK's `rex::renderdoc` exported target, with source-tree
  `renderdoc` fallback. A bare library name did not select the imported target.
- Make the Python/C++ fixtures tolerate embedded Python's isolated import path,
  predefined `_MSC_VER`, Windows `min/max` macros, forward-slash CMake paths,
  CRLF metadata, MSVCRT symbol names, and Ninja's fresh-build producer counts.
- Register the POSIX `resource`/`preexec_fn` memory fixture only on UNIX.
- Fresh generation without further overrides produced 165131 registrations,
  dropping 136 original entries and adding six. It emitted an unresolved
  conditional backward branch from 0x8371921C to 0x837191F8.
- The local game config explicitly restores those 136 known entries. The
  original generated body proves 0x837191E8 spans 0x50 bytes, so that extent is
  explicit. With these declarations, regeneration preserves all 165261 original
  address/symbol pairs, adds none, and emits no unresolved branch markers.
  All 136 restored bodies retain their original guest instruction comments.
  This is a verified workaround; the underlying discovery discrepancy is not
  diagnosed or fixed in the SDK.
- SDK dependency symlink stubs were materialized in this checkout only. All
  dependency Git revisions are pinned. The SDK's SIMDe preparation script and
  the game's Plume preparation script were applied to isolated dependencies.
- A private local manifest uses absolute game input paths and a relative
  generated output path. Generated code, shader banks, game inputs and runtime
  binaries are ignored and are not included in commits.

## Offline results

- SDK: 1674 registered tests, 1670 passed, four explicitly skipped. No failures.
  The suite contains 1458 PPC and 216 unit tests. Linux-only regression fixtures
  were not registered on this Windows host.
- Renderer: all 61 Windows checks passed in one full run after relocation
  and the binary-cache fixture correction. POSIX-only checks
  were not run here.
- Main shader compilation: 9290 valid shaders; the known
  A58EBA123EFB541C memexport container remains unsupported. Its compiler failure
  is recorded rather than treated as a new regression.
- Runtime shader compilation: 255/255 containers.
- Both raw banks preserve the old shader identities and specialization masks.
- Every required specialization subset is prelinked: main 18444 variants,
  runtime 458. Microcode index retains 18993 records.

## Evidence and pending validation

Local logs and JSON proofs are in
`E:/Claude/LEGO Dimensions/rexlego/out/pr28-validation`.
The candidate source is in
`E:/Claude/LEGO Dimensions/rexlego/out/pr28-validation/source`.

The Windows native target linked successfully. `candidate-ready.json` records
static DLL import checks, matching installed-SDK runtime/plugin hashes and
corrected embedded native fingerprint `ef072b62fd02e827`. The original executable,
runtime, GPU plugin and config hashes are unchanged. The staged executable is
`E:/Claude/LEGO Dimensions/rexlego/out/pr28-validation/game-build/legodimensions.exe`.

Gameplay, portraits, prompts, LOTR textures, stability and performance have not
been tested for this candidate. No visual fix or FPS improvement is claimed.
The existing native path still requires Windows D3D12. The staging configuration
uses monitor 3, 1920x1080 output and depth of field off; these settings do not
establish actual 1080p internal rendering or stable 60 FPS.

## First menu test failed; native shader preparation correction

The owner launched the first candidate and supplied an overexposed, incorrectly
colored menu screenshot. The game log confirms native routing and the DoF
correction were active. The game was then closed by the owner.

The first banks were compiled without XENOS_RECOMP_LEGO_NATIVE_SCALE, so DXC did
not enable LEGO_NATIVE_GPU. That bypassed native output scaling, alpha and
texture corrections. This was a local build-preparation error; identity/mask
coverage checks alone did not detect the incorrect compiler mode.

prepare_native_compiler.py now writes the native define unconditionally into
its isolated DXC source and records native_gpu_define_forced=true. The generic
XenosRecomp checkout and original installed banks remain untouched. The
corrected compiler is exercised with the environment flag absent. All 255
corrected runtime DXIL payloads and all 9290 main DXIL payloads match the previous
native banks byte for byte. All 18444/458 prelinked specializations are present.
The failed executable, screenshot, log and configuration are preserved locally
under pr28-validation/failed-nonnative-shaders. Further visual validation is
pending; no replacement game instance has been launched by the agent.

## Relocation to E: complete

The owner requested removal of the C: checkout. The source, SDK/dependencies,
generated C++ and local shader banks are now under
`E:/Claude/LEGO Dimensions/rexlego/out/pr28-validation/source`.
Git worktree registration was repaired; the old C: worktree folder is gone.
The executable was rebuilt from the E: source and all 61 renderer checks passed
from E:. The SDK binaries were reused byte for byte from the verified install;
`sdk-build-e` is the newly configured E: SDK build tree. Old build directories
are retained on E: as historical evidence. The Codex attachment tool does not
register this manually relocated checkout as a managed worktree; Git itself
lists the repaired E: worktree normally.

The corrected executable is staged at the same `game-build/legodimensions.exe`
path. Monitor 3 and DoF off remain configured. Static import checks pass, the
fingerprint is embedded, and original build hashes are unchanged. The game
remains closed. A menu retest must precede any further visual/performance claim.

## Follow-up: shadow state candidate

The owner's subsequent Vorton screenshots show restored Sonic portraits and
substantially improved colors, with large platform/floor cast shadows absent
in the native image. Scene/camera differences prevent a pixel-perfect comparison.
The saved native log reports 1280x720 render targets/viewports and a 1920x1080
swap chain. The settings request 60 FPS, with host framerate_limit=240; achieved
FPS has not been measured. Native 1080p is still outstanding.

This local candidate enables guest stencil and face-culling state by default.
Stencil initialization previously ran only with LEGO_NATIVE_STENCIL, while draws
otherwise stripped all stencil fields from RB_DEPTHCONTROL. Initialization and
draw state now both use the restart-only LEGO_NATIVE_NO_STENCIL comparison
switch. LEGO_NATIVE_NO_CULL restores the preceding unculled behavior. Existing
positive switches are redundant; the viewport candidate remains opt-in.

Stencil reference now participates in the cached pipeline identity along with
the read/write masks. The pinned Plume D3D12 constructor leaves stencilRef at
zero, and its checkStencilRef can overwrite an earlier raw OMSetStencilRef.
The native layer therefore initializes this field before publishing a pipeline;
no SDK or Plume dependency source is changed. The early raw binding is removed.
Two-sided reference/mask differences when both faces survive remain unsupported.

The Windows native executable builds with embedded fingerprint c2f1d40a1b07ba67.
All 62 renderer checks pass, including a new production-body stencil pipeline
test that executes Plume's actual reference submission for all 256 references.
Removing the reference handoff fails that test. The separate private-image
fixture passes the original TU23 stencil leaves, defaults and depth rebind;
it accepts an explicit image path and locates the rebind independently of
generated translation-unit numbering. These are offline state checks, not GPU
pixel or gameplay validation.

The candidate is staged at the same E: executable path, monitor 3 and DoF off.
Shader banks, SDK binaries and original baseline executable/config are preserved.
No game was launched, no PR was pushed or merged. Owner Vorton comparison is
required before claiming shadow restoration or proceeding with a combined
resolution change. Private evidence is under pr28-validation/shadow-check.

## Depth-only coverage repair after failed stencil retest

The owner reports that f7814aa still has no shadows. Its log confirms stencil
draws are enabled, including the two-sided C07E07B3 state; stencil defaulting
alone did not restore shadows. No game is running at investigation time.

DispatchDraw accepted a depth-only framebuffer but bounded its scissor using
the absent RT0's zero width/height. DecodeNativeScissor consequently returned
an empty rectangle. The raster bounds now select RT0 when present, otherwise
the depth attachment. The screen-space viewport and constant target selection
use the same fallback. Saved October 3/4 gameplay traces prove depth-only draws
occur (RT=0, PS=0, nonzero depth); they do not prove this is the only shadow bug.

The actual draw-section fixture now exercises a 960x960 depth-only pass,
requested subrects, both-face culling, ordinary color-target precedence and the
old zero-extent negative case. Scissor/viewport/culling/shader-binding checks
pass. The full renderer run passed 61/62 before a depth-upload harness compile
error was corrected (missing include for the new bounded log set); that check
then passed both directly and via CTest --rerun-failed. No test remains failing.
Depth-atlas logging deduplicates source/destination/region rather than exhausting
its budget on repeated early scene-depth copies. Depth-only draws log their
host extent and effective scissor for the first 16 occurrences.

Native Release fingerprint 647874d7f0f65636 is staged on E:. No shader-bank,
SDK or Plume edits, no push/merge or game launch. Native internal resolution
remains unchanged. The private Run-shadow-diagnostic.cmd launcher enables
bounded captures and a draw trigger without changing persistent settings;
it was parsed but not executed. Owner Vorton readiness is requested so actual
depth/atlas pixels can be captured. Shadow restoration is not yet verified.

## Owner and screenshot confirm Vorton shadows

The owner subsequently reported “shadows are finally back”. The assistant's
window capture independently shows large platform/floor cast shadows in Vorton,
with Sonic's portrait visible. The running native fingerprint is
647874d7f0f65636, corresponding to the depth-only repair in 2bf6bef. Its log
records nonempty depth-only coverage and 960x960 transfers into atlas AC585174
at destination offsets (0,0) and (0,960). No live diagnostic run directory was
created, so this is a window screenshot/log comparison, not a GPU frame capture.

The verified executable, matching SDK DLLs, configuration, screenshot and logs
are preserved privately under shadow-check/verified-vorton. This validates the
observed Vorton shadow regression only. Other worlds/cutscenes, AMD, true native
1080p and clean performance measurements remain outstanding. No push or merge.


## Native 1080p scene storage candidate (awaiting gameplay verification)

The native C++ renderer now separates logical guest extents from physical host
extents. Exact full-scene 1280x720 surfaces allocate 1920x1080 storage by default.
BeginTiling promotes 1280x512 surfaces to logical1280x720/physical1920x1080.
Guest headers and viewport clipping retain logical coordinates. Raster viewports,
scissors, explicit clears and resolve regions map to physical coordinates;
the 512/208-row split maps to768/312 rows. Pixel-space shader constants retain
logical dimensions. Full-size host resolve destinations, float sampling mirrors
and raw depth aliases carry the same physical extent through postprocessing.
Copy/Load resolves reject mismatched physical coverage instead of cropping it.

CPU assets, cube/volume textures, portraits, 960x3840 shadow atlases and existing
640x360/320x180 effect targets retain their original dimensions. This is a
full-resolution scene candidate with lower-resolution effects, not a claim that
every render pass is1920x1080. Set LEGO_NATIVE_720P to restore the previous
allocation policy for comparison. No shader ABI/bank, SDK or Plume changes.

Resizing publishes a complete texture/view/descriptor transaction, invalidates
cached framebuffer references, retains old immutable descriptor slots, and
retires old raw/resolved/sampling storage at the host submission fence. Failed
allocations leave the old binding intact. CPU uploads cannot mark a partial
720p depth upload into an invalid1080p mirror as authoritative. Borrowed raw
depth aliases restore guest-sized storage before returning to CPU authority.

All64 portable renderer checks pass on Windows. New checks execute production
resize code with allocation/registration failures and retained storage; render
extent math checks tile continuity, unchanged atlases and scale mismatch
rejection. Existing production draw scissor and guest viewport fixtures cover
physical1080/logical720 separation. Full native Release build succeeds with
fingerprint6c8faafca2102bc7. The staged configuration uses monitor3 and DoF off.
Internal present dimensions are logged before the swap-chain copy. An opt-in
manual-only GPU snapshot mode preserves the bounded readback budget until the
owner reaches the chosen scene and a capture-next-frame trigger is written.

No game was launched by the assistant. The verified720p shadow build remains
preserved at shadow-check/verified-vorton/build. Actual1080p resource captures,
Vorton/LOTR visual parity and frame timings are pending a manual gameplay run.
No stable60FPS, AMD, additional-world or visual correctness claim is made.
No PR was pushed or merged; the original baseline hashes remain unchanged.


##1080p menu regression: guest pixel-position correction

The owner reports ghostlike objects and textures showing through menu geometry.
The running fingerprint6c8faafca2102bc7 matches the1080p candidate. A saved
window capture reproduces the distorted logo/portal geometry.56 completed
present-input DDS headers independently confirm1920x1080 internal textures.
The owner screenshot, assistant window capture, draw trace, logs and timings
are retained privately under native1080-check/live-20261008-140303. The game
was closed normally after capture and not relaunched by the assistant.

Inspection found that the recompiler initializes the pixel parameter register
from SV_Position minus0.5 without undoing native raster scale. The SDK's
emulated DXBC translator explicitly reverts draw resolution scale after
obtaining the host pixel address. With native1080 storage, the missing
correction gives guest pixel-position-based sampling a1.5x coordinate range.
This is a concrete shader coordinate defect; its causal contribution to the
owner's complete visual regression still requires the next comparison.

The host now uploads inverse uniform raster scale at SharedConstants byte588,
previously unused padding. Default1 and legacyzero both retain scale1; full
scene1080 uses2/3. The native-only compiler preparation declares this float
atc36.w and multiplies the pixel parameter XY after removing the host half
pixel, preserving face sign. ABI624 and existing alpha, viewport, shadow-PCF
and color-output offsets remain unchanged. Physical1080 allocation remains.
Resolve helper shaders continue using physical coordinates for integer loads.

All64 renderer checks pass.50 synthetic gradient microcode/HLSL/DXIL cases
and3 unsupported-mode refusals pass with the new compiler. A new fixture
executes the actual emitted pixel-position expression across720/1080 scales,
front/back faces and legacyzero; deleting the correction fails its negative
control. DXIL disassembly confirms register36 lane3 (byte588) is loaded and
multiplied before derivatives; the shared buffer remains624 bytes. Fresh tracked
compiler preparation reproduces the tested recompiler, common header and DXC
source exactly (normalizing filesystem line endings).

Main9290 and runtime255 shader identities and specialization masks are
preserved, with all DXIL payloads nonempty. Main compilation returns2 for
exactly the known A58EBA123EFB541C unsupported memexport shader; runtime has
no failures. All payloads change after the shared-buffer declaration changes;
previous byte-identical-bank proofs do not apply to this candidate. Prelink
coverage remains18444 main and458 runtime variants. Parallel compilation is
used locally; all255 runtime payloads match serial output byte for byte.
Old candidate banks, index, executable and config are preserved separately.

The native executable rebuilds with fingerprintf4ead82ea9da0aa2, monitor3,
DoFoff. The original baseline executable/config and shared SDK/Plume checkouts
are untouched. Visual correction and stable60FPS are not yet verified. No
push, merge or release; a manual menu comparison is the next required test.


### 1080p depth-bank mapping follow-up

Owner retest of fingerprint f4ead82ea9da0aa2 still shows severe fragmentation
in Vorton. The captured screenshot is preserved privately under
native1080-check/live-20261008-175435/hub-regression.png. The game was closed
normally after capture; no subsequent instance was launched by the assistant.
The pixel-position correction alone did not establish visual correctness.

The same run logs color-to-depth EDRAM aliases at 1920x1080. Their restore
shader still exchanged fixed 40-pixel halves of 80-pixel tiles. For a logical
1280-wide surface stored at 1920, those halves occupy 60 host pixels. The
restore path now derives that half-tile width from logical and physical width,
passes it in the existing second push-constant uint, and uses it in the actual
restore shader. Logical widths must contain whole guest tiles and the scaled
half-tile must be integral. Source and destination logical widths must match.
Unscaled surfaces retain 40-pixel halves. The host rejects invalid restore
widths before binding or drawing. Shader archive banks and SDK DLLs unchanged.

All 65 registered renderer checks pass: the initial suite had 64 passes and a
new-fixture argument-parser failure; adding its standard --compiler option
made the failed check pass on rerun. The depth fixture executes the actual
restore shader coordinate expression, checks 1920-wide mapping and the actual
alias tracker with 720p and 1080p surfaces, and preserves exhaustive D24
conversion checks. A negative control using the old fixed-40 shader fails.
The pipeline fixture checks the actual push-constant value (40/60) and rejects
zero or incomplete tile widths without drawing.

The executable rebuilt with fingerprint 1cf45f6b38ade6e6. Staging verifies
embedded fingerprint, DLL imports, monitor 3, DoF off, and unchanged hashes of
the original baseline. Full-size scene storage remains 1920x1080; this build's
visual correctness and gameplay FPS remain unverified. The verified 720p
shadow build remains preserved. No push, merge, or SDK/Vulkan changes.


### Owner confirmation after scaled depth restore fix

Owner reports Vorton fragmentation fixed with fingerprint 1cf45f6b38ade6e6.
A fresh window capture is saved privately as
native1080-check/live-20261008-181500/hub-depth-fixed.png. The scene shows
continuous terrain and coherent portal platforms, without the previous large
misplaced image fragments. The present DDS header confirms 1920x1080. This
confirms the tested Vorton scene; other dimensions and stable 60FPS remain
unverified. The diagnostic instance remains open for world comparisons.


### LOTR missing shaders, water, particles and minimap follow-up

The owner reports black water, broken materials/minimap and missing particles
(P1). Current-run screenshots lotr-issues.png and lotr-black-water.png are
saved under native1080-check/live-20261008-181500. The game was closed normally
after captures and bounded read-only shader snapshots; no later launch by the
assistant. These defects remain visually unverified pending a retest.

The trace records unrecognized shaders and missing VS/PS bindings in LOTR.
Twelve complete placement containers were reconstructed from the current
resource headers and physical sections. Required header/virtual-section bytes
and physical bytes match repeated reads; neighboring bytes outside those
sections are excluded from the stability comparison. All 12 compile with the
same native pixel-scale compiler. Their exact physical and instruction
sections both resolve to their own compiled container identities through the
production lookup, with development overlays absent. No guessed shader binds.
The lookup fixture now explicitly stubs the absent overlay bank.

The runtime archive grows from 255 to 267 shaders; all previous identities,
specialization masks and 255 DXIL payloads are preserved byte for byte. Runtime
prelinked variants grow from 458 to 482; placement signatures grow from 18993
to 19017 with all old records retained. Main archive remains unchanged. Private
proofs: lotr-banks-verification.json and lotr-index-verification.json. Captured
shader bytes and generated banks remain local ignored artifacts.

The minimap destination AC3F1C7C is 512x512 with FOUR mips. Its base-level
resolve from scene-scaled 3019C000 is rejected for unequal physical coverage;
lower mips are independently written from unscaled surfaces. Scaling the whole
destination would conflict with those smaller writes, so that candidate was
removed before final staging. The final code preserves its guest-sized storage
and adds a bounded RGBA8 base-level GPU downsample. Existing equal-scale copies
and smaller mip writes keep their original paths. Other unsupported format,
slice, mip, exponent or nonuniform-scale combinations fail rather than copy
incorrect coverage. Push constants remain 16 bytes; ABI624 is unchanged.

The coordinate mapper can explicitly return separate physical source/dest
coverage. A dedicated RGBA8 helper shader uses nearest texel loads at mapped
pixel centers. The host passes uniform ratio and source offsets in the existing
push-constant words. Tests cover 768-to-512 coverage, all three smaller mip
extents, actual host constants at nonzero offsets, rejected nonuniform scale,
and pipeline allocation failure/retry for all five conversion paths. The helper
HLSL is compiled by DXC and included in the build fingerprint.

Final executable fingerprint dc7c74998b3317fb, monitor3, DoFoff. The prepared
launcher additionally enables bounded missing-container captures and on-demand
full-mip texture capture, so a failed visual retest can supply more evidence.
No SDK/Vulkan rebuilds or edits, no baseline changes, no push or merge. Water,
particle and minimap visual restoration is still pending owner confirmation;
no performance claim is made for diagnostic runs.

Final verification: all65 registered renderer checks pass on the final downsample implementation (lotr-final-renderer-tests.log). Staging checks embedded fingerprint, imports, monitor3/DoFoff and unchanged original baseline hashes. Next action is an owner-launched LOTR visual retest, including P1 particles.

### Later owner retest: water restored, broader shader coverage still missing

The owner confirms water restored on dc7c74998b3317fb, but reports black LOTR
sky/materials, vertical hub background lines after returning, and black/invisible
Year 2 shard geometry. Owner screenshots are preserved privately under
world-captures-184424. The exact diagnostic was closed normally after the shard
draw and full-mip texture captures. Shutdown logged its existing hard-exit
timeout; no claim of clean runtime teardown is made.

The run live-20261008-184424 captured 116 complete missing placement containers.
The trace records 155727 missing-pixel and 60201 missing-vertex draw events,
with overlapping events possible. All 116 containers compile and pass exact
physical/instruction lookup through the production function. Runtime383 and
index19249 retain all267 previous payloads/masks and all19017 previous index
records. Required runtime specialization subsets are present. Build fingerprint
4a0baa0d95d9bbb0 is staged on E:, monitor3/DoFoff. Rendering implementation,
water downsample, depth/shadow fixes, native1080 allocations and main bank are
unchanged. This candidate has not been launched or visually verified.

The owner then requested the earlier offline approach instead of manually
visiting all worlds. ENGINEERING-PLAN.md records missing executable-resident
shaders and incorrect name-tree/file-index association in old extraction. The
current extractor has CRC-first name resolution and a streaming all-entry mode.
Private offline-corpus work scans the installed disc, TU23 and all30 DLC
directories; extraction reads data without running the game. The existing
SDK-loaded TU23 image hash matches the recorded sampler audit
(b01649336b1f3eb6cb54cbc0e8d39a30b4ae1bdcfed2226da68f32b0463ff3ce).
Image extraction finds483 unique structural containers. TU23 all115956 entries
are scanned without decode failures, yielding7168 unique containers. The disc
scan visits every indexed entry but explicitly reports sparse installer ranges;
it is not complete readable-content coverage. DLC scans and compilation are
in progress. Structural extraction is not visual correctness or crash proof.

### Whole-installation offline cache completed

All33 source groups are processed:44 archives across the disc, TU23 and30 DLC
directories, plus the verified SDK-loaded image. All483876 indexed archive
entries were attempted, with zero unvisited indices. The31874 unreadable entries
are explicitly reported in the three sparse INSTALL archives; the readable
GAME/TU23/DLC scans have no decode failures. The corpus contains39052 unique
complete source-container hashes. All116 recent captured physical sections and
instruction streams have exact origins in this offline corpus.

The native-pixel compiler processes30145 runtime/novel-source inputs and emits
30080 successful containers, adding29697 to the383-container candidate. All383
previous raw DXIL payloads and masks remain byte-identical. The65 explicit
refusals are14 memexport and51 unmapped vertex-fetch cases, recorded with their
image/archive origins in private offline-corpus/compiler-refusals.json. Failed
identities are excluded from the index. Index75372 retains all19249 old rows;
duplicate exact stage/length/signatures retain old lookup choices. All116
captures still pass exact physical/instruction lookup through production code.
Main9290 is unchanged; container counts include reflection variants.

Prelink emits59802 required specialization variants. All714 previous linked
payloads remain byte-identical. The runtime linked bank has642590532 decompressed
bytes and214211387 compressed bytes. Its generated C++ is about1.6GB, so the
existing bounded COFF embedding now supports the distinct runtime symbol too.
Metadata still compiles through the existing symbol-renaming C++ wrapper; the
compressed array is assembled into a read-only object. The current full bank's
numeric/literal representations and source/payload hashes are checked. Unsupported
COFF configurations retain the existing C++ fallback.

The65-check suite passes after the implementation change. The newly registered
runtime binary fixture also passes: actual production wrapper, exact COFF payload
bytes, successful DLL linkage and a failing missing-payload control. The66 checks
are asset-free/CPU/build evidence, not a GPU image comparison. Final build and
incremental rebuild succeed; staging verifies imports, fingerprint, monitor3,
DoFoff and unchanged original baseline hashes. Final fingerprint8e299ba49552d64b.
Private proof: offline-corpus/final-candidate-proof.json. Everything stays onE:;
no game launch, SDK/Vulkan rebuild/edit, push or merge. LOTR sky/materials,
particles(P1), shard geometry, hub lines after travel, crash freedom and FPS
remain visually/runtime unverified. Next human check is one owner-launched
LOTR/shard/return-to-hub diagnostic sequence, rather than shader discovery by
visiting every world.

### Shard texture and performance follow-up

The owner tested fingerprint8e299ba49552d64b and reported restored shard
geometry, but strongly streaked terrain and about20FPS. Run
native1080-check/live-20261008-193353 was captured and closed. The window
capture was occluded by another window; the owner's supplied screenshot and
GPU snapshots are the useful scene evidence. There was no subsequent launch.

The final200 instrumented frames average20.22FPS, median48.15ms and
p95 56.43ms, with1149 draws/frame. Median texture CPU0.59ms, constants4.70ms
(texture binding2.60ms), vertices5.90ms, buffer hash1.22ms. Median sum of
recorded GPU submission durations is10.26ms. These sums exclude CPU/queue
idle gaps and are not utilization, full frame latency or a60FPS prediction.
This run saved1291 snapshots and463464 texture-fetch-change events overall;
it is not a clean benchmark. Private summary.json preserves the evidence.

All32 late-triggered base uploads were decoded for inspection. Six rock/detail
texture mip chains (eight levels each) appear coherent; this does not prove
correct binding, UVs, sampler state or all installed textures. An exact
instruction-match terrain pixel container uses seven ordinary normalized2D
fetches, inherited filters, without register gradients or denormalized coords.
The corresponding captured vertex container was recompiled for HLSL inspection;
no semantic rewrite is justified by the current evidence. Existing draw traces
sampled position/normal/color but excluded UVs. The recorder now reserves a
separate12-sample UV budget; an asset-free production-body fixture verifies
UV capture after the other budget is exhausted. Streaks remain unresolved.

Only one missing container was captured:vs-A58EBA123EFB541C. Offline compilation
explicitly refuses vertex export register32 because it requires memexport
writeback. It is not repaired by generating a raster-only substitute. Earlier
pool-copy research and hooks_buffer.cpp already provide CPU emulation for its
observed copy operation, with original-helper fallback for unsupported copies.
The refusal alone is not evidence that this run lost particles or materials.

Manual-only snapshot mode now takes precedence over long-probe periodic and
anomaly scheduling; explicit whole-frame requests and bounded present bursts
remain supported. The selection fixture replays the production branch for
trace enabled/disabled, manual selection, burst exhaustion and automatic mode.
Index bounds use SSSE3/SSE4.1 when compiled for that ISA, with portable scalar
fallback. Every index is still read on every use; no new mutable-content cache
or invalidation assumption is introduced. Randomized tests cover both widths,
unaligned starts, tails, unsigned extrema and direct writes. The x86-64-v3 path
was also compiled/executed separately. A local synthetic microbenchmark shows
gains for240/4096/65536-index ranges and a small regression for24-index ranges;
these measurements are not game FPS. Existing draw window/base-vertex guards
are retained.

Private Run-native1080-benchmark.cmd records every-present CSV without snapshot,
long-probe, texture/shader dump or per-draw trace settings. Detailed timing is
an optional PowerShell flag for a separate comparison. Configuration is checked
for monitor3,1920x1080 andDoFoff before owner-initiated launch. The launcher is
prepared only, not run. Neither60FPS nor visual repair of terrain is verified.

Final build fingerprint87665dbffb001282 links successfully. All68 offline
checks pass in96.55s, plus the separate x86-64-v3 index fixture. Candidate
staging rechecks DLL imports, unchanged shader banks and original baseline
hashes. The performance launcher passes PowerShell parsing; launch behavior
and gameplay remain pending human testing. No SDK/Vulkan changes, push or merge.

### Offline localization of shard bands

The owner requested continued offline work rather than another launch. Saved
frame9026 supplies a useful separation: the1920x1080 normal G-buffer resolve
at guest2901079004 already contains the long bands, while the adjacent diffuse
resolve at2901078704 shows coherent terrain colour detail. The bands are also
visible in the subsequent HDR lighting resolve. This localizes the artifact
before final postprocessing; it does not establish whether normal-map sampling,
vertex tangent conversion, shader arithmetic or normal-buffer writes cause it.
All32 captured texture uploads were inspected as separate RGBA channels; the
rock normal maps' green and alpha channels appear coherent. No global UV/channel
rewrite is justified. Private uv-audit holds the original-pass PNGs, channel
montage and shader disassemblies.

The Avengers material-name matches are reference families, not identical shader
oracles. The nearest pixel family has six variants but uses a forward colour
output rather than Dimensions' deferred targets. The broad vertex-name match
also includes unrelated simple icon shaders. No Avengers assets are shipped.

The bounded draw recorder additionally captures four tangent samples independent
of its position/normal/colour and UV budgets, VS constants25..28 for secondary
UV transforms, and PS constants11/15 for normal strength/material parameters.
The production recorder fixture checks these fields after the general sample
budget is exhausted. This improves the next diagnosis; it is not a visual fix.
Rebuilt fingerprintb5782e4de7688efb passes all68 checks in the full suite;
staging again verifies imports, retained shader payloads and original baseline
hashes. This build has not been launched. The owner requested offline work.

### October9 visual retest and repeatable material diagnostics

The owner reports a stable30FPS benchmark and prioritizes visual correctness.
No scene or benchmark CSV was supplied for that report. Owner-launched build
b5782e4de7688efb on RTX4060/monitor3/DoFoff captures shard terrain without the
previous long bands in live-20261009-231558/f4069-59-present.dds. The owner
closed that instance manually; do not record it as a crash. The next instance,
live-20261009-232049, captures LOTR at f2243 and f4848: sky and water appear,
but vertical geometry streaks, black snow patches and an incorrect minimap base
remain. Sonic lying down is his idle animation, as clarified by the owner.
Particles are not globally validated by this still capture. Return-to-shard
f10867-59-present.dds still lacks the previous long terrain bands. This is
scene evidence, not proof of a general rendering repair or its cause. The
assistant closed this exact second instance normally after capture; no relaunch.

The LOTR streaks/black areas are already visible in the diffuse scene-buffer
resolve before final lighting/postprocessing. The first CPU capture selected
depth-only draws. A second request was not consumed because the recorder was
permanently complete after its first frame. It now supports up to four explicit
requests per launch, each retaining its64-draw/1MiB bound; later numbered output
files preserve prior evidence. Every request waits for the next full frame.
The production fixture covers re-arm after frame completion, skip counts,
malformed requests, preserved files and the four-request cap. Selected draws
now include actual texture addresses and all six fetch words for each bound
slot, using protected reads. The fixture checks slots7/31 independently of
change-only texture tracing. No shader/texture arithmetic is changed here.

All68 checks pass for the re-arm change (58.04s). After the additional binding
fields, the affected production recorder fixture passes again (3.42s). Build
3c59011a08a820fb links and staging verifies retained banks/imports/original
baseline hashes. It has not been launched. Next owner test is a LOTR material
capture starting later than the depth prepass, with repeat capture available
in the same process. No SDK/Vulkan edits or rebuild, push or merge.

FSR4/4.1 is not added: this machine's adapter is verified as RTX4060 in the
native initialization log. AMD's official FSR SDK page currently specifies
Radeon RX7000/9000 discrete hardware for FSR Upscaling4. The existing application
also records the visual limitation of FSR2/3 supplied with synthesized motion
and depth. A temporal integration requires validated frame inputs; it is not a
repair for missing materials or particles. Reference: https://gpuopen.com/amd-fsr-sdk/.

## October 10 transition, lava and story-level investigation

Owner tested candidate 3c59011a08a820fb in live-20261010-001529 on monitor 3.
LOTR still has vertical geometry spikes, black terrain patches and an incorrect
minimap base. Scooby-Doo shows fewer defects but lacks the expected cartoon
presentation according to the owner. Adventure Time lava flickers with black
striped areas. Level 1 foliage also flickers and shows striped gaps; its owner
screenshot reports 10 FPS. The owner identifies travel as a trigger: after
leaving worlds, hub terrain/background become stretched or blurry. A hub pad
that should spin appears static. These are open visual issues, not fixed claims.

Saved owner images and GPU captures: LOTR f3260, Scooby f5970, hub/transition
f11468, Adventure Time f14593, level 1 f17829, returned hub f20489. CPU material
requests 1-3 are populated; request 4 is empty because its selected full frame
did not reach the requested skip count. Do not claim that it captured lava UVs.
The trace has 499569 texture_fetch_changed events. One level-1 sample has
2316 draws and 62.4734 ms interval. Diagnostic readbacks/tracing were active;
this is not a clean performance benchmark. The assistant closed PID30928
normally with Alt+F4; shutdown is logged. No relaunch was performed.

Found and fixed an active-binding lifecycle gap: InvalidatePlacementShader
removed the registry entry while retaining the old g_bound_shaders program.
It now clears that active program while retaining the guest request. Both XDK
placement initializer hooks save the original resource address, invalidate,
run the guest initializer, then refresh an active request at that address.
Thus a redundant SetShader omitted by the guest cannot retain the old shader.
Unavailable replacements remain failed nonnull binds; intentional null and
owned resource behavior remain unchanged. No per-draw hashing was added.
This is a verified code defect, not proof that it caused every travel glitch.

The production-body fixture exercises same-address replacement without
SetShader, unavailable replacement, stage isolation and owned-resource guards.
It passes; removing the refresh makes the regression fail. Four affected
renderer tests pass (6.41 seconds). Windows fixture assertion reporting now
uses stderr rather than blocking on an assertion dialog during mutant tests.
Candidate ba9e37f1f6e208d7 builds. transition-audit/candidate-proof.json verifies
unchanged SDK binaries, original baseline hashes, monitor3/native1080 config
and DoFoff. Shader banks retained. Private ten-container DXIL inspection compile
passes; it was not installed. No SDK/Vulkan edits/rebuild, push or merge.

Live visual verification and clean level-1 performance remain pending. Use
Run-native1080-benchmark.cmd for the next owner-run test: it disables GPU
snapshots/material traces and retains lightweight frames.csv. Check fresh
level 1 foliage/FPS, then return to the hub. Do not start the game yourself.

October10 clean level1 benchmark: owner ran Run-native1080-benchmark.cmd,
PID21040, benchmark-20261010-010214, startup fingerprintba9e37f1f6e208d7.
Captures=false, trace=false, detailedTiming=false. After owner 'lvl1 ready',
latest600 complete rows17881..18480 average53.5535ms=18.6729FPS,
median51.8186ms, p95=68.6522ms, draws2158..2159. Saved level1-window.json.
This confirms poor performance persists without heavy diagnostic captures;
it does not identify the CPU/GPU bottleneck. Asked whether leaf flicker persists
and to return to hub for the travel-corruption check. Keep this instance open;
no new launch, no claim of visual validation yet.

## October10 TT engine research and FPS-only overlay

Owner confirms level1 leaves still flicker on clean ba9e37f1f6e208d7 and says
hub ready. No reliable hub visual verification was obtained: the Windows
capture returned foreground browser content, then user input invalidated the
observation. Closing input was rejected; fresh window enumeration subsequently
found no game and CIM confirmed it was gone. Do not claim a captured normal hub
or that the assistant successfully closed this benchmark.

Added a digits-only bottom-left native FPS counter after the final present
copy and before the PRESENT transition. It samples actual native present cadence
over half-second windows, uses fixed 3x5 digit masks at 2x pixel size with a dark
shadow, and prints no title, units, timing, graph or other stats. Two bounded
rectangular color clears avoid font/shader/upload resources and UI-refresh FPS.
Built candidate15b518ac3b345155, staging proof passes, SDK/baseline unchanged.
No launch; counter appearance and overhead remain unverified live.

Research sources reviewed October10:
- https://ttmodding.fandom.com/wiki/Engine (direct open unavailable; search index readable)
- https://ttmodding.com/engine.html (accessible community documentation)
- https://ttmodding.com/fileformats/tsh.html
- https://github.com/AcK77/TTGames-Explorer-Rebirth (tool author's repository)
- https://github.com/JayFrancoe/NuTCrackerV3 (tool author's repository)
- E:\Claude\LEGO Dimensions\research\avengers\REPORT.md (existing local measurements)

The community groups Nu2 into older PS2 and NXG families, followed by a
separate NTT family. Within NXG, Cinema includes Dimensions: Xbox360/PS3/WiiU
are in its DX9 group, PS4/XboxOne in its DX11 group. Jurassic World and Batman3
are also Cinema references. Avengers and Force Awakens belong to later TFA,
which has both DX9 and DX11 groups. DX9/DX11 naming refers to Windows APIs;
it should not be interpreted as the API of every console listed. The wiki
itself notes its Nu2 groupings are approximate. These are research labels,
not proof of identical material binaries or engine ABI.
Source: https://ttmodding.com/engine.html

Useful original tool capability: Explorer Rebirth documents extraction and
inspection of pc_shaders, nxg_textures, TSH, GHG/GSC and DAT/FPK/PAC/PAK.
NuTCrackerV3 is specifically an NXG_TEXTURES extractor/compiler. These can
provide independent offline texture/container references; neither is a
renderer replacement or an automatic repair for active GPU state.
Source: https://github.com/AcK77/TTGames-Explorer-Rebirth
Source: https://github.com/JayFrancoe/NuTCrackerV3

TSH documentation describes a texture sheet containing DDS plus crop/trim
coordinates and character hashes, with significant version differences.
This is a useful lead for UI/minimap/icon UV audits, not proof that the present
minimap defect is in TSH loading. Source: https://ttmodding.com/fileformats/tsh.html

Our measured Avengers evidence is stronger than taxonomy: installed D3D9 exe
is x86, DX11 exe x64; shared chunk layout but shader IDs did not match.
Existing avgref.py signature matching finds material-family references for
55423/95932 Dimensions shaders (57.8%), not 55423 verified equivalent shaders.
A material family can contain multiple platform/permutation behaviors. For
flickering foliage/lava, compare the exact captured Dimensions material with
its matched PC disassembly, alpha/depth states and constants. D3D9 executable
is the closer backend reference; DX11 reflection is useful independent shader
math evidence. Do not port ready PC shaders based only on names/signatures.
No Avengers content is staged or shipped.

Practical direction: retain C++ nativeD3D12 and our Xenos material inputs;
use Cinema/TFA tool research to audit packing, animation UV constants,
alpha/depth handling and resource lifetime. Engine labels alone cannot identify
the 18.7FPS bottleneck. Detailed CPU/GPU timing remains needed; visuals,
particles, lava/foliage flicker, minimap and the static spinning pad are open.
No SDK/Vulkan changes, no push/merge, no game relaunch.

## October10 offline texture binding and performance follow-up

UploadTextureResource can replace a color texture's host storage and SRV when
a depth/color alias is promoted or returned to its guest dimensions. BindConstants
previously selected the descriptor before upload and refreshed only depth views.
It now selects the successful upload's current view for every texture type, and
rejects a missing post-upload view instead of submitting with the prior descriptor.
Old storage remains retained for earlier draws. This fixes a demonstrated binding
ordering error; its contribution to the owner's world-travel corruption remains
unverified. No global depth-comparison or material/color substitution was applied.

The production texture-binding fixture now simulates SRV replacement during a
color upload and descriptor publication failure. Its --verify-regression mode
restores the old depth-only refresh and confirms the replacement assertion fails.
The full68 portable checks pass, including texture authority, aliasing, residency,
descriptor retirement, stream conversion and frame cadence.

Packed vertex-stream bindings now use a bounded64-entry stack array/span rather
than a vector allocation per used stream per draw. Sorting, duplicate removal,
conversion keys and every-use content checks are preserved. This removes those
heap allocations; no live frame-rate gain is claimed. Existing production stream
window and vertex-conversion fixtures pass.

Saved level1 instrumented trace window18000..19500 averages2315.8 draws,
7.80ms constants (including3.69ms texture binding),6.62ms vertex work and
1.23ms buffer hashing. Independently selected GPU source frames18000..19500
average18.98ms of timestamped queue work. Probe and GPU frame counters are not
paired here; capture overhead, waits and guest CPU time prevent treating this as
the clean benchmark. Private evidence: transition-audit/performance-window.json.
The clean baseline remains18.67FPS over600 frames with captures and detailed
timing off. Candidate throughput and stable30FPS have not been verified.

Final build fingerprint is recorded in transition-audit/candidate-proof.json.
Monitor3, native1080p and DoFoff remain configured. Original build/SDK DLL hashes
remain unchanged. Foliage/lava flicker, LOTR spikes/black patches, minimap base,
cartoon presentation and spinning-pad animation remain open. No game launch.

## October10 next test candidate: depth precision and explicit CPU overwrites

Guest D24S8/texture format22 now maps to D24_UNORM_S8_UINT, preserving the guest
UNORM24 depth quantization rather than storing it as float32. D24FS8/format23
retains D32_FLOAT_S8_UINT. Both retain stencil. Native stencil selection now uses
the pinned Plume stencil-format predicate; the EDRAM color-to-depth restoration
pipeline uses the UNORM24 destination selected by its format22-only caller.
No depth comparison was relaxed. The pinned Plume implementation already supports
the required typeless allocation and depth/stencil/sample views; no dependency
changes were made. Verified storage size is4 bytes/pixel for UNORM24 versus8 for
the previous float32+stencil mapping. This does not establish an FPS improvement.

The new depth-format fixture executes production conversion code using the pinned
SDK/Plume enums. Stencil pipeline checks now cover256 references on each format;
depth upload/alias, pipeline failure and resize checks pass. Float24's exact guest
precision emulation remains outside this change.

Explicit CPU pool copies previously cleared the texture's upload cache but left
resolved_on_host and its sampled/depth mirror valid. UploadTextureResource then
treated the old GPU resolve as authoritative and ignored the overwritten guest
bytes. Invalidation now clears host authority, sampled validity, resolve/alias
generations and the CPU stamp for overlapping data writes. EDRAM surfaces and
non-overlapping allocations remain intact. Descriptor/storage retirement on
header overwrite is preserved; ordinary address relocation still preserves GPU
authority until an explicit CPU write supersedes it.

The production pool-invalidation fixture exercises data and header overwrites,
unchanged-header reuse, non-overlapping writes, EDRAM exclusion, source authority
and retirement. Restoring the stale-host-authority behavior fails its regression
case. Final suite results and fingerprint are in transition-audit's proof/logs.
These are proven code defects; visual outcomes remain for the owner's next test.
Recommended clean test: main hub -> level1 foliage -> hub return -> Adventure Time
lava, with native1080p/monitor3/DoFoff and the FPS-only overlay. Stable30FPS remains
the performance target. No launch, SDK rebuild, push or merge by this agent.

## October10 owner rejects UNORM24 storage candidate

Owner reports level1 looks worse, with incorrectly pictured textures. Screenshot
shows warped yellow-road detail and vertical artifacts; this does not isolate
which change caused them. Owner separately reports FPS feels substantially more
stable. Roll back the UNORM24 storage mapping and its restore PSO to the established
D32_FLOAT_S8_UINT representation before testing further; do not claim the rollback
proves the cause. Preserve placement refresh, post-upload SRV selection, explicit
CPU overwrite invalidation and stack conversion keys. The updated depth fixture
locks down that compatibility mapping; generic stencil-format support remains.
Six affected depth/stencil/alias/cache checks pass after rollback.

FPS glyph blocks grow from2x2 to3x3 pixels, with spacing8->12 and height10->15;
the requested1.5x enlargement retains bottom-left placement and digits only.
Build initially could not overwrite the running game; owner closed it on request.
No assistant launch. Final candidate fingerprint is in transition-audit proof.
The latest600 recorded rows of benchmark-20261010-014730 average16.22FPS, with
2662..2672 draws/frame and no independent scene marker. These are not directly
comparable to the earlier18.67FPS/2158..2159-draw level1 window; do not use them
to contradict or numerically quantify the owner's stability observation.

## October10 rollback retest and transition-stall diagnostic

Owner ran bc57717afb1c8937 in benchmark-20261010-015433. Level1 screenshot
still shows smeared yellow-road detail, blurred rocks and striped foliage.
The depth-storage rollback therefore did not resolve the reported regression.
FPS-only digits are visibly enlarged. A marked600-frame level1 window averages
20.20FPS at2415..2416 draws; this does not meet stable30FPS. Across this run,
110 complete gameplay rows exceed111ms at more than1000 draws. Owner confirms
5..9FPS occurred in focused normal gameplay, apparently first return from a level.
A returned-main-hub screenshot shows intact floor/background/shadows in this
single view; it does not establish all travel corruption fixed. Evidence and full
closed-run log are in native1080-check/benchmark-20261010-015433. Game closed;
no assistant launch.

Added opt-in LEGO_NATIVE_SLOW_OPERATIONS CPU timings: pool-copy recording-lock,
source validation, CPU copy, cache invalidation, guest-helper tail; texture-upload
total. Logs only operations >=4ms, capped128 entries across the process. No GPU
readbacks, texture dumps or per-frame files are added. Normal benchmark leaves
this option disabled. Local Run-transition-profile.cmd opts in for a subsequent
owner-authorized run; no new visual or FPS fix is claimed. Native build passes;
pool-copy, pool invalidation, binding failure and depth-upload checks4/4 pass.
Monitor3/native1080p/DoFoff and shader coverage remain; dependency changes intact.

## October10 retained uploads across relocation and steadier FPS digits

Owner requests actual stutter/drop-frame improvements and a stable counter.
Implemented allocation-free NativeFpsCounter in device state: rolling >=1-second
completed-present interval average, digits update no more than twice/second.
Records successful presents only; slow intervals stay included, no30FPS clamp.
Device/clock reset clears history. Existing1.5x bottom-left digits remain alone.
Tests cover steady30/60, a600ms stall lowering the reading, recovery, unequal
intervals using elapsed-time weighting, display cadence, clock reset/NaN and
bounded-ring wrap at600FPS.

Borrowed color textures now compare relocation-independent source keys: hash ALL
base/mip bytes and size/layout semantics, masking only the two20-bit allocation
addresses in SDK fetch words1/5 while retaining allocation presence. Depth upload
keys remain unchanged. An address-only header refresh discards its page stamp but
retains a valid CPU upload for comparison. Plain CPU pool overwrites likewise keep
that comparison candidate while discarding the clean stamp. A changed payload
therefore hashes differently and decodes/uploads; identical pool moves avoid
those costs. GPU-resolved/depth-alias resources still discard their CPU comparison,
host authority, mirrors and generations on explicit overwrites. Header or layout
replacement still retires storage/descriptors at fences; dirty/failed uploads stay
dirty. This removes proven redundant work, without claiming a measured FPS gain.

Actual refresh-body fixture passed2708 captured header pairs:2515 relocations,
193 replacements,10836 calls. Source-key regressions cover changed base/mip bytes,
changed decoding semantics and mip-allocation removal; restoring address-dependent
keys fails the identical-relocation assertion. Old stale-GPU-authority mutant still
fails. Full72 checks pass; after the final overlapping-pool-copy optimization,
five relevant counter/cache/binding/depth checks pass again. Native build passes.
Fingerprint499ed11f3d95f053; no launch/push/merge; baseline/SDK hashes unchanged.
Current visual defects (level1 smear/foliage flicker etc.) and stable30FPS remain
pending live verification. Local test profiles remain on monitor3, native1080p,
DoFoff. Run-transition-profile.cmd enables bounded slow CPU operation logging;
Run-native1080-benchmark.cmd leaves it disabled for clean performance sampling.
