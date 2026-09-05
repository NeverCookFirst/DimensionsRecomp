# Putting this project into git

Written 2026-09-05. Everything below is meant to be run by hand, one repo at a
time. Nothing here has been pushed for you.

The working folder is ~20 GB, of which the code we actually wrote is about
**1 MB**. So this is not one repository - it is four small new ones plus the
repos that already exist. The rule that decides what goes in: **anything that
came from the game, or is generated from it, stays out.**

---

## 1. What is already under git

| Folder | Remote | State right now |
|---|---|---|
| `DimensionsModManager/` | NeverCookFirst/DimensionsModLoader | 2 changed files, nothing unpushed |
| `DimensionsSaveConverter/` | NeverCookFirst/DimensionsSaveConverter | clean |
| `rexglue-sdk/` | rexglue/rexglue-sdk (branch `toypad-ui`) | **6 changed files, uncommitted** |
| `xenia-canary/` | xenia-canary fork (branch `toypad`) | clean |
| `LegoToypad/` | harrysof/LegoToypad | clean, someone else's project |

Two of these need attention before anything else:

```powershell
cd "E:\Claude\LEGO Dimensions\rexglue-sdk"
git status              # the six changes are our runtime work - look, then commit
git diff

cd "E:\Claude\LEGO Dimensions\DimensionsModManager"
git status
```

`LegoToypad/` is harrysof's repository, not ours. Leave it alone; we only ship
his released .exe.

---

## 2. New repositories to create

`.gitignore` files are already in place for all four.

### a. `rexlego/` -> the recompilation project

The important one. What actually gets committed: `src/` (8 files), `res/`,
`CMakeLists.txt`, `CMakePresets.json`, `legodimensions_config.toml` - roughly
**2 MB**. Ignored: `generated/` (469 MB of translated game code), `out/`,
`content/`, `tu23/`, `tu23-mods/`, `build-snapshots/`, `save-backups/`.

```powershell
cd "E:\Claude\LEGO Dimensions\rexlego"
git init
git add .
git status              # CHECK THE LIST before committing - see section 3
git commit -m "LEGO Dimensions recompilation project"
gh repo create NeverCookFirst/Dimensions-Recompiled --private --source=. --push
```

### b. `rexlego-installer/` -> the tester installer

```powershell
cd "E:\Claude\LEGO Dimensions\rexlego-installer"
git init
git add .
git status
git commit -m "Installer for the LEGO Dimensions recompilation"
gh repo create NeverCookFirst/DimensionsRecompiled-Installer --private --source=. --push
```

### c. `xexdump/` -> the XEX image dumper

Tiny tool (one `main.cpp`). Could equally live inside the rexlego repo as
`tools/xexdump`; keeping it separate is fine too.

```powershell
cd "E:\Claude\LEGO Dimensions\xexdump"
git init && git add . && git commit -m "XEX image dumper"
gh repo create NeverCookFirst/xexdump --private --source=. --push
```

### d. `DimensionsModManager-CLI/`

This one is awkward on purpose: its `.csproj` compiles sources out of
`..\DimensionsModManager\`, so as a standalone repo it will not build on anyone
else's machine. The clean fix is to move it **into** the DimensionsModLoader
repo as a subfolder and change the three `<Compile Include="..\DimensionsModManager\...">`
lines to `..\`. Until that is done, keep it local or accept the broken
standalone build.

---

## 3. Before you push - the checks that matter

**Run `git status` and actually read it.** In `rexlego` in particular, confirm
that `generated/`, `content/`, `tu23*`, and `out/` are absent from the list.
A single stray `PATCH.DAT` is 800 MB and GitHub will reject the push anyway;
worse, game code in a public repo is what gets projects taken down.

**Decide on these before making anything public** - they are all in `rexlego/res/`
and all derived from someone else's work:

- `achievement_unlocked.wav` - the Xbox 360 achievement sound, Microsoft's.
- `legodimensions.ico` and `discord_cover.png` - game art, TT Games / Warner.
- `FiraSans-Regular.ttf` - SIL Open Font Licence, fine to ship, but the licence
  text should travel with it.
- `gamecontrollerdb.txt` - community database, Zlib licence, fine.

For a private repo none of this matters. For a public one, the first two should
be dropped and fetched by the user, the way the game data already is.

**`legodimensions_config.toml` is committed on purpose** - it is codegen
configuration (function addresses and overrides), the same thing other
recompilation projects ship. It contains no game code.

**The `research/` folder is other people's repositories** (Simpsons recomp,
re:Blue, Unleashed) - clones for reference. Do not commit it anywhere.

---

## 4. What is deliberately not in git, and how to get it back

| Thing | Why not | How to reproduce |
|---|---|---|
| `rexlego/generated/` | 469 MB, translated game code | ReXGlue codegen against your own `Default.xex` + TU23 |
| `rexlego/content/`, `tu23/`, `tu23-mods/` | game data | the installer builds all of it from the user's own dump |
| `rexlego/out/`, `*/bin`, `*/obj` | build output | rebuild |
| `rexlego-installer/dist`, `staging` | contains the game binaries | `build-installer.ps1` |
| `xexdump/dump/` | the game's executable image | run xexdump against your own game |
| `LD Backups/` | 831 MB of save history | nothing to reproduce, it is a personal backup |
