# Native renderer validation strategy

The experimental native renderer translates the game's Xbox D3D calls into host
D3D12 operations. Recompiling the CPU executable does not establish correct
texture layouts, shader coverage, depth ownership, render state, or GPU resource
lifetime. Those need separate evidence.

## Current decisions

Keep the tested native build as a baseline. Before expanding the renderer or
changing implementation language, use the existing whole-session PM4 reference
route to compare equivalent scenes through the SDK's Xbox GPU implementation.
`gpu_native_pm4` / `LEGO_NATIVE_PM4_REFERENCE` restore the `xenos` plugin and route
native hook wrappers to the original Xbox D3D functions. This requires a matching
`rexgpu-xenos.dll` and its dependencies; an older published plugin is not a valid
substitute for the current runtime.

The reference is useful independent evidence, not a guarantee of exact console
behavior. A per-draw hybrid is a different project: native and PM4 paths have
different resource ownership and synchronization. Do not switch between them
inside a frame without an explicit ownership design.

## Regression loop

1. Clone a legitimate saved checkpoint, profile, installation state, and normal
   Toypad tags into a private run. Keep separate writable files and ports.
2. Record executable and dependency hashes, selected renderer, configuration,
   shader packs, checkpoint hashes, and the input recipe.
3. Reach the scene with a short known recipe. Confirm camera, characters, and
   scene identity before comparing results. A save directory is not a running
   process snapshot; current SDK running-thread serialization is incomplete.
4. Capture rendered output and diagnostics for native and reference runs. Use
   focused reproductions for failures, and fixed scenes without concurrent
   compilation for performance measurements.
5. Turn each confirmed failure into a regression check. Repeat broad gameplay
   after the targeted reproduction passes.

Portable helper and mocked-driver checks remain useful, but they cannot establish
rendered pixel correctness. Add image checks and real GPU execution to that layer.
The full campaign is an integration test after this loop is reliable.

The first matched Vorton captures have no confirmed renderer discrepancy. The
matching PM4 plugin requires the RTV render-target path on this Intel/Proton
setup; its default ROV path produced grossly incorrect output. Record that
selection instead of treating either reference configuration as ground truth.

A static unpaused native Vorton sample measured 14.20 FPS across 444 complete
frame intervals, with 81.50 ms p95 and about 543 draws per frame. LongProbe and
detailed timing were enabled, so this is a failing instrumented performance
baseline, not an instrumentation-free benchmark. Paused UI results must never
substitute for gameplay performance. Trace-disabled and per-present-only probes
will quantify instrumentation cost before choosing an optimization.

The measured command-slot waits averaged 14.87 ms per frame across 8.34 waits.
Guest kickoffs reuse the three-slot command ring independently of presentation.
A bounded larger-pool experiment must preserve every submission, query,
callback, fence and descriptor-retirement ticket; increasing swap-chain latency
or removing completion waits is not equivalent. Keep the current default until
GPU runs demonstrate a useful gain without regressions.

The frozen `293d4aa0f7071b69` build now provides per-present CSV without detailed
per-draw clocks. In matched, warmed Vorton runs, twelve command slots measured
16.432 FPS against 16.389 FPS with three slots: a 0.26% difference, insufficient
to justify changing the default. A colder interval with resource streaming was
rejected and retained. Cheap wait-call counts are not measured wait durations.

An exact-build userspace CPU profile identified the SDK timer spin loop in 641
of 2,031 samples (31.56%). A restart-only blocking-timer experiment reduced that
thread's CPU consumption from 6.710 to 0.231 seconds per ten-second interval
(96.56%). Bracketed cadence measurements were 16.389, 17.395 and 16.669 FPS for
spin, blocking and spin. NPC/particle work varied, so the CPU saving is stronger
evidence than the modest observed FPS benefit. Timer defaults remain unchanged;
deadline precision, other workloads and campaign behavior still need validation.

Use the tracked `regression/` tools for independent writable checkpoint copies,
exact binary/runtime identities, explicit launch ownership, fixed measurement
windows and comparisons that retain rejected outcomes. Their synthetic checks
and a real-build preparation/report/dry-run smoke do not establish rendered
correctness. Add GPU queue timestamp spans before attributing unassigned frame
time to CPU work; blocked time is absent from userspace CPU samples.

## Shader coverage

The static extractor filters named archive entries. Its output count does not
prove coverage of unnamed entries, shaders embedded in other asset types, sparse
installer archives, or programs constructed by the game at runtime.

An initial audit found no exact physical or instruction-stream matches between
the static-only index and 169 captured runtime containers. Removing container
reflection from identity therefore does not fix these misses.

A bounded pass visited all 19,000 GAME.DAT entry indices without finding those
169 programs. Its legacy decoder did not verify actual produced byte counts,
so the compressed-entry negative result remains provisional. A faithful
SDK-loaded and TU23-patched executable image contained byte-identical metadata,
physical sections, and instruction
streams for 96 of them. Executable-resident shaders were missing from the
DAT-only input inventory. Include the patched image in offline extraction before
asking gameplay to discover this coverage again.

The remaining 73 observed programs are exactly the 73 containers without CTAB
reflection. Their instruction streams did not match the scanned image. A bounded
PATCH.DAT scan subsequently traced 54 of them to ordinary named shader assets:
physical sections, instructions, and fixed shader metadata match byte for byte.
The engine's XDK repacking path removes reflection. The standard extractor was
reading name-tree positions as entry indices; new-format archives require the
authoritative CRC-based `FindEntry` lookup. Fix this association before judging
coverage from an extraction count. A subsequent complete named TU23 scan, with
actual decoded counts verified,
traced the remaining nineteen as well. All 169 observed runtime origins are now
accounted for: 96 executable-resident and 73 archive-resident. This is coverage
of the observed set, not proof that later scenes contain no additional shaders.
The scan produced 7,209 distinct structural source containers. All 3,270 newly
indexed programs passed guarded compilation. Of these, 2,939 strict non-alias
programs passed the normal compiler/exporter/parser chain in fifteen unpublished
additive packs. Those packs remain bound to an older baseline and need normal
re-export for a new build before adoption. Reflection/instruction aliases were
audited separately: both prelinked specialization variants matched byte for byte
for all 281 directly compared pairs. No alias canonicalization is enabled.
An auxiliary ledger initially read the wrong initializer column for masks; its
corrected ledger matches actual compiler fields and the unchanged pack metadata.

Use bounded scan reports with explicit unreadable, sparse, malformed, and
unexamined counts. Record exact container and microcode identities. Compile new
coverage offline, then use validated additive packs to avoid rebuild/restart
cycles. A pack-loaded event proves acceptance; successful adoption of a shader
requested immediately before publication is separate recovery evidence.

## Acceptance gates

- Correctness: independent reference evidence plus a reproducible failure check.
- Coverage: explicit shader and scene inventories, including known gaps.
- Performance: matched scene, camera, build, flags, and an uncontended measurement
  interval; report frame-time distributions and resource work.
- Integration: genuine saves reload, input and Toypad behavior remain correct,
  and campaign testing completes without the failures under investigation.

As of 2026-10-07, the native build has passed 51 portable GPU checks and reached
Oz gameplay. Full campaign completion, universal visual correctness, and smooth
performance have not been established.
