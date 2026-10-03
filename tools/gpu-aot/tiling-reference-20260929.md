# LEGO TU23: measured Xenos tiling reference, 2026-09-29

## Scope and capture

Read-only rendering diagnostics; no native rendering change in this investigation.
Xenos test PID14684 ran 21:39:52–21:40:30 on monitor2, sound ON, QuickStartup
only, SDL_WINDOW_ACTIVATE_WHEN_SHOWN=0. Assistant requested normal close at
21:40:29; not a crash. No keys, mouse inputs or focus activation.
Computer Use capture showed a correctly coloured WB Games logo over the galaxy
intro. This is not visual verification of the later story scene or hub.

Trace: `rexlego/out/native-gpu/xenos-tiling-20260929-2140.log`.
4662 events, partial frame0 then complete frames1/2, overflow=0.
Opt-in environment variable LEGO_XENOS_TILING_TRACE; trace automatically arms
at the first 1280-wide surface with a nonzero window offset. Draw packets are
logged BEFORE bin predication, and actual draw / decoded resolve details after
the applicable execution paths. No render registers or predication changed.
Trace stops after three swaps or 20000 events. It covers early intro, NOT all
worlds, arbitrary BeginTiling flag combinations or all geometry draw patterns.

Only the separate win-amd64-release-xenos plugin was replaced. Previous DLL
preserved beside it as rexgpu-xenos.pre-tiling-trace.dll. Diagnostic DLL SHA256:
6510FD6FE2198B426B591B0FBD23C22E67002092536D6DA5633E4A5739F5D2F5.
Xenos config unchanged before/after (SHA256
029E0D611203EA9C957912CB1D78544E4052851C2945CDCE5A3FC5D2FB131CD6).
Monitor2 was a command-line override. Native exe/config unchanged.

## Observed tile pair

Corresponding geometry draw in frame1: packet events1511/1678, draw
events1512/1679. VS microcode hash166D424A038E594A,
PS microcode hash9171C4BB046A5B2E, indices4680. These are Xenos microcode
hashes, not necessarily the native archive's container hashes.

- Tile0 window=(0,0), window scissor=(0,0)..(1280,512).
- Tile1 window=(0,-512), window scissor=(0,512)..(1280,720).
- Screen scissor remains (0,0)..(8192,8192) in BOTH, unlike the generic
  suggestion that PA_SC_SCREEN_SCISSOR necessarily changes per tile.
- Same viewport words: 44200000,44200000,C3B40000,43B40000,3F800000,00000000
  (X scale/offset640/640, Y scale/offset-360/360, Z1/0).
- Same VS constant-bank hash997FECFBBABA9FC1, PS158A1AC01E0263DC,
  fetch99F3D8E5B7316F19 for both corresponding draws.
- Tile0 maskFFFFFFFF/select80000003; tile1 mask8000000F/selectC.
  Predicated resolve commands for the other tile are observed and skipped.
- Frame2 repeats the same geometry pair (events3185/3352). Its banks differ
  from frame1 but match each other within that frame: VS45C987895919889D,
  PS9E20609B2AAD4F5F, fetchB75BF464743E03EA.

Thus these captured draws do not show a tile-dependent shader constant or
viewport adjustment. Equality of hashes is evidence for these banks/frames,
not proof for every draw or world.

## Resolve layout (frame1)

| Source | Format | Tile0 destination base | Tile1 destination base |
| --- | --- | --- | --- |
| RT0 | 6 / 8_8_8_8 | 1E140000 | 1E3C0000 |
| RT1 | 6 / 8_8_8_8 | 1DDA8000 | 1E028000 |
| RT2 | 6 / 8_8_8_8 | 1DA10000 | 1DC90000 |
| Depth (src4) | 22 / 24_8 | 1E4D8000 | 1E758000 |

Each second base is first+0x280000 (=1280*512*4). Destination pitch register
02D00500, decoded source rect tile0=(0,0,1280,512), tile1=(0,0,1280,208).
Decoded destination offset is0,0 in both; placement is encoded in the changed
base, not a nonzero decoded destination offset. Guest resolve vertices still
refer to Y511.5..719.5 for tile1; window offset maps source back to eDRAM top.
These are physical Xenos addresses and must not be hardcoded into native code.
Colour tile resolves have clear_color=1, clear_depth=0. Depth resolves have
both clear flags0. This does NOT mean depth is never cleared: clear draws are
also in the command stream, and their complete clear-state semantics were not
decoded by this trace.

Later in the SAME frame: full1280x720 HDR resolve (src0, format26, exp11,
number2) at event2269; full1280x720 depth resolve to1E4D8000 at event2271.
Do not treat all subsequent resolves as duplicate tile replays and discard them.

## Native gaps / review question

Native draw.cpp currently creates a PSO with renderTargetCount0/1 and uses
RT0. Xenos demonstrably resolves RT0,RT1,RT2 and depth in this pass. Bound RT
registers alone do not establish each pixel shader's output mask; this trace
does not log RB_COLOR_MASK or shader export masks. MRT support/output mapping
must be checked before claiming a full-size depth change fixes the scene.

BeginTilingHook currently logs rectangles and invokes ClearHook unless flags&1;
it does NOT promote surfaces or reproduce the guest tiling state. Its old
"using full host target" log is not evidence of an implemented full-size target.
EndTilingHook directly resolves the selected native surface. Original TU23
sub_83FBCD28 writes count+13124, rects+13128, aligned origins+13368,
extent+13556/+13560 and flags+13564, then enters original clear/worker/command
machinery. Native's deliberately minimal D3DDevice does not have that machinery;
calling the original wholesale is unsafe (same reason for SetPredicationHook).

Question for the peer reviewing TU23 (via user):
Can we isolate a CPU-only BeginTiling/EndTiling state contract from
sub_83FBCD28/sub_83FBD098, while retaining direct native draws and resolves?
Which fields must be set/reset, and which unhooked consumers would interpret
a nonzero +13124/+13564 as permission to access the absent Xbox command queues?
Need concrete consumer/function addresses and end-of-tiling reset semantics,
not a recommendation to call the full original or merely write count/extent.
Host-side full-size RT0–RT2/DS can then be implemented without accidentally
reactivating the old worker path. Do not implement speculative metadata writes.

Build verification: rexgpu-xenos target compiled successfully, bounded capture
completed normally, scoped SDK git diff --check passed. Native unchanged and
not re-tested in this investigation. No new performance benchmark claim.
