# Native renderer: performance, vsync-off and diagnostics (2026-10-10, afternoon)

Source: `rexlego/out/pr28-validation/source`, branch `codex/alen-pr28-local-test`,
on top of e69c242. **All changes below are uncommitted** in the working tree:
`rexlego/CMakeLists.txt`, `rexlego/src/sample_profiler.cpp` (new),
`gpu_native/device.cpp`, `hooks_device.cpp`, `sampler_state.h`, `textures.cpp`.
Build: `E:\devtools\ninja\ninja.exe legodimensions.exe` in `game-build`
(PowerShell + `E:\devtools\msvc\setup_x64.bat`; ninja is not on PATH).
The game now opens on **monitor 2** (`legodimensions.toml monitor = 2`;
`run-native1080-benchmark.ps1` checks this).

## Results (owner, 1080p, after all changes)

| Scene | Before today | Now |
|---|---|---|
| Upper hub | ~60 (vsync) | 100-150 |
| Lower hub | 15-20 | 60 |
| Oz level start | 13-20 | 30-40 |
| Simpsons world | 5-20 | 30-60+, with stutters |
| Main menu | ~30-40 | 30-40 (GPU-bound, see below) |

## What changed and why

### 1. Surface pool (the big win) - `textures.cpp`
Built-in sampling profile of Oz: the render thread spent ~40% in
`D3D12MA::CreateResource`/`AllocateCommittedResource`/`Release`, all from
`CreateSurfaceHook -> CreateSurfaceResource -> CreateHostTexture`. The game
creates and releases EDRAM surfaces many times per frame (free on Xenos), and
native made a fresh committed texture for each one (~1000/s).
- `~TextureResource()` hands a surface's texture to `surface_pool` (keyed by
  host width/height/format, at most 16 per key) after dropping its views and
  framebuffers. A TextureResource is destroyed only after `RetireResource`'s
  fence, so the texture is idle. `AllocateResource` takes from the pool first.
- **Recycled surfaces must be zero-cleared** (`pool_reused` -> clear colour 0
  or depth/stencil 0 in `CreateSurfaceResource`). The game relies on a new
  committed texture being zeroed; without the clear a white haze accumulates.
- The pool map is leaked on purpose (destroying it after the device crashes
  at exit). The log prints `surface pool hits=N misses=M` at powers of two
  (owner run: 262144 hits / 1396 misses).
- `LEGO_NATIVE_NO_SURFACE_POOL=1` disables it.

### 2. Unknown-resource waits skipped - `hooks_device.cpp`
`BlockUntilNotBusyHook` did a full `Synchronize(kResource)` (~14 ms/frame in
cutscenes). Logging showed every wait was on a resource native never
registered (game-side textures/VB/IB before first use). Such a resource cannot
be referenced by GPU work, so the wait is skipped.
`LEGO_NATIVE_WAIT_UNKNOWN_RESOURCES=1` restores it;
`LEGO_NATIVE_LOG_RESOURCE_WAITS=1` logs the first 400 waits with their class.

### 3. Pool-copy layout cache - `textures.cpp`
Simpsons profile: the game thread spent ~27% in `PoolCopyHook`, which scans
every texture per copy and called `GetGuestTextureLayout` each time.
`TextureDataOverlapsPoolCopy` now caches the base/mip extents per resource,
keyed on the fetch words + extent (mutable `layout_*` fields).

### 4. Vsync off + frame cap - `device.cpp`
- Plume defaults to vsync on and native never changed it, so native was pinned
  to the display rate. Vsync is now **off by default**
  (`LEGO_NATIVE_VSYNC=1` turns it on).
- Native had no frame limiter (`framerate_limit` is a GPU-plugin cvar, never
  registered in native mode). `LimitFrameRate()` before `PresentTexture`'s
  locks: high-resolution waitable timer plus a 0.5 ms spin. The value comes
  from `legodimensions.toml` through `ReadConfigInt`;
  `LEGO_NATIVE_FPS_LIMIT=N` overrides it (0 = uncapped).
- **Vsync-off crash, root cause:** `nvwgf2umx.dll` fast-fail `0xC0000409` at
  offset `EB48BD`, ~1 s after boot, only above ~120 fps (30/60/120 fine, 240
  and uncapped crash, with or without ALLOW_TEARING; a frame-latency-object
  wait did not help). The minidump stack showed
  `PresentTexture -> plume clearColor`: the FPS digit overlay clears dozens of
  rects in ONE `ClearRenderTargetView` on the swap-chain back buffer. Fix: one
  rect per `clearColor` call (`LEGO_NATIVE_FPS_OVERLAY_BATCHED=1` = old path,
  `LEGO_NATIVE_NO_FPS_OVERLAY=1` hides the counter). Verified: 240 cap 90 s and
  uncapped 60 s with no crash.
- `LEGO_NATIVE_PRESENT=<sync>,<flags>` calls `IDXGISwapChain::Present`
  directly, for experiments.

### 5. Anisotropy actually applied - `device.cpp`, `sampler_state.h`
`anisotropic_override` is also a plugin cvar that is never registered in native,
so `RegisterSampler` read "" (-1). It now falls back to the toml value (log:
`anisotropic_override=5 (from legodimensions.toml)`). The owner reports the Oz
road is **still smeared**, so the blur is mip-level selection, not
anisotropy (forcing mip 0/1 shows bricks).

### 6. Bloom investigation (not fixed) - `textures.cpp`
- Avengers PC exe strings: per-level `Bloom Threshold/Intensity (Last-Gen)` and
  `Increased 360 Bloom Range`, so bloom is a threshold bright-pass.
- Chain (from the 2026-10-08 snapshots): HDR 1920x1080 float ->
  ACEAF058/F184 (16_16_16_16, num_format=integer, fetch exp -13, resolve
  exp +11) -> 640x360 -> 320x180 -> 160x90 ping-pong. Native's integer packing
  gives sample = scene/4 (max 8).
- Xenia-style clamped UNORM packing (`LEGO_NATIVE_UNORM_RESOLVE=1`) makes the
  glow appear but **whites out the whole menu**. The default stays integer
  packing (`IntegerResolvePacking`). The glow is lost later in the chain. Next
  step: per-stage snapshot comparison against the emulated build at the main
  menu.

## Diagnostics added

- **Sampling profiler** `rexlego/src/sample_profiler.cpp`: with
  `LEGO_SAMPLE_PROFILE=1`, F9 starts recording (low beep) and F9 again stops
  it (high beep), writing `game-build/profile-<tick>.bin`. Every ~1 ms it
  suspends each thread and unwinds 24 frames via RtlVirtualUnwind; it
  allocates only after resuming. The linker writes `legodimensions.map`
  (`LINKER:/MAP` in rexlego/CMakeLists.txt). Analyse with
  `python analyze-profile.py game-build/profile-*.bin game-build/legodimensions.map`
  (in pr28-validation; per-thread self/inclusive). No PDB or WPA is needed.
- `Run-native1080-profile.cmd` / `Run-native1080-benchmark.cmd` reopen
  themselves minimised, wait for the game to exit, then close (`-Wait` in the
  ps1).
- Crash dumps without admin rights or a debugger: while WerFault is up, copy
  `C:\ProgramData\Microsoft\Windows\WER\Temp\*.mdmp`. Then scan the faulting
  stack for return addresses into the exe (minidump streams 3/4/6/9, map
  lookup): `python mdscan.py crashdumps/<dump>.mdmp game-build/legodimensions.map`
  in pr28-validation. Dumps from today are in `pr28-validation/crashdumps/`.

## Open, in priority order

1. **Main menu 30-40 fps is GPU-bound.** The render thread waits in
   `WaitForSubmissionLocked` (~16%). Use `LEGO_NATIVE_GPU_TIMESTAMPS` to find
   the expensive passes (portal/rings/full-screen effects at 1080p).
2. **Simpsons stutters.** Likely first-use pipeline/shader creation (FPS
   improves the longer you play). Confirm with a profile of a stutter, then
   consider a persistent pipeline cache.
3. Oz road blur: mip selection in the compiled shaders (gradients/LOD).
4. Missing bloom (see 6).
5. Leaves blinking: not investigated today.
6. Remaining render-thread CPU: per-draw texture binding (two
   `ResolveTextureResource` lookups + `RegisterSampler` mutex per slot),
   buffer hashing, the 8 KB constant byte-swap per draw.

## GPU pass profile (F10), added at the end of the session
`gpu_native/gpu_pass_profile.h`, hooked in draw.cpp (draws, indexed draws,
resolves; clears are not timed) and `SwapHook`. Press F10 in game: the next
frame is bracketed per pass with timestamps, the GPU is drained at the
following Swap, and `game.log` gets `GPU pass profile: N passes, X ms` plus the
top 40 groups (kind, VS/PS hash, guest viewport). Always compiled in; it costs
nothing until F10 is pressed. Sample from the Vorton cutscene: ~25 ms summed,
43 resolves 2.5 ms, shadow depth (960x960, PS=0) ~3 ms, otherwise spread out.
Summed per-pass time excludes gaps/barriers between passes. The main menu has
NOT been captured yet: press F10 there and compare.
