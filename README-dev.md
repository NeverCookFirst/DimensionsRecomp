# Building Dimensions Recompiled

Read this before cloning and expecting `cmake` to work. It will not.

## The one thing to understand first

**A clean clone of this repository cannot build the game.** That is on purpose,
not an oversight.

The bulk of the recompiled game is roughly 470 MB of C++ across ~720 files that
was machine translated from the original Xbox 360 executable. That translation is
a derivative of copyrighted code, so it is not distributed here and never will be.
`rexlego/CMakeLists.txt` starts with `include(generated/rexglue.cmake)`, and
without that folder CMake stops immediately.

You generate it yourself, from your own copy of the game, in step 3 below. Once it
exists, everything else builds normally.

What this repository actually holds is the ~1 MB that is ours: the game specific
runtime code, the installer and updater, the mod tooling, and the build glue.

## What you need

**Your own game data.** Nothing here ships any.

- An extracted Xbox 360 disc dump of LEGO Dimensions, with `Default.xex`,
  `GAME*.DAT` and `GAME*.HDR` in it. Title ID `5752084B`, media ID `72B1DD2A`.
- Title Update 23, either as the raw STFS package or already extracted. The
  installer verifies it by hash, so an older TU will be rejected.
- Optionally the DLC packages, as STFS containers.

**A toolchain.**

- CMake 3.25 or newer, and Ninja.
- Clang 17 or newer. The presets ask for `clang` / `clang++` on PATH. MSVC's
  headers and libs are still needed on Windows, so run from a Visual Studio
  developer environment (or any environment where `windows.h` resolves), even
  though MSVC itself does not do the compiling.
- .NET 8 SDK, for the installer, the updater and the mod CLI.
- Roughly 40 GB of free disk: the generated sources, the object files, and a test
  install add up faster than you would think.

**The ReXGlue SDK**, which is a separate project. It provides the runtime: GPU
translation, kernel, audio, input, the settings overlay. Upstream is
[`rexglue/rexglue-sdk`](https://github.com/rexglue/rexglue-sdk). This project
tracks a fork with the Toy Pad work in it.

## Repository map

The working folder this repository lives in is around 20 GB, of which about 1 MB
is code we wrote. So `.gitignore` ignores `/*` and then names what is ours. The
rule that decides what goes in: **anything that came from the game, or is
generated from it, stays out.**

| Path | What it is |
|---|---|
| `rexlego/` | the game target: our sources, CMake, mods menu, Discord presence, update check |
| `rexlego-installer/` | the installer **and** the updater, one C# project producing one binary |
| `DimensionsModManager-CLI/` | the command line mod tool shipped as `tools\modcli` |
| `xexdump/` | XEX inspection utility, used while reversing |
| `tools/` | odds and ends used during development |
| `docs/` | artwork used by the README |
| `VERSION` | the version the build script stamps into a release |
| `rexlego-installer/releases/` | one manifest per shipped release, needed to build the next update pack |

Deliberately not here: game data, dumps, the SDK checkout, and the sibling
projects listed at the bottom of the main [README](README.md).

## Step 1: get the SDK and the sibling repos

The SDK and three smaller repositories are git submodules, so one clone brings
everything:

```bash
git clone --recursive https://github.com/NeverCookFirst/DimensionsRecomp.git
```

Already cloned without `--recursive`? `git submodule update --init --recursive`
fixes it.

The SDK submodule tracks **our fork, on the `toypad-ui` branch** - not upstream.
That branch carries the Toy Pad work, the codegen fix behind the crash on ropes,
discrete GPU selection, keyboard input and the crash logging. Upstream builds
fine and then behaves differently from every release, which is a miserable thing
to debug.

Build and install it. **Use the presets:**

```bash
cmake --preset win-amd64 -S rexglue-sdk
cmake --build rexglue-sdk/out/build/win-amd64 --config Release --target install
```

Two things that are not optional:

- **`--config Release`.** The SDK builds with Ninja Multi-Config, and a plain
  `cmake --build` defaults to RelWithDebInfo, which produces `rexruntimerd.lib`.
  The game links `rexruntime.lib`, the Release one. Getting this wrong gives a
  link error that does not explain itself.
- **The x86 baseline.** The SDK byte-swap helpers use SSSE3 intrinsics, and
  clang's bare `x86-64` default is SSE2 only. A configure that sets no `-march`
  at all used to fail with `always_inline function '_mm_shuffle_epi8' requires
  target feature 'ssse3'`. The baseline now lives in the SDK's `CMakeLists.txt`
  (`REXGLUE_X86_BASELINE`, default `x86-64-v2`) rather than only in the presets,
  so a preset-less configure works too - but the presets are still the tested
  path.

### Building for a CPU without AVX2

The shipped builds target `x86-64-v3` (Haswell / Zen 1 or newer) because the
recompiled code is where nearly all the time goes. On an older CPU - Ivy Bridge,
Sandy Bridge, Bulldozer - lower the baseline in **both** halves, or the two
disagree about AVX2 and the game dies on an illegal instruction:

```bash
cmake --preset win-amd64 -S rexglue-sdk -DREXGLUE_X86_BASELINE=x86-64-v2
# ... and in step 4:
cmake -S rexlego -B rexlego/out/build/win-amd64-release \
      -DLEGODIMENSIONS_X86_BASELINE=x86-64-v2 ...
```

`x86-64-v2` (Nehalem / Bulldozer, SSE4.2) is the floor that still builds; plain
`x86-64` does not, because of the SSSE3 helpers above. Passing
`-DCMAKE_CXX_FLAGS=-march=...` instead does **not** work for the game target:
`target_compile_options` land after `CMAKE_CXX_FLAGS` on the command line, so the
project's own value would win. That is what the cache variables are for.

## Step 2: point the game at the SDK

With the submodule in place the default works as-is:

```
-DREXSDK_DIR=../rexglue-sdk
```

That builds the SDK as a subproject of the game, which is what you want while
changing SDK code. The alternative is `find_package(rexglue CONFIG)`, which needs
the SDK already installed and on `CMAKE_PREFIX_PATH`; the game refuses to
configure if neither works.

Use the SDK from this checkout, not a `rexglue` that happens to be on `PATH`: a
different SDK build produces a runtime that does not match the generated sources.

On Windows, paths copied out of Explorer come with `\` separators. CMake wants
`/` (or doubled backslashes) - a single `\` silently eats the next character.

## Step 3: generate the recompiled sources

Copy the manifest template and point it at your dump:

```bash
cp rexlego/legodimensions_manifest.toml.example rexlego/legodimensions_manifest.toml
```

Edit `game_root` and `entrypoint.file_path` so they point at your extracted disc.
Paths are relative to the manifest file. The real `legodimensions_manifest.toml`
is gitignored precisely because it holds paths specific to your machine.

**Put Title Update 23's `Default.xexp` next to `Default.xex` in your dump first.**
The recomp is generated from the *patched* executable. The loader looks for a
sibling file whose name is the XEX's name plus `p` (`user_module.cpp`: it resolves
`path + "p"`), so `default.xex` needs `default.xexp` beside it, matching case, and
applies the patch itself. You do **not** need xextool. You will see it work:

```
XEX patch applied successfully: base version: 0.0.0.3, new version: 0.0.23.3
  Version:  0.0.23.3
```

If that line says `0.0.0.3`, the patch was not found and every address the
analyser reports will be for the wrong image.

Then run the codegen tool from the SDK build:

```bash
cd rexlego
../rexglue-sdk/out/install/win-amd64/bin/rexglue codegen legodimensions_manifest.toml
```

This is the long step. It analyses the XEX, finds functions, and writes
`rexlego/generated/`. Expect a few minutes and about 470 MB. Add `--ignore-stamp`
to force a regeneration when it decides everything is already up to date.

`legodimensions_config.toml` sits next to the manifest and is included by it. It
carries the hand written hints the analyser needs: function boundaries it could
not infer, addresses to skip, and so on. That file **is** committed, because it is
our work rather than the game's.

## Step 4: build the game

```bash
cmake -S rexlego -B rexlego/out/build/win-amd64-release \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_CXX_COMPILER=clang++ \
      -DREXSDK_DIR=../rexglue-sdk
cmake --build rexlego/out/build/win-amd64-release --target legodimensions
```

Or use the presets in `CMakePresets.json` (`win-amd64-release` and friends) if you
have the tools on PATH.

The first build compiles those ~720 generated translation units and takes a while.
Incremental builds after that are quick. The configure prints the baseline it
chose, which is worth a glance:

```
-- legodimensions x86 baseline: -march=x86-64-v3
```

Output lands in the build directory: `legodimensions.exe`, plus `rexruntime.dll`
and `rexgpu-xenos.dll` copied from the SDK.


### Running it from a build tree

The game reads `legodimensions.toml` from its working directory. At minimum set:

```toml
game_data_root   = 'path/to/your/extracted/disc'
update_data_root = 'path/to/your/extracted/TU23'
user_data_root   = 'path/to/where/saves/should/live'
```

Everything else has a default. Press F4 in game for the full settings list, which
is generated from the runtime's cvar registry, so anything you add shows up there
automatically.

## Step 5: build the installer

```bash
cd rexlego-installer
dotnet build Setup.csproj -c Release
```

That gives you a runnable installer without a payload, which is enough for
debugging the wizard. To produce a real release, see [RELEASING.md](RELEASING.md):
`build-installer.ps1` publishes the single file host, stages the game binaries,
appends the payload, and writes the update pack and the release manifest.

```powershell
cd rexlego-installer
powershell -ExecutionPolicy Bypass -File .\build-installer.ps1
```

### What the release build needs on disk

The script does not build anything but the C# projects, so have all of this
ready first. It names whatever is missing and how to get it, rather than failing
on a bare "missing file":

| What | Where it must be | If it is not there |
|---|---|---|
| `legodimensions.exe`, `rexruntime.dll`, `achievement_unlocked.wav` | the game build dir (`-GameBuild`, default `rexlego\out\build\win-amd64-release`) | do step 4 |
| `rexgpu-xenos.dll` | the SDK output tree (`-SdkDir`, default `..\rexglue-sdk`) | `cmake --build rexglue-sdk/out/build/win-amd64 --config Release --target rexgpu-xenos` |
| `FiraSans-Regular.ttf` | `rexlego\res` | it is committed; check your `-GameBuild` / `-SdkDir` |
| the mod manager's `mods` folder | `DimensionsModManager\` or `DimensionsModLoader\` next to this repo | submodule, or clone it (see step 6) |
| `ModCli.csproj` | `DimensionsModManager-CLI\` | it is in this repository. **Required**, not optional: the script publishes it every run |
| `DimensionsSaveConverter.exe` and `READ ME FIRST.txt` | `DimensionsSaveConverter\` | submodule. The exe is prebuilt in that repo - there is no project to build |
| the Toy Pad app | downloaded from its own latest GitHub release, every run | needs network |

The `rexgpu-xenos.dll` and font rows used to mean copying files into the build
directory by hand, with nothing saying so. The script now looks where those files
actually live.

The Russian translation is two ordinary mods from the mod manager repo. If your
checkout does not have them, drop them from `$RussianMods` in the script and the
wizard simply stops offering that component.

The script takes the game binaries from `rexlego\out\build\win-amd64-release` by
default. Override with `-GameBuild`.

## Step 6: build the mod CLI

`modcli` is the tool the in game mod menu shells out to. Its project compiles
three files out of the mod manager, which is a different repository:

```
<Compile Include="..\DimensionsModManager\ModEngine.cs" />
```

The submodule already puts it there. Cloning by hand instead? The repository's
GitHub name is `DimensionsModLoader` while everything here calls it
`DimensionsModManager`. `build-installer.ps1` accepts either folder name, but
`ModCli.csproj` reaches for `..\DimensionsModManager\ModEngine.cs` by relative
path, so clone it under that name:

```bash
git clone https://github.com/NeverCookFirst/DimensionsModLoader.git DimensionsModManager
```

Then:

```bash
cd DimensionsModManager-CLI
dotnet build ModCli.csproj -c Release
```

Skip it if you are not touching mods — the game runs without `modcli` and only
complains when you press Apply in the mod menu.

## How much of this has actually been walked

These steps were originally written from a machine where everything was already
in place. In September 2026 someone built the whole thing from a clone on an Ivy
Bridge CPU with no AVX2 and wrote up every place the guide was wrong; that report
is what the steps above now say. Fixed since: the missing `--preset` and with it
the SSSE3 compile failure, the hard-coded `-march`, the unexplained `.xexp` step,
and the undocumented prerequisites of the release build.

Still unwalked: Linux and macOS. The SDK targets them, this game has only ever
been built on Windows. Treat a snag there as a gap in this document rather than
something you did wrong, and open an issue.

## Things that have cost real time

Written down so nobody has to rediscover them.

**A space in the path breaks Ninja dependency tracking.** If the checkout lives
somewhere like `C:\My Projects\...`, the link line in `build.ninja` splits at the
space, and Ninja never notices that an SDK library changed. The build reports
success and relinks nothing. Object outputs are fine because their paths get
sanitised; it is the library and source edges that break. Either avoid spaces in
the path, or verify the artifact after every build.

**Verify the artifact, never the build log.** After a change that should be
visible, look for a string you just added in the actual binary rather than
trusting "Build succeeded":

```powershell
$b = [System.Text.Encoding]::ASCII.GetString([System.IO.File]::ReadAllBytes($exe))
$b.Contains("your new cvar")
```

**Know which binary your change lands in.** SDK graphics code goes into
`rexgpu-xenos.dll`, most other SDK code into `rexruntime.dll`, and `rex_app.cpp`
is compiled into `legodimensions.exe` itself. Changing an SDK file and only
copying the exe means you are still running the old code.

**`rexgpu-xenos.dll` does not get refreshed in the build directory.** It is
copied in from the SDK by a CMake rule, and that is exactly the kind of edge the
space in the path breaks: the SDK rebuilds the plugin, the game build reports
success, and the copy next to `legodimensions.exe` stays whatever it was. This
cost a whole debugging round on 2026-09-17 - a cvar change that was provably in
the SDK's DLL and provably not in the game's. After touching SDK graphics code:

```powershell
Copy-Item rexglue-sdk\out\win-amd64\Release\rexgpu-xenos.dll `
          rexlego\out\build\win-amd64-release\ -Force
```

Then check the string is really there, the same way as above. `build-installer.ps1`
warns if it is asked to ship a build-directory copy older than the SDK's.

**A cvar must be defined in the same library that reads it.** `REXCVAR_DEFINE` in
one target and `REXCVAR_GET` in another fails to link with
`undefined symbol: FLAGS_<name>_storage_`. If you genuinely need to read a cvar
from another module, go through the registry rather than the generated symbol.

**Adding a source file triggers a CMake re configure**, which will fail if the
tools are not on PATH the way they were when the build directory was first
created. A failed re configure also blanks `CMAKE_BUILD_TYPE`, and the next build
compiles as Debug and dies at link on an `_ITERATOR_DEBUG_LEVEL` mismatch against
the Release SDK. If that happens, re run the configure command from step 4
explicitly rather than letting the implicit one run.

**The game rewrites `legodimensions.toml` when it exits**, from the cvar values
it holds, and drops keys whose value is empty. Edit the file with the game
closed, and check your edit survived the next exit before concluding it did
nothing.

## Digging into the title itself

Three things make the guest answer questions it used to swallow.

**`MISSING-FUNCTION` names the caller.** The target address is almost always a
vtable dispatch thunk, so on its own it only says "some slot". The `lr` in the
message is the game function that made the call. Set
`invalid_function_log_every` when you need repeats: by default each address is
reported once per process, which hides the skip you are hunting if that address
came up earlier in the session.

Most of these come from one table of 16-byte thunks shaped

```
lwz   r12, 0(r3)        ; the vtable
lwz   r11, <slot>(r12)  ; the method
mtctr r11
bctr
```

A full-image scan for that shape finds 324 of them; they are declared in
`legodimensions_config.toml` under `[functions]`. If a new one ever turns up,
add it, delete `rexlego/generated/default/codegen.stamp`, and rebuild.

**`threads` and `hangwatch` in the console** say which guest threads are
running. A statically recompiled build has no program counter to sample, but
`lr`, `last_indirect_target`, `r1` and `ctr` together do the job: a tuple that
is bit-for-bit identical for tens of seconds is not executing guest code. When
a thread is stopped, its `lr` points at the *return*, so the call is at `lr-4`;
addresses around `0x8411xxxx` are the import table, and
`generated/default/legodimensions_register.cpp` maps them to kernel names.

**`STUCK-LOCK`** appears when `RtlEnterCriticalSection` has waited longer than
`critical_section_stuck_seconds`, naming the section, the owner and the counts.
An owner of 0, or one naming a thread that has exited, means the lock leaked.

**`trace_file_opens`** logs successful opens too, not just failures - the way to
tell "asked for the level and was refused" from "never asked".

## Fixing bugs that live in the data

Not every title bug is in code. Some are a script line the developers commented
out. Editing the player's own archives to correct that is intrusive and fights
with the installer's content verification, so the runtime rewrites the bytes as
they are read instead: see `rex/system/file_fixups.h` in the SDK, and the
built-in fix registered in `rexlego/src/mod_menu.cpp`.

A fixup must check what it is about to overwrite and do nothing when the bytes
are not what it expects. That is what makes it a no-op on a different update or
on data somebody has already patched, instead of damage. Built-in fixes appear
in the mod menu greyed out and locked, and are turned off in the config rather
than by a checkbox, because without them the stock game is broken.

## Shipping a large mod: use a free archive slot

The game already mounts extra archives - no code needed. At every start it
probes twenty slots on the disc:

```
OPENED  d:\INSTALL0_.HDR   /  .DAT
OPENED  d:\INSTALL0_0.HDR  /  .DAT
OPENED  d:\INSTALL0_1.HDR  /  .DAT
FAILED  d:\INSTALL0_2.HDR            <- first free slot
FAILED  d:\INSTALL0_3..18.DAT
```

Headers are read in order until one is missing. A vanilla install uses three,
leaving **seventeen free**: drop a `DAT`/`HDR` pair in as `INSTALL0_2.*` and the
game reads it. The update device has room too - `update:\PATCH3.*` mounts, and
the update device outranks the disc, which is where to put a file that has to
override a base one.

Verified with a 149 MB third-party archive: both devices mounted it, the probe
carried on to the next slot, and the session logged no errors.

This is the route for anything that adds files. The mod CLI patches in place at
the same length and cannot add entries at all.

## Sibling repositories

These live in the same working folder during development and are separate
repositories:

| Folder | Remote | Branch |
|---|---|---|
| `rexglue-sdk/` | upstream [rexglue/rexglue-sdk](https://github.com/rexglue/rexglue-sdk), fork [NeverCookFirst/rexglue-sdk](https://github.com/NeverCookFirst/rexglue-sdk) | `toypad-ui` |
| `xenia-canary/` | upstream [xenia-canary/xenia-canary](https://github.com/xenia-canary/xenia-canary), fork [Xenia-Seamless-Toypad-Build](https://github.com/NeverCookFirst/Xenia-Seamless-Toypad-Build) | `toypad` |
| `DimensionsModManager/` | [NeverCookFirst/DimensionsModLoader](https://github.com/NeverCookFirst/DimensionsModLoader) | `main` |
| `DimensionsSaveConverter/` | [NeverCookFirst/DimensionsSaveConverter](https://github.com/NeverCookFirst/DimensionsSaveConverter) | `main` |
| `LegoToypad/` | upstream [harrysof/LegoToypad](https://github.com/harrysof/LegoToypad), fork [NeverCookFirst/LegoToypad-pr](https://github.com/NeverCookFirst/LegoToypad-pr) | feature branches |

Forks push to `fork`, never to `origin`.

## Prior art worth reading

If you are here to do the same thing to another Xbox 360 title, read these
first. This project would not have been attempted without the first one.

- [UnleashedRecomp](https://github.com/hedge-dev/UnleashedRecomp), the one that
  started it for me.
- [TheSimpsonsGameRecomp](https://github.com/YesterMester/TheSimpsonsGameRecomp)
- [reblue](https://github.com/zolaware/reblue)

## Licence

MIT, see [LICENSE](LICENSE). Covers the code in this repository only. It does not
cover LEGO Dimensions or any of its data, and the generated sources you produce in
step 3 are a derivative of the game, so they are yours to keep and not to
redistribute.
