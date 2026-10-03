# Required runtime validation after October 3 offline fixes

Do not launch the game until the user authorizes it. All subsequent tests use
monitor2, no window activation or injected input. Keep the exact executable,
SDK/runtime/plugin hashes and configuration with every probe; close the exact
owned process afterward. See `session-20261003/build-proof.json` for the build.

Current output1920x1080, actual game render targets1280x720. This is not a
1440p native-rendering benchmark. The60FPS1440p target is still unverified.

Once launches are authorized:

1. Verify Enter transition against the saved blue/yellow corruption and normal
   cyan/red frame. The swizzle core passes tests; no post-fix pixels yet.
2. Slot2 controllable Vorton, matching native/emulated character and camera.
   Check head, body, hands, HUD icons, portrait, effects, shadows and portals.
   Preserve the override material/texture identity if the portrait remains blank.
3. Audit buffer watches in hub and three story levels, loading/cutscenes and
   mesh-pool relocations. Require zero stale-clean errors. Until then buffer
   watches stay OFF by default, independent of the existing texture watch.
4. Measure clean comparable runs without snapshots, missing-shader dumps,
   RenderDoc or long traces: warmed hub plus the same level/cutscene sections.
   Record FPS, median/p95 intervals, draws, texture/constants/vertices/hash and
   synchronization breakdowns. Confirm which output and actual RT dimensions
   were measured. Compare fused upload and watch candidates individually.
5. Level completion and return to hub: record spikes, frame-slot/resource waits
   and callback latency; check recovery without Alt+Tab. Resize/minimize/restore
   should preserve aspect and fence/descriptor lifetime.
6. AMD hardware pass on at least one available RX6600/RX9060XT/Vega8-class
   machine using the same D3D12 route and matched SDK pair. Compare identical
   hub, three story levels and cutscenes with NVIDIA captures: HDR brightness,
   resolve exponent/gamma, RGBA8 transitions, depth/shadow atlas, alpha/HUD,
   skinned vertex data, level-end spikes and resize/device diagnostics. Record
   GPU/driver/CPU, dimensions and timing data. No AMD hardware validation today.
7. Promote native defaults only after these correctness/performance gates.
   Retain the tested emulated switch. Async CPU-resource wait bypass remains
   separately opt-in until its CPU-worker/fence contracts are resolved.

The confirmed A58 memexport pool-copy use is already recognized as CPU
memmove with cache/generation invalidation and immutable host uploads. Today's
hub log records verified copies with VS A58 and no byte-verification failure
in those recorded samples. This does not implement arbitrary memexport GPU
writeback or establish every possible consumer; GPU-owned copy ranges still
need their own ordered host-resource copy path if observed.
