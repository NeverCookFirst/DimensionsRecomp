# Mods

## Turning mods on

Press **F8** in game, tick the mods you want, click **Apply**, then restart the
game. That is it.

Two built-in entries are always on and greyed out: the **Mystery Dimension
portal fix** and **Fern** (Finn can swap into Fern; see the worked example
below). All other mods start off. Four come with the install:

- **QuickStartup** skips the intro splash screens.
- **Recomp_TextTest** changes one line of text, so you can tell at a glance
  whether mods are working.
- **SuperSonicInfinite** makes Super Sonic last until you cancel it instead of
  draining studs. Needs the Sonic Level Pack (DLC16) installed.
- **AllWorlds** lets every character open every adventure world, Year 1 and
  Year 2 alike. It only rewrites the character configs, so it conflicts with a
  mod that edits the same character file; the mod lower in the list wins.

Your original game files are never touched. Mods are applied to a separate copy
of the update folder, and unticking a mod puts the original bytes back. The one
exception is a mod that changes a DLC archive: those are patched in place with a
byte backup, and unticking restores them the same way.

## Installing someone else's mod

Drop the mod's folder into the `mods` folder next to the game, then press F8.
The folder must contain a `mod.json`. If it does not show up in the list, that
file is missing or the mod is for a different platform.

## Making your own

A mod is just a folder:

```
mods/
  MyMod/
    mod.json
    datfiles/
      PATCH/
        STUFF/TEXT/TEXT.CSV      <- replaces that file inside PATCH.DAT
```

`mod.json` describes it:

```json
{
  "name": "My Mod",
  "author": "you",
  "version": "1.0",
  "platform": "x360",
  "description": "What it changes."
}
```

Use `"platform": "x360"` for this build, or `"any"` if the mod also works on the
PS3 version.

Everything under `datfiles/<ARCHIVE>/` is injected into `<ARCHIVE>.DAT`, keeping
the folder structure the game uses inside that archive. `PATCH` is where most of
the interesting things live: text, GUI scripts, character stats, abilities.
DLC archives work too: `datfiles/DLC16/...` finds `DLC16.DAT2` inside its package
folder under `content`, which is where DLC characters keep their abilities.

The disc archives are reachable as well: `datfiles/GAME/...` finds `GAME.DAT` in
the `game` folder, and so do `GAME0`..`GAME4` and `INSTALL0_*`. Reach for them
only for something the update does not carry — the button prompts in
`GUI/FONT/BUTTONS_360_NXG.FT2`, for one. If a file exists in both, the update's
copy wins, so modding the disc copy would change nothing.

### The one rule that matters

**Your replacement file must not be longer than the original.** Shorter is fine,
the same length is ideal.

The game streams these archives and expects the files inside to stay where they
are. A bigger file has to be moved to the end, and that is what makes the game
crash on boot. So when you edit text, watch the character count: a replacement
only fits where the original was if you pad or trim it to match.

### Getting the original file out

Editing means starting from the real file. `tools\modcli\modcli.exe` in your
install reads every archive the game has:

```
modcli names <archive>                       list what is inside (index, offset, size, name)
modcli grep <archive> <text>                 find files by name
modcli extract <archive> <name> <out file>   pull one file out, decompressed
```

[DimensionsModLoader](https://github.com/NeverCookFirst/DimensionsModLoader)
and connorh315's BrickVault (see *Modding tools* below) do the same with a
window.

## Bigger mods: your own PATCH archive

The same-length rule above only applies to in-place mods. For anything that
adds files, or that is larger than the original, build a new archive instead:

```
modcli build <folder> <update folder>\PATCH4.DAT
```

`<folder>` holds files at the paths the game uses (for example
`additionalcontent\opus_tagcharswave1\...`). This writes `PATCH4.DAT` and
`PATCH4.HDR`, and the game opens them **by itself** - it probes `PATCH`,
`PATCH0`, `PATCH1`, ... in the update folder at boot. No size limits, no
restore step: delete the two files and the mod is gone. Put them in both
`update` and `update-mods` if you use F8 mods.

Things to know:

- **The earlier archive wins.** The game opens `PATCH.DAT` first, then
  `PATCH0`-`PATCH2` (the title update's), then ours, then the disc
  (`INSTALL0_*`, `GAME*`), then DLC. So a new archive overrides disc and DLC
  files, but not a file that also exists in `PATCH.DAT` - change those with an
  in-place mod.
- `PATCH3` is taken by Fern (below). Use `PATCH4` and up, without gaps.
- Build from the plain folder only; do not give the archive a "mod name" in
  other tools - that mode blanks the lookup hash of `TEXT.CSV` and
  `COLLECTION.TXT` and the game can no longer find them.

The archive writer is connorh315's BrickVault, bundled in modcli with his
permission.

### Finding out what the game is looking for

When something does not load - a character stuck in the portal vortex, a
missing texture - the game asked for a file and did not find it. Add this to
the top of `legodimensions.toml` (top level, not under a `[section]`):

```toml
trace_file_names = 'fern'
```

and `game.log` gets a `[name]` line for every file name the game looks up that
contains that text, found or not. `trace_file_reads = 'PATCH3,DLC9'` logs the
actual reads from those archives with their offsets, which tells you which
archive a file really came from. Both are very chatty - remove them afterwards.

### Worked example: Fern

Finn can turn into Fern, but the Adventure Time DLC shipped without Fern's
*charcache*, so the swap hung forever. The trace showed that for a character
the game looks up every asset under a cache prefix:

```
additionalcontent\opus_tagcharswave1\charcache\fern\<the asset's normal path>
e.g. ...\charcache\fern\additionalcontent\opus_tagcharswave1\chars\minifig\fern\fern.cd
```

Fern's model, animations and items all exist in `DLC9.DAT2` at their normal
paths. Copying each one under the `charcache\fern\` prefix (plus Finn's cached
abilities, cloned to that prefix) and building that into `PATCH3` made him
work - 621 files. The same recipe should apply to any character that has its
files but no charcache. Fern ships with the installer and shows in F8 as a
built-in entry that cannot be switched off.

## Modding tools

The installer has an optional **Modding tools** component (off by default). It
puts the latest releases of connorh315's tools in `tools\ModdingTools`:

| Tool | What it is for |
|---|---|
| BrickVault | browse, extract and build `.DAT` archives |
| Flux | `.led`, `.cd`, `.cpd`, `.as` - level and character data |
| AbilityDefEditor | abilities |
| SoundEventEditor | `.sound_event` |
| CBXDecoder | `.cbx` audio to WAV |
| Hologram | level viewer and exporter |
| Diorama | geometry viewer |
| DATPacker, DATManager | older archive packer and extractor |

They are his work, used with his personal permission - credit him if you use
them. `CREDITS.txt` in that folder lists the versions. Texture replacement is
not solved yet: the Xbox 360 build's `.tex`/`.nxg_textures` use the 360's tiled
layout, and the console has 512 MB for everything.

### Testing

Tick your mod in F8, apply, restart. If the game boots and you see your change,
it works. If it hangs on a black screen or crashes at startup, the file is
almost certainly too long.

`game.log` next to the game says which mods were loaded and which were rejected.

## If something breaks

- Untick every mod in F8, apply, restart. That restores the vanilla files.
- Still broken: re-run the installer over the same folder. It repairs the
  install and keeps your saves.

## Sharing

Zip the mod folder. That is the whole distribution format. Do not include any
game files in it, only your changed ones.
