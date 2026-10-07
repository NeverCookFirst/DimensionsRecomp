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

## Shader coverage

The static extractor filters named archive entries. Its output count does not
prove coverage of unnamed entries, shaders embedded in other asset types, sparse
installer archives, or programs constructed by the game at runtime.

An initial audit found no exact physical or instruction-stream matches between
the static-only index and 169 captured runtime containers. Removing container
reflection from identity therefore does not fix these misses. Establish whether
the missing programs exist in unscanned asset entries before designing a new
runtime shader-generation path.

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
