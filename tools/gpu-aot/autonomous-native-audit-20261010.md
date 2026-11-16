# Native resource corrections and Avengers audit, October 10 evening

## Authorization and current scope

The owner authorized launches and all game inputs on monitor 1 while away,
superseding the previous owner-only launch restrictions. Downloads and new
evidence stay on E:. Native 1920x1080, DoF off, C++/D3D12, stable 30 FPS remain
the target. Preserve the independently dirty SDK, Plume and mod-manager trees;
no SDK rebuild, push or merge was performed.

The afternoon performance work was checkpointed locally at d118c1d, including
fixture updates for its new interfaces. This retains surface reuse, pacing,
anisotropy fallback and sampling/pass profiling. The original executable is
saved privately as session-20261010-autonomous/performance-baseline.exe.

## Corrections implemented

- PoolCopyHook guards GPU-authoritative **sources**, not destinations. A CPU
  copy can overwrite an old GPU destination, then publish its new authority
  through the existing texture invalidation path. GPU-only source data remains
  guarded; this does not implement general memexport or GPU source readback.
- Header bytes alone are CPU data and no longer trigger the GPU-pixel-source
  guard. Existing destination-header replacement still retires descriptors and
  storage; this change does not prove preservation across every header move.
- Source rejections now name the matching texture, physical source, resolve
  authority and depth-alias generation, bounded to 32 messages. The earlier
  65-copy burst cannot yet be classified as resolved without this live evidence.
- Surface pooling stops and releases its stored textures before device teardown.
  Late retained-frame resource destruction cannot repopulate the old-device
  pool. Device creation starts a fresh active cache. Same-device reuse and the
  16-per-shape bound remain. An empty wrapper deliberately survives static exit
  to avoid destruction-order dependence.
- Draw-resource reset releases the F10 timestamp query pool before its creating
  device is destroyed.

Production-body tests cover CPU overwrite of a GPU destination, protected GPU
sources, unreadable/non-writable/unaligned copies, physical overlap, original
helper execution and POINTLIST scope restoration. Separate tests cover header
versus pixel ownership and surface reuse, shutdown, late destruction, different
formats, cache limits and device recreation. Restoring the previous destination
or header guard, or allowing recycling after shutdown, makes the corresponding
regression fail. All 74 checks passed (72 in the full run, followed by the two
fixture updates rerun); final modified copy and surface fixtures also passed
their mutation checks. Builds use the existing deployed SDK libraries.

## Actual runs and limits

Private benchmark evidence:

- native1080-check/benchmark-20261010-163403: baseline ba5dbf5d9e8f3af8.
- native1080-check/benchmark-20261010-165110: copy candidate 81a317e0169b3939.
- session-20261010-autonomous/title-tail-metrics.json: last 1,800 frames each.

| Animated title tail | Baseline | Copy candidate |
|---|---:|---:|
| Mean completed-present FPS | 39.30 | 38.96 |
| Median interval, ms | 25.29 | 25.78 |
| p95 interval, ms | 29.58 | 29.33 |
| p99 interval, ms | 31.37 | 31.87 |
| Frames over 50 ms / 100 ms | 0 / 0 | 0 / 0 |

These are diagnostic timing runs, not matched-camera gameplay benchmarks.
Animation and average draw counts differ; they do not prove improvement,
stable gameplay, or world-travel correctness. The later lifetime correction
was built and tested offline, not included in these two live measurements.

One baseline F10 title capture: 1,425 passes, 33.11 ms summed timestamp time.
Leading groups: 3.33 ms/70 draws (VS014C16723AA828EF/PSF1522C6C7C4B24DF),
3.04 ms/238 shadow-depth draws, 2.53 ms/105 draws, 2.20 ms/145 depth draws,
2.17 ms/23 draws and 1.75 ms/43 resolves. There is no single dominant
full-screen pass. Summed timestamps exclude gaps/barriers and capture overhead
prevents treating this as an exact utilization measurement. Logged viewport
dimensions are guest dimensions, not the physical native scene resolution.

Computer-use key taps and a temporary mouse START binding did not advance
the title. F10 worked through its pressed-since-last-query polling. Alt+F4
closed both probes; the known shutdown watchdog hard-exit was logged. No hub,
Oz or world-return route was reached autonomously. The temporary binding was
removed. Monitor 1 remains configured as requested. Do not report visual fixes
or gameplay acceptance from the title screenshots or offline tests.

## Read-only Avengers reverse engineering

Private outputs stay in session-20261010-autonomous; no Avengers executable,
DXBC or disassembly is added to this repository or shader banks.

The installed D3D9 executable gives these string-associated addresses:

- 0x608EF0 is an enum-to-name switch containing "Bloom", not bloom rendering.
- 0x592B20 serializes named settings via virtual calls, including the Bloom
  field at object offset 0xBB; it is not the bloom pixel-processing routine.
- 0x99D780 associates threshold/intensity and their last-generation variants
  with settings; no Dimensions constant register follows from that alone.
- 0x68E200 contains references to NuPostFilterGen::buildMipChain and a mip-level
  loop. Argument meanings and equivalence to the console routine remain open.

The DX11 executable contains 66 valid DXBC shaders whose reflected names
include mip/post/blur terms, including motion blur and DoF. The private scanner
corrects the older avgref PE32 section helper for PE32+ image bases. File offsets
are stable for this installed file, not portable function identities.

Three inspected embedded pixel shaders:

- File offset 0x1C735E0 samples full colour from t0 and a mip texture's alpha
  from t4, computing a clamped blur-related output alpha.
- 0x1C944E0 combines t4/t5/t6, uses their alpha to weight colours and outputs
  a scaled alpha. A single base-mip or global bias override cannot reproduce
  these distinct downsampled-image inputs.
- 0x1CB6CB0 samples nonnegative full RGB and mip RGBA, computes
  `w = saturate(2 * (mip.a * cb2.x + cb2.y) - 1)`, blends full and mip RGB,
  then applies a rational tone curve. Its blend equation matches the existing
  Dimensions tonemap DoF-off correction's documented equation. The PC buffer
  index is not the console register index; names alone do not prove equivalence.

These findings support preserving the narrowly targeted DoF-off mix fix and
investigating bloom stage by stage. They provide no replacement shader IDs,
whole-engine shortcut or proof of the native bloom failure's exact location.
Earlier name-signature twins identify material families, not identical shaders.

### Extended PC post-process inspection

49 reflected pixel shader candidates were disassembled privately on E:. This
is a subset selected by post-process resource names, not the entire game's
shader set. The following additional equations are verified for this installed
Avengers DX11 executable; their use in Dimensions still needs a console draw
and intermediate-image comparison:

- Offset 0x21D2360 samples a luminance texture's green channel at its centre.
  With `L` that value, `C` input RGB and `M = max(C)`, its bright factor is
  `saturate(M - 2/L)`. RGB is multiplied by that factor and a scalar setting,
  divided by `max(max(RGB), 1)`, square-root encoded, tinted and saturated.
  Alpha separately encodes `saturate(log2(M)*0.115525 + 0.5)`.
- 0x21E24F0 performs a four-sample variant: each RGB sample is weighted by
  `max(max(C_i) - 2/L, 0)`, then the sum is scaled by one quarter and the
  scalar setting before the same normalization/square-root/tint steps.
  Its threshold factors are not saturated like the single-sample variant.
- 0x21CE5F0 is a seven-tap weighted RGBA blur with per-coordinate weights and
  an output tint. Reflection names alone do not prove which effect calls it.
- 0x1CBE4C0 combines `max(fullRGB,0) + max(blurRGB,0)^2`, then applies the
  same rational tone curve found in the earlier DoF-family inspection.
  0x1CBFCC0 additionally performs animated distortion and LUT sampling.
- 0x1C5DFC0 is a radial ten-sample blur, not a bright-pass: distance controls
  its radial sample span and output alpha.

This provides a useful encoding/composition reference, not shader bytecode to
port. A bloom failure can arise before the blur, in luminance/threshold inputs,
or during encoding/resolve/composition. No blanket exposure or UNORM override
is justified by these results. No Avengers binaries, disassembly or assets
were added to Git.

## Work still required

### Follow-up: upload costs and input delivery

The complete 74-check suite passes after the compressed-block copy changes.
Upload diagnostics now split source validation/hash, decode/content hash,
allocation/map/staging, and command recording/retirement. A startup probe
reported two large decode stages at 21.602/21.919 ms before optimization and
16.379/16.455 ms afterward. The runs were not paired by texture identity, so
these are observations rather than a measured world-return improvement.

Aligned 8/16-byte blocks validate the whole source span once and copy words
with the same absolute-address endian XOR. Small texels retain the original
bytewise loop; unaligned compressed blocks also retain its checked fallback.
6,400 length/alignment/format/endian cases compare successful output and
rejection against a bytewise oracle, alongside the existing 3D layout cases.
Private microbenchmark output shows compressed copies improving from roughly
1.6-3.4 ms to 0.9-1.8 ms in its synthetic blocked ordering. This measures the
copier, not SDK tiling, GPU frame time or actual gameplay acceptance.

Additional probes: benchmark-20261010-172144, -172702 (temporary
SDL_WINDOWS_RAW_KEYBOARD=0), and -173844 (new copier and input audit). No
probe reached the main menu or gameplay. Return did reach the application's
OnKeyDown: key 13, window_focus=true. Focus cycling and disabling raw keyboard
did not solve START. The opt-in LEGO_NATIVE_INPUT_AUDIT logs at most 32 key
events. SDK MnK has a separate focus flag and only samples current held keys;
late focus initialization and short-tap delivery remain hypotheses to test.

The existing sdk-build cache's CMAKE_HOME_DIRECTORY still points to the old
C: worktree. It was only inspected; no SDK object/library was rebuilt or
installed. Any SDK fix needs an isolated E: build and must preserve the other
renderer work. All game probes were closed, with the known shutdown watchdog.

The [engine wiki](https://ttmodding.fandom.com/wiki/Engine) is community
classification and explicitly calls its grouping arbitrary. Its current
listing puts Dimensions Xbox 360 in Cinema/DX9 and Avengers Xbox 360 in
TFA/DX9, while their newer consoles are listed under DX11. Treat this as a
research lead, not proof of identical shader ABI or algorithms. Our local
disassembly remains the evidence for each proposed reuse.

1. Obtain a repeatable gameplay route through fresh hub -> Oz -> hub and one
   adventure world -> hub, including the first 30 seconds after return.
   Resolve automated input delivery or use the owner's next ready scene.
2. Use the new bounded source logs to classify remaining rejected copies before
   adding GPU-source relocation/readback. Never substitute stale CPU backing.
3. Pair exact material draws and all mip levels for foliage, Oz road/ground and
   rock bands. Separate alpha/depth flicker from wrong LOD or stale bindings.
4. Compare native and emulated bloom/downsample/tonemap intermediates; do not
   enable the UNORM resolve experiment that previously whitened the menu.
5. Measure clean marked scenes and travel stalls. Preserve surface reuse;
   optimize observed allocation, pipeline or binding costs with fence-safe
   content validation. Do not smooth the FPS counter to hide long frames.
6. LOTR minimap/terrain spikes, lava, particles, cartoon outlines and the static
   hub pad remain unverified/open. No claim that all known bugs are fixed.

The opt-in slow-operation trace now also emits `pool-copy-invalidation` with
separate buffer, texture and shader durations when their combined work takes
at least 4 ms. The outer `pool-copy` total still includes that work; nested
times must not be added to it again. This is attribution instrumentation,
not a measured transition-speed fix.

### Title RenderDoc probe, 18:07-18:14 local time

QuickStartup's installed mod changes only splash-screen durations; it does
not load a save. The earlier prompt's auto-load description is not true for
this current installed mod. No safe automatic gameplay route was found.

RenderDoc 1.46 was already installed on E:. A native title probe launched
through renderdoccmd (target 38920, process 17588) reached START on monitor 1.
F9/F12 did not produce a capture. At 18:11:23 the game reported a five-second
guest critical-section wait and wrote `game-build/hang-17588.dmp`.
The subsequent documented RenderDoc target-control request connected to the
correct process but produced no capture while that process was stalled.

The dump has no exception stream; the old crash-only mdscan script cannot
read it. A private thread/stack-word inventory found native worker thread
21740 in a command-fence wait, with candidate return addresses in
WaitForSubmissionLocked, BeginFrameCommandsLocked and DispatchDraw. These
are scanned stack words, not an unwound call stack. Guest diagnostic owner
LR 82BCE244 is the caller of indexed draw hook 83FC6A58. This is evidence of
a stalled native submission path under capture, not proof of a new rendering
fix or of the cause of ordinary travel stutters. RenderDoc/NVIDIA overlays
were active; no clean benchmark claim is made.

Alt+F4 closed the game at 18:14:13 through the normal title-termination path
(no shutdown-watchdog message in this probe). The diagnostic helper was also
closed. Raw dump, failed-capture log and replay scripts remain private on E:.
No emulated comparison was captured. Next use the existing manual one-frame
native snapshot route for title intermediate images; avoid repeating this
RenderDoc capture without investigating the submission stall first.

### Built-in title snapshot, 18:18-18:27 local time

A clean native process (6776, fingerprint 256f0820fe8656b1) reached animated
START on monitor 1. The existing manual capture-next-frame trigger saved 66
DDS intermediates, 462,564,904 bytes, for one complete frame (f16862). No
RenderDoc overlay was involved. Private ranges, contact sheet and original
snapshots are under session-20261010-autonomous/title-snapshots and its parent.

The HDR resolve and sampled scene are 1920x1080. The sampled HDR is bounded
near 8. The 640x360 sampled downsample is populated; the next 320x192 resolve
contains bright portal/logo highlights (maximum RGB about 0.694), and the
subsequent blur chain is populated (index 57 maximum about 0.0902). Final
tonemap and present are 1920x1080. Resolved surfaces can include padded rows;
these dimensions are not evidence that the guest viewport is 320x192.

This rules out a wholly empty bright-pass/blur chain in this title frame.
It does not establish correct bloom strength, integer resolve equivalence,
or gameplay correctness. Contact-sheet HDR display uses an explicit 0.125
analysis multiplier, not an in-game exposure change. No UNORM override was
enabled. An emulated intermediate comparison is still needed.

Alt+F4 closed the probe. The log reports the existing three-second shutdown
watchdog hard exit at 18:27:14. No game process remains. Automated START
delivery remains unresolved; gameplay, first-return stalls, foliage, lava
and world-specific visuals have not received live acceptance in these runs.

### Owner-assisted Oz and hub return, 18:34-18:48 local time

Owner loaded level 1 in PID23144, the current native diagnostic on monitor 1,
and returned to the hub. Owner then requested monitor 2. The fresh window
observation placed the game at screen X1921 on the portrait secondary display;
the persisted monitor setting is now 2. Alt+F4 closed that window at 18:48:40,
followed by the existing shutdown watchdog at 18:48:43. No instance remains.

Private evidence is native1080-check/live-20261010-183436: level frame f13059
(54 DDS), returned hub frame f28168 (56 DDS), texture uploads with guest mip
bytes, draw traces, GPU timings and closed-game.log. The first draw trace
contains 64 early passes. The skip=500 request produced an empty file; a
transition/loading frame with fewer draws is possible, and no material facts
may be inferred from that empty trace. The next hub trace contains data.

The observed level foliage still has changing striped gaps and the yellow
road is smeared. Sonic is in his idle sprite animation; this is not evidence
of a death/respawn bug. Two F10 profiles have 2669 passes; VS C0C579A9C7D3ACD9 /
PS BC67385D86BABED8 accounts for 1430 draws and roughly half the summed pass
time. Timestamp instrumentation perturbs tiny draws; its 50.07 ms sum is not
the ordinary frame interval or proof of a 20 FPS gameplay limit.

Timing rows classified by draw counts, not paired camera markers:
- Last 600 level frames: 32.09 FPS from summed intervals, median 30.38 ms,
  p95 37.16 ms, p99 49.02 ms; four frames >50 ms, none >100 ms.
- First 600 returned-hub rows: 41.51 FPS, median 20.19 ms, p99 137.24 ms;
  nine frames >100 ms.
- Last 600 hub rows: 59.75 FPS, median 16.66 ms, p99 19.57 ms;
  no frames >50 ms.

These are instrumented scene windows, with focus/monitor changes and animation
differences. Do not describe them as a clean stable30 acceptance or a paired
speedup. Private level-return-timing.json preserves rows and assumptions.

The optional bounded draw recorder now accepts `ps=bc67385d86babed8` (or an
optional 0x prefix) as an alternative to `skip=N`. It records up to 64 matching
draws from one following frame and retains the four-request and byte budgets.
`ps=0` explicitly selects depth-only draws. Default trigger behavior and normal
rendering stay unchanged. The production regression exercises unrelated draws,
an exact 64-bit material hash, depth-only selection, malformed hashes and frame
boundaries. Next capture material-specific alpha/depth, UV and fetch states;
the first-64-draw trace alone cannot diagnose the remaining foliage or road bug.

### Material-specific Oz capture, 18:58-19:04 local time

PID16616, fingerprint75212d84155233b8, ran on monitor2 with owner-assisted
level1 navigation. Four requests saved64 draws each for pixel shaders
BC67385D86BABED8, 30E7126AF4380F71, C427F57A96315196 and7F247ED809B11B6C.
Evidence remains private in native1080-check/live-20261010-185851. Alt+F4
closed the probe at19:04:14; the shutdown watchdog exited at19:04:17.

Matching index range/base and instance c48..51 yields39 pairs across the
foliage color and prepass traces, taken in different frames. The depth trace
contains43 wind VS FFA4DD4B364A999D and21 static VS E669A744921D81E3 draws;
comparing the static VS alone with the wind color VS would be incorrect.
The linked color shader already discards using texture alpha, vertex COLOR1.w,
PS c4.w and c58.xy. Fixed-function alpha disabled there is not proof of a bug.

Active fetch for bound ED4B2674 contains size word0 and a dimension different
from its uploaded1024x1024 resource. This is an observed mismatch requiring
writer attribution, not evidence to force all mips or replace all samplers.
The old trace omitted c4..7/c18/c92..95 wind transforms and PS c58. Its first128
snapshot constant blobs belong to earlier passes and do not cover this material.
No new F9 CPU profile was produced; existing afternoon profiles are unchanged.

The next diagnostic reads a bounded96 VS/64 PS register set and the readable
bound texture header beside each active fetch. `pair=<PS hash>,<PS hash>` records
up to32 draws per material from one following frame, preventing an early depth
pass from exhausting the later color-pass quota. It keeps the64-record,1MiB,
four-request caps. Regression verifies both quotas, late color capture, malformed
pairs, wind/cutoff constants and distinct active/header fields. Rendering logic
and SDK sources are unchanged; this is not a visual-fix acceptance.
