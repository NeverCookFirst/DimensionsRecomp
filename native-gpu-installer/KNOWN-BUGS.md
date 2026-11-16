# NativeGPU known bugs

## Later owner reports

- **Open** — Blue particles missing on character swap. F10 captured resolves only.
- **Open** — First Toy Pad placement per run is slower for characters and vehicles.
- **Mitigated, gameplay verification pending** — Midway Arcade to Vorton crash. The local guard skips the observed invalid free. The cause of the heap corruption remains open.
- **Open** — Shard travel crashed after Bloom and VSync changes were saved. The crash occurred during guest cleanup.
- **Owner verified** — Year 2 thumbnails and Ethan Hunt's HUD border and hearts returned with synchronous pool copies.
- **Owner verified** — The Ghostbusters canal water fix worked in the reported scene.
- **Pending** — Full shader and pipeline coverage, physical AMD testing, and Linux parity.

## Tester reports as of November 5

### TEST-01 — Adventure Time entry access violation

**Open**

Original Adventure Time invalid stream pointer remains unexplained; no null-skip patch. Prior reference-count/copy-order fixes do not establish closure.

### TEST-02 — Vulkan cutscene/character change freezes

**Partial ui corrected freeze open**

Native GPU F4 no longer offers ineffective SDK gpu_backend/d3d12_adapter controls. This native device is D3D12 and has no Vulkan implementation. Exact current-bank relink and deployment verified; cutscene/character freeze root causes remain open.

### TEST-03 — Game remains alive after a crash

**Implemented component verified pending user**

Current crash monitor now retires its own child after a confirmed fatal capture attempt. Eight prior cases and explicit old/new stuck-child control pass. Large captures may still delay closure.

### TEST-04 — Portal 2 adventure world/Shard broken geometry

**Partial pending investigation**

V41/V42 gather and shader arithmetic corrections are present; Portal2/Shard draw attribution and world acceptance remain missing.

### TEST-05 — Sonic terrain, Emerald Coast and cutscene corruption

**Partial pending investigation**

V41/V42 input and arithmetic corrections are present; Sonic terrain/pipes/palms/cutscene world results still need evidence.

### TEST-06 — Adventure Time character/material corruption

**Open**

No failing Adventure Time character/material draw identified.

### TEST-07 — Main menu black geometry after travelling

**Open**

Post-travel main-menu resource corruption requires stronger ownership/data evidence.

### TEST-08 — Bloom overexposure in Sonic and Doctor Who

**Open**

Base-lighting and bloom overexposure still require reference/material comparison; no global darkening.

### TEST-09 — Brightness control has no visible effect

**Implemented offline pending user**

Title gamma LUT path is implemented and tested offline in V39+. Tester brightness endpoints still need acceptance.

### TEST-10 — Original cel outlines missing

**Open**

Cel-outline selection exists; missing outlines remain unproven as repaired.

### TEST-11 — Missing blue transformation/relic-scanner particles

**Open**

Missing transformation/scanner particle draw attribution remains open.

### TEST-12 — Vortech cutscene face/eye texture corruption

**Open**

Vortech face report has conflicting hardware outcomes; root cause not established.

### TEST-13 — Wide/stretched character report

**Open**

Wide character stream/skinning cause remains open.

### TEST-14 — Toy Pad app not registering

**Partial component fix pending user**

Reproduced in accepted DLL: an incomplete LOAD holds the shared command lock and blocks another client LED query. Current DLL reads payloads before locking, keeps LED IO independent of placement, cancels pending Winsock IO, transfers socket ownership during shutdown and waits for workers. Actual DLL LOAD/guest event/MOVE/REMOVE/LED-during-MOVE and partial-client shutdown pass; 40 immediate start/stop cycles have no handle growth. Tester-specific registration cause/acceptance remains pending; no world-travel fix implied.

### TEST-15 — Year 2 progress missing after save conversion

**Open missing source save**

Affected tester original/converted saves are absent. Actual converter read/write on an existing complete backup preserves all 31 progress files (V2GAME01 + 30 DLC) bit-for-bit and leaves every source file unchanged. Generic missing-V2/DLC conversion is not reproduced; tester-specific failure remains open pending source pair.

### TEST-16 — Half normal/half Super Sonic after death

**Open**

Half-Super-Sonic death lifecycle needs enabled-mod and guest state comparison.

### TEST-17 — Simpsons black spot

**Open**

Simpsons black spot remains a renderer/shadow/depth investigation.

### TEST-18 — Settings selection needs clearer feedback

**Implemented component verified pending user**

Current native Video/Effects highlight the selected label/value with a yellow RGB pulse and dim inactive FSR-dependent rows. Shared staged-value rules reject inactive edits while preserving values. 59 frontend scenarios/127 adjustment steps and 104 translated-bridge scenarios/511 input masks pass. Native alpha, navigation and sound path retained; actual menu appearance/gameplay acceptance pending.

### TEST-19 — Debug panel overlaps HUD and is not movable

**Implemented component verified pending user**

November 2 owner reproduction disproved previous cursor/drag acceptance. Current host eca2b3ab shows the pointer across the client area for any overlay and releases mouse look while overlays are open. F3 left-title drag no longer requires Ctrl; passive input, release, clamp and focus loss pass real ImGui checks. Gameplay acceptance pending.

### TEST-20 — World/vehicle performance stalls

**Open**

World/vehicle stalls remain unaccepted; saved-frame analysis alone does not prove a stutter fix.

### TEST-21 — Installer/import instructions confusing

**Partial guidance corrected**

Replaced obsolete current-folder README/start instructions with normal Play/report tools and exact ReXGlue TOML versus extracted disc/TU23/mods examples. Original small documents preserved in research. Installer browse-dialog changes and tester setup acceptance remain pending.

### TEST-22 — Manual dump falsely classified as crash

**Implemented component verified**

Manual dump-result FATAL notifications are excluded. Current helper passes manual-dump, handled exception and split fatal regression cases.

### TEST-23 — Large/slow report and full-dump capture

**Implemented component verified pending user**

Current support host bounds compact log excerpts to 8 MiB per file and 64 MiB archive source bytes, records omitted ranges, bounds event filtering, and prioritizes setup/settings. 36 checks and both real report CLI modes pass. Full reports/local originals remain complete; real full dumps may still take time.

### TEST-24 — Doctor Who/TARDIS intermittent freeze

**Open**

Doctor Who/TARDIS freeze has conflicting outcomes. No universal travel fix claim.

### TEST-25 — Toy Pad LEDs disabled in fresh companion

**Resolved by tester preference**

Tester confirmed LED setting resolves this report; no renderer work required.

### TEST-26 — Ninjago loading concern

**Retracted unconfirmed**

Immediate tester retraction; no separate confirmed bug to invent a fix for.
