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
portrait_setup / portrait_destroy. BindConstants reports portrait_texture_draw
when the current owner/material/NuTexture chain matches a referenced texture
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
