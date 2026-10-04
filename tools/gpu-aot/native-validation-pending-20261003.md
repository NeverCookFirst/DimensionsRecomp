# Required runtime validation after October 3 offline fixes

Latest October4 state: the owner assisted the single monitor3 diagnostic
oct04-format-forensics1 in Vorton and LOTR; that instance has been closed.
Continue offline; another launch requires owner authorization. See
native-capture-20261004.md for the failed image checks and evidence limitations.
The owner rejected the combined explicit-LOD candidate; production
has returned to the coupled624-byte host/shader ABI. Texture format fixes and
late-trigger mip forensics have passed offline checks, but are visually untested.
Use questions for user-assisted hub readiness; no activation or injected input.
Keep the exact executable,
SDK/runtime/plugin hashes and configuration with every probe; close the exact
owned process afterward. See `session-20261003/build-proof.json` for the build.

Current output1920x1080, actual game render targets1280x720. This is not a
1080p native-rendering benchmark. The owner changed the target to stable60FPS
at actual1920x1080 internal rendering on October4; this is still unverified.

Once launches are authorized:

1. The owner confirmed on October4 that Enter no longer causes the yellow
   glitch. Preserve that user-confirmed result; an assistant post-fix capture
   comparison has not yet been made.
2. Slot2 controllable Vorton, matching native/emulated character and camera.
   Check head, body, hands, HUD icons, portrait, effects, shadows and portals.
   Preserve the override material/texture identity if the portrait remains blank.
   October4: use -LongProbe -PortraitTrace -TextureForensics -CaptureMissing to correlate the actual mask's
   NuTexture/backend/active D3D header with draw shader/texture/constant captures. See
   native-work-20261004.md. Arm the fresh process record's textureCaptureTrigger
   only after the owner reaches the affected scene. Keep this separate from clean FPS measurements.
3. Audit buffer watches in hub and three story levels, loading/cutscenes and
   mesh-pool relocations. Require zero stale-clean errors. Until then buffer
   watches stay OFF by default, independent of the existing texture watch.
4. Measure clean comparable runs without snapshots, missing-shader dumps,
   RenderDoc or long traces: warmed hub plus the same level/cutscene sections.
   Record FPS, median/p95/p99/max intervals, draws, texture/constants/vertices/
   hash and synchronization breakdowns, 60/30FPS-budget misses,
   frame-slot/acquire/submit/present CPU times and buffer watch hits/audits/
   mismatches. The new columns are CPU wall times, not GPU timestamps.
   Confirm which output and actual RT dimensions
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
