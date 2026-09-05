# Where the code lives

Written 2026-09-05, updated 2026-09-06. The working folder is ~20 GB, of which
the code we wrote is about 1 MB. So this is not one repository: it is this one
plus a handful of others that sit inside the same folder and are ignored here.
The rule that decides what goes in: **anything that came from the game, or is
generated from it, stays out.**

All of them are **private** for now.

## This repository - NeverCookFirst/DimensionsRecomp

The workspace root is the repo. `.gitignore` ignores `/*` and then names what
is ours:

| Path | What it is |
|---|---|
| `rexlego/` | the recompiled game: our sources, CMake, mods menu, Discord presence, update check |
| `rexlego-installer/` | the installer **and** the updater (one C# project, one binary) |
| `DimensionsModManager-CLI/` | the command-line mod tool shipped in `tools\modcli` |
| `xexdump/` | the XEX inspection utility |
| `tools/` | odds and ends used during development |
| `VERSION` | the version the build script stamps into a release |
| `rexlego-installer/releases/` | one manifest per shipped release, needed to build the next update pack |

Not included, on purpose: `game\`, `update\`, dumps, `LD Backups\`, `xpng\`,
`research\`, `rpcs3-ref\`, and the nested repositories below.

## The other repositories

| Folder | Remote | Branch |
|---|---|---|
| `DimensionsModManager/` | NeverCookFirst/DimensionsModLoader | `main` |
| `DimensionsSaveConverter/` | NeverCookFirst/DimensionsSaveConverter | `main` |
| `rexglue-sdk/` | `origin` rexglue/rexglue-sdk, `fork` NeverCookFirst/rexglue-sdk | `toypad-ui` |
| `xenia-canary/` | `origin` xenia-canary/xenia-canary, `fork` NeverCookFirst/Xenia-Seamless-Toypad-Build | `toypad` |
| `LegoToypad/` | `origin` harrysof/LegoToypad, `fork` NeverCookFirst/LegoToypad-pr | `feature/pad-shortcuts-story-mode-rebinding` |

Forks push to `fork`, never to `origin`. The SDK clone was shallow, so its
first push needed `git fetch --unshallow origin`.

## Releasing

See [RELEASING.md](RELEASING.md).
