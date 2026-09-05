# LEGO Dimensions Toypad — working notes

Written 2026-08-23. Everything here is stuff that cost time to find out, so a
future session can skip re-deriving it. Facts about the code are true as of
commit `9c2133b` on the PR branch.

## What lives in this folder

| Path | What it is |
|---|---|
| `LegoToypad\` | Git clone of the companion app, on branch `feature/pad-shortcuts-story-mode-rebinding` |
| `build-ci\`, `build-ci\latest\`, `build-ci\latest2\` | `LegoToypad.exe` downloaded from CI runs (newest is `latest2`). `build-ci\LegoToypad.ini` is a real settings file from testing |
| `xbox\`, `dualshock4\`, `Switch Button Icons [Essential pack] 0.9\` | Source icon packs the icons were copied from |
| `old.rar` | Pre-existing archive, untouched |

The shadPS4 bridge work lives in a different folder:
`E:\Claude\LEGO Dimensions Toypad shadPS4 (dev)`.

## Repos and accounts

- **Upstream**: https://github.com/harrysof/LegoToypad (default branch `main`).
- **Fork used for PRs**: https://github.com/NeverCookFirst/LegoToypad-pr —
  a real fork. The older `NeverCookFirst/LegoToypad` is a **non-fork copy**, so
  GitHub refuses to open PRs from it; don't try again.
- Commits are authored as
  `NeverCookFirst <46416213+NeverCookFirst@users.noreply.github.com>`
  (git identity is set per-clone, not globally — set it after every fresh clone).
- **Open PR**: https://github.com/harrysof/LegoToypad/pull/7 — "Version 1.5",
  51 files, +1129/−114.

Related repos by the same user: `NeverCookFirst/shadPS4-Seamless-Toypad-Bridge`
(utility, release v1.1, video guide `98PDU0lcJ1c`) and
`NeverCookFirst/RPCS3-Seamless-Toypad-Build` (video guide `VkkL2L1ESCU`).

## Building — there is no local toolchain

This machine has **no MSVC/Visual Studio, no cmake, and no fontTools/Pillow**,
so the app cannot be built locally. Compile-verification goes through the fork's
own workflow:

```powershell
git -C "E:\Claude\LEGO Dimensions\LegoToypad" push fork feature/pad-shortcuts-story-mode-rebinding
gh workflow run build.yml -R NeverCookFirst/LegoToypad-pr --ref feature/pad-shortcuts-story-mode-rebinding
gh run list -R NeverCookFirst/LegoToypad-pr --limit 1 --json databaseId,status
gh run watch <id> -R NeverCookFirst/LegoToypad-pr    # ~5-8 min per run
gh run download <id> -R NeverCookFirst/LegoToypad-pr -n LegoToypad -D <dir>
```

The artifact is a ready-to-run `LegoToypad.exe`. Actions had to be enabled on
the fork once (`gh api -X PUT repos/.../actions/permissions -F enabled=true`).
Downloads fail with "The file exists" when the previous exe is still running —
check `Get-Process LegoToypad` and download into a new folder instead of killing
the user's session.

## How the app is put together

- One translation unit: `main.cpp` (~5000 lines, everything in an anonymous
  namespace) plus generated `GeneratedAssetTable.cpp`. Win32 + GDI+ + SDL2.
- `generate_assets.py` runs from CMake before every build and walks
  `All Bin Files\` and `Assets\`, emitting `resources.rc`,
  `GeneratedAssetTable.{h,cpp}` and `app.ico`. **Every** tag, portrait, logo,
  wallpaper, font and icon is embedded as a Win32 RCDATA resource — nothing is
  read from disk at runtime except `LegoToypad.ini` and an optional
  `gamecontrollerdb.txt`.
- Settings live in `LegoToypad.ini` next to the exe, sections `[Listener]`,
  `[Shortcut]`, `[Input]`, `[Web]`. Current `[Input]` keys: `SwapConfirmBackButtons`,
  `BackgroundIndex`, `StoryMode`, `ButtonConfirm/Back/Settings/MoveActive/QuickLoad/QuickClear`,
  `ButtonStyle`. `EnsureDefaultIniExists()` only writes the file when missing, so
  new keys never appear in an existing ini until something saves.
- Wire protocol to the emulator (unchanged, cross-repo contract with Cemu/RPCS3/
  the shadPS4 bridge): LOAD `0x01,pad,index,0,0` + 180 tag bytes + u16le path
  length + path; REMOVE `0x02,pad,index,0,0`; MOVE `0x03,destPad,destIndex,srcPad,srcIndex`.
  Pad 1 = centre, 2 = left, 3 = right; slots 0..6 in the 3/1/3 layout.
- Overlay is a fixed 900x610 layered WS_POPUP window. Sizes are hardcoded —
  anything added to a screen has to fit that height by hand.

## Gotchas found the hard way

- **LT/RT are axes, not buttons.** XInput's button word is full (0x0001..0x8000),
  so the mask type was widened to a 32-bit `ButtonMask` alias and the triggers got
  bits `0x10000`/`0x20000`, thresholded at 8000 of SDL's 0..32767. This is why
  rebinding to a trigger silently did nothing before.
- **`g_app.status` was never drawn on screen** — it only reached the web remote's
  JSON. Any "error" reported through it is invisible unless a screen renders it.
  The Settings screen now draws it, but only while capturing a button.
- **Settings layout is tight**: 14 rows at 33px pitch from y=100, icons 30px,
  capture line at y≈572. The window is 610px, and the wordmark band occupies
  roughly y=20..110, so rows can't start much above 100.
- **Nintendo face buttons**: SDL is asked for labels
  (`SDL_GAMECONTROLLER_USE_BUTTON_LABELS=1`), so `SDL_CONTROLLER_BUTTON_A` is the
  button *printed* A on a Switch pad even though it sits where Xbox has B. That is
  why the Switch column in `kButtonNames` keeps A/B/X/Y rather than swapping them.
- **No Background** works via `LWA_COLORKEY` with the key colour `RGB(8,0,8)`.
  Not pure black on purpose: antialiased text edges blend toward black and would
  punch holes in the UI.
- **Icon files** are `Assets\ControllerIcons\<Xbox|DualShock4|Switch>\<Button>.png`
  with positional (Xbox-style) names: `DpadUp/Down/Left/Right`, `Start`, `Back`,
  `LeftStickClick`, `RightStickClick`, `LB`, `RB`, `LT`, `RT`, `A`, `B`, `X`, `Y`.
  On Xbox One art, `Start` came from `Menu.png` and `Back` from `Windows.png`.
  A missing PNG falls back to a name pill — no build break. Still missing:
  `DualShock4\DpadLeft.png`.
- The button order in `kButtonNames` **must** match `controller_icon_buttons` in
  `generate_assets.py`; the index is used as the column into the icon table. The
  `ButtonStyle` enum order must match the style rows.

## Xenia (Xbox 360) toypad build — added 2026-08-25

- **Fork**: https://github.com/NeverCookFirst/Xenia-Seamless-Toypad-Build
  (fork of xenia-canary), branch `toypad`. Local clone: `xenia-canary\`.
  Built exe + test artifacts in `xenia-toypad-build\v1\`.
- Upstream xenia-canary has a `Portal` abstraction
  (`src/xenia/hid/portal/`) but only libusb *passthrough* of physical
  portals; no emulation existed. The X360 game talks to the toypad via
  `XamInputNonControllerGetRawEx/SetRawEx` with **device_id=6**, 32-byte
  buffers (resolved at runtime — the xex import table has no NonController
  entries, don't be fooled by it).
- **X360 frame wrapping**: the game wraps every standard toypad frame as
  `0B 16 <0x55-frame>` on writes (0x16 constant). Our `EmulatedToypad`
  auto-detects the offset of 0x55 and mirrors the prefix on replies with
  byte[1] = actual frame length — the game accepts this. Raw (PS3-style)
  frames also still work, offset 0.
- New code: `src/xenia/hid/portal/emulated_toypad.{h,cc}` — port of RPCS3's
  `dimensions_toypad` (TEA crypto, RNG challenge, D2/D3/D4 tag ops) + the
  same TCP listener as the RPCS3 build (127.0.0.1:9191, LOAD/REMOVE/MOVE,
  `XENIA_TOYPAD_PORT` env override) + picker watcher for
  `Local\CemuToypadPickerInputActive` (gamepad is neutralized in
  `InputSystem::GetState`/`GetKeystroke` while the overlay is open).
  Cvar `toypad_emulation` (default true) picks emulated vs hardware portal.
- **CI**: no local toolchain still. `.github/workflows/Toypad_build.yml`
  (push to `toypad` / manual) reuses upstream `Windows_x86.yml`; skips the
  lint gate. ~25 min per build. Artifact is uploaded with `archive: false`,
  so `gh run download` FAILS ("not a valid zip") — download
  `actions/artifacts/<id>/zip` with curl + auth token; the response body is
  the raw `.7z` despite the /zip endpoint name.
- Windows.h gotcha: define `WIN32_LEAN_AND_MEAN`/`NOMINMAX` before
  winsock2.h or `std::min` breaks (cost one CI cycle).
- **Verified end-to-end** (2026-08-25): game boots with the emulated pad,
  full handshake (B0 wake → C0 → B1 seed → B3 challenge → C6 fades), TCP
  LOAD of `Batman.bin` → game receives the 0x56 add event and issues D2
  page reads + D4 model query. Run with
  `--content_root=E:\xenia\content` to reuse the user's installed data.
  The LegoToypad app needs no changes (same port/contract).
- Not yet verified: in-game figure visuals/gameplay (needs a human with a
  controller), MOVE/REMOVE in-game behavior, .bin persistence writes.

### Performance panel — 2026-08-30

- `perf_monitor` now defaults to **false**; nothing is sampled until asked.
- The panel is its own top-level **Performance** menu (no longer under Mods)
  and carries a "Measure performance" checkbox that starts/stops the sampler
  at runtime via `PerfMonitor::SetEnabled` and saves the cvar, so no config
  edit + restart is needed. Each sampler run clears the previous history —
  the thread restarts its clock, so mixing timelines would be wrong.
- Hotkey moved from `P` (a letter the game itself uses — the panel kept
  popping up mid-play even for users who had disabled it) to **Right Shift**.
- Win32 gotcha for that: Windows reports both shift keys as `VK_SHIFT`, so
  `Win32Window::HandleKeyboard` now recovers the side with
  `MapVirtualKey(scancode, MAPVK_VSC_TO_VK_EX)` and delivers `kLShift`/
  `kRShift`. `ImGuiDrawer::OnKey` accepts all three for `io.KeyShift`; the
  `is_shift_pressed` modifier flag is unchanged.
- `Start`/`Shutdown` are serialized by `lifecycle_mutex_` — the GPU thread
  calls `Start()` on every swap while the UI thread may be shutting down.

### Graphics artifacts — SOLVED 2026-08-25 (config, not code)

- Symptom: menus/cutscenes/terrain accumulate rainbow/checkerboard garbage
  within ~30 s. NOT an alt-tab bug and NOT a base-revision mismatch (the
  user's "known-good" exe was the same canary_experimental@1e834f8a8).
- Root cause: the TT Games engine reads resolved frames back on the CPU
  (HDR eye adaptation) and re-injects them into lighting. With readback
  disabled it gets garbage that compounds every frame. Same class of issue
  is documented for the whole LEGO family in xenia-canary game-compat
  (issues #1141, #1166 — label `gpu-readback`).
- Fix (validated 9+ min of real gameplay incl. heavy alt-tabbing, clean):
  `readback_resolve = "fast"` + `readback_memexport = true`, keep
  `render_target_path_d3d12 = "rtv"` (or empty/default).
- **ROV path hangs LEGO Dimensions on the loading screen** (endless DLC
  STFS re-enumeration in the log) — do not use `"rov"` for this game.
- Do NOT use Vulkan: memexport readback is broken on canary Vulkan.
- Gotcha: xenia rewrites the config on exit (`config::SaveConfig` in
  OnDestroy), which can silently revert hand-edited values if the exe was
  running while you edited — edit only while xenia is closed.
- Crash lead: WER logged RADAR_PRE_LEAK_64 (runaway memory) for the exe;
  the emulated toypad's response queue was unbounded while the game spams
  LED fades without draining replies. Capped at 256 frames in commit
  `b4fcf29b` ([HID] Cap emulated toypad response queue). Gameplay memory
  is stable ~2.7 GB private after the readback config.
- Both `E:\xenia` and `xenia-toypad-build\v1` configs updated with the
  validated settings. Log noise that is safe to ignore: `ResolvePath
  (update:) failed` (no title update installed; TU not present in the
  Complete Pack — only DLC, content type 00000002).

### DLC + Title Update — WORKING 2026-08-25

- DLC packages were installed all along; they were dead because
  `license_mask = 0`. Set `license_mask = -1` in the config (both installs).
- TU23 (final title update, needed for later DLC waves) is NOT in the
  Complete Pack. Downloaded from archive.org item `LEGODIMENSIONSDLC`
  (`Lego Dimensions Extras/000B0000/tu00000003_00000000`, 2,069,790,720
  bytes, game Media ID must be 72B1DD2A) into
  `content\0000000000000000\5752084B\000B0000\`. Log confirms:
  "XEX patch applied successfully: 0.0.0.3 -> 0.0.23.3".
- Post-TU the game re-installs its data (17 MB, then 536 MB). That
  installer needed THREE emulator fixes (commits on `toypad`):
  1. `4b2c5bcb` — stub `IoDismountVolume*` as success (finalize step).
  2. `a9228086` — write directory-package header at creation, not close
     (installer enumerates its new `appdata` package while still open).
  3. `0190972f` — return `ERROR_ALREADY_EXISTS` (183, per XDK) instead of
     `ERROR_INVALID_PARAMETER` when a content root name is already mapped
     (installer re-opens its open package to validate and expects 183).
- If an install attempt fails midway, DELETE the partial
  `content\0000000000000000\5752084B\00000001` (+ `Headers\00000001`)
  before retrying — leftovers make the next attempt fail immediately.
  `00000001.bak` there is a pre-TU install backup, safe to delete later.
- Verified working by the user: install completes, update nag gone, DLC
  available in-game on a fresh save.
- **Open**: the pre-TU save (`content\E03000002BAF493E\5752084B\00000001\
  savegame_1`) black-screens on load under TU23 (game alive, renders
  nothing; waited 2+ min). Backup at `...\5752084B\savegame_1.backup-preTU23`.
  User started a new save in slot 2.

### Release v1.0 — published 2026-08-25

- https://github.com/NeverCookFirst/Xenia-Seamless-Toypad-Build/releases/tag/v1.0
  (repo is public; asset = exe + tuned config + LICENSE + portable.txt,
  built from `0bc63b8a`). Release notes contain the full user-facing
  install guide (game/DLC/TU via Internet Archive search queries, no
  direct links) and the 60 FPS / frame-gen (LSFG) note per the user.
- New UI in this build: `Display > Internal Resolution` (1x/2x/3x,
  applies next launch) and `Display > Toggle 60 FPS Unlock` (vblank
  120 Hz, applies live — frame limiter now re-reads cvars per tick).
  User-verified: 60 FPS works great in-game.
- Gotcha: `OVERRIDE_*` cvar macros only work in the defining translation
  unit; cross-module overrides go through the `cvar::ConfigVars` registry
  by name (see `OverrideConfigVarByName` in emulator_window.cc) — cost
  one CI cycle.
- CI gotcha: pushes to `toypad` trigger the upstream workflow with the
  clang-format lint gate (fails, skips build) AND duplicate runs — always
  cancel push-runs and dispatch `Toypad_build.yml` manually.

## Open items

1. **Icon licences.** `Assets\ControllerIcons\SOURCES.md` records provenance:
   Xbox/DualShock4 file names match Xelu's CC0 prompt pack; the Switch pack's
   licence is **unconfirmed** and was flagged in the PR body.
2. `DualShock4\DpadLeft.png` still missing (only shows if the D-pad is part of
   the overlay toggle chord).
3. PR #7 is awaiting harrysof's review.

## User preferences worth remembering

- Communicates in Russian; wants the actual outcome first, not a play-by-play.
- Verifies builds personally — don't open PRs or publish releases until asked.
- Release notes/PR text should follow harrysof's own style: `### Section`
  headings with `- ` bullets separated by blank lines (see upstream v1.3 notes).
