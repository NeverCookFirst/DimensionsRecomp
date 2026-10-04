# Native D3D12 work, October 4

The owner changed the target to **stable 60 FPS at actual 1920x1080 internal
rendering**, retaining C++. Another agent is working on ReXGlue Vulkan fixes.
This work modifies the game-side D3D12 renderer only; it does not rebuild,
deploy or modify the SDK or Plume. The owner subsequently authorized game tests
on monitor 3. Probes now default to monitor 3, superseding the initial monitor 2
and no-launch instructions. Ask through the question UI for hub readiness.

The owner confirms the Enter yellow transition glitch no longer appears.
This is user-confirmed visual evidence, not a new assistant capture.

## Portrait identity evidence and diagnostic

The read-only Avengers research identified Dimensions setup 833738B8 and its
material override. Further inspection of the actual TU23 generated bodies:

- 833738B8 stores the loaded object at owner+84 and the override at owner+200.
- 83022C20 returns a smart-pointer wrapper for the property texture.
- 82CC4E50 copies the wrapper's pointee into material+956 and retains/releases
  through the object's vtable. Material+956 is therefore a NuTexture pointer,
  not directly a D3D texture header.
- The first live Sonic trace disproved the earlier backend inference: asset+4
  is the reference count (3), and 82B6FC70 is a generic reference-count getter.
  Read-only memory identifies the asset as sonic_portraitalpha_nxg.tex:1.
- Actual binder 82BCBB78 loads asset+44 into r30 and passes r30 as r6 to
  82BCE7F8. Asset+40 supplies sampler parameters. 82BCE7F8 selects the inline
  D3D header at backend+120+52*index, with index at backend+116. The corrected
  probe follows this statically verified binder path; a live matching draw is
  still required.
- 83373850 destroys this owner. The diagnostic removes it before destruction.

New C++ hooks retain the original setup/destructor behavior. With
LEGO_NATIVE_PORTRAIT_TRACE=1 and LEGO_NATIVE_TRACE_DIR set, they log
portrait_setup / portrait_destroy. DispatchDraw reports portrait_bound_draw
when the current owner/material/NuTexture chain matches an attempted texture
slot, including VS/PS hashes. Re-reading the chain avoids keeping stale setup
identities; reads check commitment and 32-bit bounds. At most 32 owners are
tracked and 64 matched draws reported per frame. Disabled mode performs no
guest reads, and the emulated route bypasses the diagnostic hook bodies.

run_native_probe.ps1 -PortraitTrace requires -LongProbe, which provides the
existing correlated draw, constants and intermediate-image capture. This is a
heavy correctness probe, never an FPS benchmark. No missing portrait cause or
fix is claimed until the actual material/texture/alpha/UV is captured.

## Performance attribution

Timing CSV now includes frame_slot_wait_calls/ms, acquire_cpu_ms,
present_submit_cpu_ms and swap_present_cpu_ms, plus buffer watch hits/audits/
mismatches. These are CPU wall times around existing calls; they are not GPU
timestamp durations. Frame-slot timing includes the existing completion refresh
after the fence wait. Present submission excludes separate KickOff submissions.
The counters do not change fences, descriptor lifetime or submission order.

The summarizer includes p99 and the proportions over exact 60/30 FPS frame
budgets. It documents its lower-order-statistic percentile convention, ignores
nonfinite/nonpositive intervals and unfinished live CSV rows, and excludes
buffer audits and portrait tracing from clean performance claims. Earlier CSV
columns and the previous 33.5ms statistic remain supported.

Offline checks passed: actual portrait identity chain (null/unreadable/relocated
objects and overflow), actual renderer routing macros for all 58 hooks across
12 environment/config cases, actual draw upload/slot-reset functions with timing
ON/OFF, actual fence/resize functions with deterministic driver/events, and
report CLI percentile/budget/window/diagnostic exclusion fixtures. Native Release
build verification is recorded under session-20261004. No new gameplay FPS.

## Actual 1080p is separate work

Output remains 1920x1080; game targets remain 1280x720. The generated initializer
82B67190 creates multiple full-screen targets via 82B64790 / 82B64828, a 640x360
target, and specially arranged 960x800 and 1280x608 surfaces with stack-supplied
offset parameters. Owner+924 is also initialized to 1280. Scaling only the
1280x720 allocations would leave this layout inconsistent. The 1080p change
must account for engine dimensions, viewport/scissor/resolve rectangles,
postprocessing and the depth-bank mapping together; no blind allocation-size
override was enabled.

Next authorized runs: a matching Vorton portrait capture, clean warmed baseline,
buffer-watch audit, then controlled OFF/ON comparisons. Actual 1080p, three story
levels, level-completion spikes and AMD remain open validation gates.

## First Sonic capture and added world coverage

The October 4 Sonic capture shows the head and HUD icons present, with an empty
portrait circle. It remains 1280x720 internally. This heavily instrumented run
is not an FPS baseline. The probe was closed after the capture and read-only
object inspection. The owner also reports mostly black surfaces in the upper
Vorton portal layer and Lord of the Rings world, though some textures appear
correct and the world loads. Include both in correctness coverage; diagnose
shader/resource failures before applying texture color adjustments.

## Upper hub / LOTR coverage candidate, and latest launch constraint

The owner reports every character and vehicle portrait missing, plus world
textures; diagnose a shared path. The second Sonic trace follows asset+44 to
a readable backend and active inline header. No successful BindConstants draw
matched its mask. This does not prove no attempted draw: the original trace ran
after pipeline creation. The revised trace now observes attempted bindings
before shader, declaration and pipeline rejection, including unused slots;
portrait_bound_draw is explicitly not evidence of GPU submission.

The owner moved to the upper Vorton portal layer and LOTR world in the same
probe. Captures saved locally as upper-hub-before.png and the LOTR present DDS.
Large black surfaces are visible in the upper-hub image. The run captured 75
new valid containers, merged with all 175 existing runtime inputs. The current
624-byte alpha/viewport compiler successfully compiled 250/250. The merged
index preserves every previous record, 18836 -> 18983 (147 new signatures).
All 250 inputs pass exact physical/instruction lookup with the main bank present.
This addresses observed shader coverage failures; visual correction remains
unverified. Original mask missingness can have another cause.

The diagnostic process 12764 was closed. The owner subsequently prohibited new
launches until explicitly requested; do not launch validation automatically.
A separate user-owned hotfix/test process 8696 was observed and left untouched.
The dev build can be prepared offline; SDK/Vulkan/Plume and hotfix stay untouched.

Installed offline dev build: runtime 250 containers / 448 prelinked variants,
index 18983, fingerprint 5a3f2a542b8519f7. All previous shader hashes and
specialization masks preserved, all specialization subsets present. Executable
contains the matching fingerprint and portrait_bound_draw marker; runtime DLL,
GPU plugin and saved configuration hashes match the prior verified build.
Proof: session-20261004/world-bank-candidate/installed-build-proof.json.
The build has not been launched. Current internal targets are still 1280x720;
native 1920x1080 and stable 60 FPS remain unfulfilled gates.

## Owner test after bank250 and offline follow-up

Owner confirms the world-transition tube and upper Vorton are fixed; LOTR is
partly improved. Character/vehicle portraits remain blank, gamepad button
prompts are missing, black objects remain, and textures show repeated lines.
A computer-use window capture saved as lotr-owner-test-window.png shows the
lines and black gaps. The title process29716 was closed and subsequent process
inspection found no game. No new launch is authorized.

The Oct4 12:3x slice of the manual log still has one unknown pixel resource,
A863A580 with backing ED5CA160 and zero prefix C77B3ABB6F87ACD9. This run was
manual without missing-shader capture, so the shader's full bytes are unavailable.
A zero prefix does not establish shader identity. Do not substitute a similar
shader or attribute every missing UI element to this one without evidence.
The existing native 2D untile path uses the SDK GetTiledOffset2D; the repeated
lines are not yet localized to data conversion, UV decoding, or mip sampling.

Game-side Alt+Enter now explicitly handles the chord before SDK binds, consumes
key repeats, toggles SDL borderless on the current monitor, restores the actual
windowed logical dimensions, and synchronizes the fullscreen setting. Other
keys use the SDK's ProcessKeyEvent. It does not modify/rebuild the SDK. Offline
helper checks cover repeated toggles, window size changes, monitor retention,
startup fullscreen and zero-size handling; full native build passes. Actual
window behavior awaits an authorized launch.

Final window-mode build fingerprint b2aae4e91c598df6; proof session-20261004/build-proof-window-mode.json. Actual OnKeyDown handler fixture also passes repeat/modifier/null-window/SDK-bind checks. No game launch; visual renderer failures remain unresolved.

## Combined explicit mip-level correction (offline)

Static audit of250 captured runtime containers found70 explicit fetches in46
shaders and41 SetTextureLod instructions. The installed624-byte compiler ignored
SetTextureLod and sampled pixel textures with computed derivatives instead.
Prepared a combined compiler from the exact October1 alpha/viewport source,
then added the previously tested explicit-LOD translation. This preserves the
later alpha gate, eight comparisons, fixed16 output scales and viewport code.
The host upload now appends32 signed fetch biases at offset624, total752 bytes.

Tests pass: seven actual synthetic microcode->HLSL->DXIL cases, predication,
positive/negative bias, vertex/computed behavior and relative-source refusal;
32768 actual host bias/slot cases; existing968 alpha compares/five shader cases;
900 viewport pixel mappings; all actual runtime SetLOD sources and explicit
fetch bias slots found in generated HLSL. Runtime250/250 and main9290/9291
compile, with only the already excluded A58 memexport shader failing. All prior
shader identities/masks retained, no shader coverage removed. Both archives
and prelinked variants are rebuilt together. Rollbackexe/banks/drawsource in
session-20261004/lod-combined/before.

This is a concrete missing instruction implementation, not a proven fix for
the owner's texture lines or missing portraits/button prompts. The remaining
manual-run shader miss needs a complete captured container; pixel validation
and clean FPS remain pending. Game stays closed, SDK/Vulkan/Plume/config untouched.

Verified installed combined build: fingerprint 7ccb1c0c3f20bbde, executable SHA256 d31c258f8f798ed4fb8293f789870d854e9879389e11a3683187e4e8f7af0a05. Main9290/18444 and runtime250/448 all prelinked specialization subsets present. DLL pair/config unchanged. No launch. Proof lod-combined/installed-build-proof.json.

## Owner rejects explicit-LOD candidate; coupled rollback

The owner subsequently manually tested the candidate and reports no repairs,
with worse LOTR rendering. The supplied screenshot shows stripes on Sonic and
terrain, a blank blue portrait and tall vertical artifacts. Preserve it and
the manual-run log in lod-combined/owner-regression.png and
owner-regression-game.log. No game process was running at rollback time.

Restore draw.cpp and both raw/prelinked banks from lod-combined/before as one
change: production returns to624 shared bytes and the Oct1 alpha/viewport
compiler, retaining250 runtime shaders/18983 index records and Alt+Enter.
Copied backup timestamps must be advanced before building: otherwise Ninja
can relink old752-byte objects alongside copied624-byte sources. Force the
affected inputs newer and verify actual recompilation in rollback-build.log.

Offline alpha, viewport and actual Alt+Enter handler checks pass. The screenshot
does not localize the pre-existing texture stripes to LOD, tiling, endian
conversion or UVs. Existing texture-upload dumps contain only the first eight
base textures, so they cannot establish the affected LOTR mip contents. Do not
claim portraits, prompts or LOTR are repaired. No launch, FPS measurement,
SDK/Plume change or hotfix change is authorized/implied by this rollback.

## Offline texture format and scene capture follow-up

See native-texture-format-20261004.md for tested format replication/DXT3A fixes, actual SDK GPU oracle results, instruction audit and late-trigger mip capture. Owner explicitly reaffirmed no launch. Visual failures and native1080p/60FPS remain unresolved.
