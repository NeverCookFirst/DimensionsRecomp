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

## Step 1: get the SDK

Clone it next to this repository, or anywhere else you like:

```bash
git clone --recursive https://github.com/rexglue/rexglue-sdk.git
```

It has git submodules and needs `--recursive`. If you forgot,
`git submodule update --init --recursive` fixes it.

Build and install it:

```bash
cmake -S rexglue-sdk -B rexglue-sdk/out/build/win-amd64 -G "Ninja Multi-Config" \
      -DCMAKE_INSTALL_PREFIX=rexglue-sdk/out/install/win-amd64
cmake --build rexglue-sdk/out/build/win-amd64 --config Release --target install
```

**`--config Release` is not optional.** The SDK builds with Ninja Multi-Config,
and a plain `cmake --build` defaults to RelWithDebInfo, which produces
`rexruntimerd.lib`. The game links `rexruntime.lib`, the Release one. Getting this
wrong gives you a link error that does not explain itself.

## Step 2: point the game at the SDK

`rexlego` finds the SDK one of two ways, and it will refuse to configure if
neither works:

- `-DREXSDK_DIR=<path to the rexglue-sdk source tree>` builds the SDK as a
  subproject of the game. Simplest, and what you want while changing SDK code.
- Otherwise it falls back to `find_package(rexglue CONFIG)`, which needs the SDK
  already installed and on `CMAKE_PREFIX_PATH`.

## Step 3: generate the recompiled sources

Copy the manifest template and point it at your dump:

```bash
cp rexlego/legodimensions_manifest.toml.example rexlego/legodimensions_manifest.toml
```

Edit `game_root` and `entrypoint.file_path` so they point at your extracted disc.
Paths are relative to the manifest file. The real `legodimensions_manifest.toml`
is gitignored precisely because it holds paths specific to your machine.

Then run the codegen tool from the SDK build:

```bash
cd rexlego
../rexglue-sdk/out/install/win-amd64/bin/rexglue codegen legodimensions_manifest.toml
```

This is the long step. It analyses the XEX, finds functions, and writes
`rexlego/generated/`. Expect a few minutes and about 470 MB.

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
Incremental builds after that are quick.

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

The script takes the game binaries from `rexlego\out\build\win-amd64-release` by
default. Override with `-GameBuild`.

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
