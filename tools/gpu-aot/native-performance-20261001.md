# Native performance investigation, October 1

User priority: improve FPS first; missing heads/HUD/details in hub remain open.
Runs are authorized on monitor 1. Close every probe when finished. User input
may be needed to enter a save and reach the hub. Do not infer native mode from
F3/F4: those overlays remain unavailable in native.

## Evidence before changes

Original user log preserved at
`rexlego/out/native-gpu/session-20261001/perf-hub/user-last-game.log`.
It includes resource/frame-slot GPU waits and placement shader misses with
prefixes C77B3ABB6F87ACD9 (VS, zero first words) / F7F244F983B4B08F (PS,
first words look like float constants). Their relationship to missing heads/HUD
is unproven. These prefixes were already present in September 30 hub evidence.

Baseline executable SHA256:
`1F873E4F113526882D4EFE07C48C3B49E1E49B9D936C7C3EEDFC43A189A0037C`.
Config SHA256:
`E97CAFFAC325E5CBDF1A8D25FD757A0BA6911EAE844D77D9E9D7D6C2CA71BF5B`.

Probe `oct01-fps-baseline1`: native, depth alias enabled, timing CSV, bounded
missing-shader capture, no RenderDoc. Visually confirmed title. At frame 3217,
last 20.05 s: 10.87 FPS, median 90.29 ms, p95 101.86 ms, texture CPU 33.49 ms,
constants 36.73 ms (includes texture CPU), vertices 10.84 ms, buffer hash 9.51 ms,
resource waits 5.61 ms. About 1080 draws/frame. Closed PID 36200.

Probe `oct01-fps-windows1`: same exe with BufferWindows enabled; other candidate
options off. Title visually confirmed. At frame 3392, last 20.08 s: 9.76 FPS,
median 97.60 ms, p95 129.47 ms, buffer hash 0.89 ms, vertices 9.87 ms,
texture CPU 37.51 ms. Animation phases differ, so this is not a deterministic
benchmark. No demonstrated overall FPS gain. Window uploads converted about
5 MB/frame and repeatedly allocated GPU buffers. Closed PID 36048.

System samples during title: 14672 MB available RAM, 3 pages input/s, global GPU
utilization 21%, global VRAM 2006/8188 MiB. Browser also running. These are title
samples, not hub measurements; no evidence of memory capacity exhaustion.

## Changes under validation

1. `BaseHeap::IsRangeCommittedReadable` checks only requested pages under the
heap mutex, including commitment and read protection. Previous texture
validation called QueryRegionInfo, which scans to the end of the allocation
on each bind. Texture content hashing remains unchanged by this change.
2. Buffer window cache replaces unordered-map begin eviction/64 entries with
LRU, a per-parent byte budget (256 KiB to 16 MiB based on parent length) and
4096 entry cap. A single oversized required range may exceed the byte budget.
Still opt-in; immutable upload lifetime and every-use hashing preserved.
3. `LEGO_NATIVE_AUDIT_TEXTURE_WATCH` / probe `-AuditTextureWatch` verifies each
would-be pixel-texture cache hit against complete source hashes. Logs false
clean stamps as errors. Vertex textures always force hashing. Audit results
are required before considering promotion of the existing pixel-only watch.

Offline checks: actual bounded range method passed holes, reserved/read-only
pages, unaligned bounds, 4 GiB edge and 10000 differential cases. Actual buffer
resolve/adoption code passed 10000 fetched-byte equivalence cases, relocation,
same-frame writes, old upload lifetime, 1000 rebasing cases and a 256-range
working set reused for three frames with no new GPU allocation (fake driver).

Build note: game links installed SDK, not SDK source directly. Initial game-only
build failed because the installed header lacked the new method. SDK Release
install succeeded; rexruntime.dll deployed (previous DLL preserved in perf-hub).
Game rebuild pending at time of this entry. Do not treat old exe as updated.

Follow-up: full SDK install/game build succeeded, followed by an incremental
check. Executable SHA256 E5666C247F0D4BB9F771547C0D51B6D96406716BD1C6DF56B3299E30F58205E7.
SDK build now identifies as 0.10.0.40-dev.gd846a43; config remains native. The
SDK install caused CMake regeneration and extensive recompilation; no generated
game functions were changed by codegen. Previous runtime DLL preserved.

Shader placement lookup now hashes each distinct candidate byte length once
per lookup, preserving the candidate iteration/matching and rehashing on every
new call. Actual-function test: 3001 shared-prefix entries, stage/missing/hit
checks, mutation at same pointer, at most four hashes instead of 1501.

Real SDK integration test `test_memory_watch_sdk.py` also passed: native watcher
plus installed runtime, real protected-page CPU writes/rearming, Protect,
explicit physical-copy invalidation, Decommit and bounded commitment query.
Standalone fixture needed the same LLD linker/runtime flags as the application.

Probe `oct01-fps-audit1` launched on monitor 1 with range cache + full texture
watch verification + bounded missing shader capture. This is a correctness
audit, not a clean performance sample. Completed 3300000 verified cache hits
with zero stale-content mismatches through intro/title; PID 28740 closed.

## Resolution

Window/video-mode/resolution config already requests 1920x1080/1080p. Main game
render targets are still 1280x720. Generated game initializer sub_82B67190
(legodimensions_recomp.133.cpp) explicitly passes 1280/720 repeatedly to
sub_82B64790 and related target creation. Native device also starts with a
1280x720 viewport. A config edit alone is not true 1080p. Scaling must preserve
postprocessing, depth aliases and the guest's 40-column depth-bank permutation;
no true 1080p implementation has been validated yet.

## End of October 1: measured hub and saved configuration

All game probes are closed. The user is going to sleep and requested wrapping
up after the hub measurement. No more launches tonight. Final executable built
successfully; its final config-backed option wiring was checked offline, not
with another game launch. The rendering algorithms had already run in the
earlier audit/fast probes.

`oct01-fps-fast1` (E5666C... executable, BufferWindows and StaticTextureWatch
explicitly enabled): visually confirmed title, last 25.01 s at frame 8400:
20.23 FPS, median 49.39 ms, p95 53.00 ms; texture CPU 0.79 ms, vertices 6.15 ms,
buffer hash 0.80 ms. Animation phases differ from baseline, so this is an
indicative title comparison, not a deterministic benchmark. Giant red minifigure
passed in front of the title camera; saved image `fast1-title-anomaly.jpg` should
not alone be taken as proof of a new geometry bug. PID 28948 closed.

`oct01-fps-textures1` PID 32380: user loaded the save and reported ready.
Foreground confirmed hub, headless Sonic and missing HUD reproduced. Foreground
frames 5881..6350: 470 frames / 22.85343 s = **20.56584 FPS**, median 48.0882 ms,
p95 54.405 ms, max 79.183 ms. Texture CPU 0.640 ms, vertices 10.324 ms, buffer
hash 1.463 ms, about 505 draws/frame. No explicit Synchronize calls in that
window; other frame-ring waits still exist (begin ~10 ms in sampled logs).
This is the first measured hub interval; there is no matching pre-change hub
baseline, so do not claim a measured hub speedup ratio.

Hub resource sample: working set 2283 MiB, private 2152 MiB, 15178 MB free RAM,
zero pages input/read per second. Hottest render thread ~75% of one core;
whole process ~1.18 cores. Global GPU 36%, VRAM 1885/8188 MiB (other apps share
these GPU counters). No evidence of memory capacity pressure. Repeated scans,
conversions, allocations, CPU work and frame-ring waits remain the leads.

Important probe caveat: textures1 ran the earlier E5666C... executable after the
launcher gained explicit `LEGO_NATIVE_BUFFER_WINDOWS=0`. That older executable's
presence-only option handling may treat `0` as enabled. Its flag record says
false but is NOT proof of a buffer-cache-off A/B. Use the measured FPS as scene
evidence, not as isolation of that switch. Final build parses `0` explicitly,
logs its effective range-cache state, and future probes save the config plus
actual environment overrides. This is not a renderer-route ambiguity: both
probes explicitly used native D3D12.

Final saved config (legodimensions.toml): native D3D12, gpu_native_pm4=false,
gpu_native_texture_watch=true, gpu_native_buffer_windows=true, monitor=1,
output/window 1920x1080 but internal targets 1280x720, depth alias enabled,
DoF=false, frame_rate=60/framerate_limit=240 (not measured native FPS), existing
pixel-shader skip CFEAC7ADB912F8A9 retained. AsyncCpuResources/Stencil/Viewport
candidates OFF. F3/F4 still unavailable. Saved xenos plugin/backend fields are
fallback settings; native bootstrap disables the SDK GPU plugin.

Final executable SHA256:
7A8ECB1F61D466FFC0A7D5765B99885353588EBDD7D9DB36F218D3DF5710E786.
Config SHA256: AD4B5E767FC3C61CEA6DCF6BB78D2D7503FD78428E097D43141E90E44EEF50E5.
Runtime SHA256: 42DCF8CBE7083AD69DE129B29F19F685C4A57D326CC06E9AFC09A7E5C57501B8.
Proof: `session-20261001/perf-hub/final-build-config.json`.
Offline final checks passed model callbacks/disabled overrides, real SDK page
fault/rearm/Protect/Decommit/physical writes both env-selected and cvar-selected,
TOML parse, and full native build. CSV now retains all draw stages for the next
profile. No SDK rebuild was needed for final config wiring.

Next: confirm saved config on a fresh launch, capture full missing shader
containers in the hub, profile vertex upload allocation/conversion plus
frame-ring waits. Missing heads/HUD/details and true 1080p remain open.
No new video recordings. Intentional evidence is on E under
`rexlego/out/native-gpu/session-20261001/perf-hub` and named oct01-fps files
under `session-20260930`; transient app/Sky caches may still use C.
