# Mods

## Turning mods on

Press **F8** in game, tick the mods you want, click **Apply**, then restart the
game. That is it.

All mods start off. Four come with the install:

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

### The one rule that matters

**Your replacement file must not be longer than the original.** Shorter is fine,
the same length is ideal.

The game streams these archives and expects the files inside to stay where they
are. A bigger file has to be moved to the end, and that is what makes the game
crash on boot. So when you edit text, watch the character count: a replacement
only fits where the original was if you pad or trim it to match.

### Getting the original file out

Editing means starting from the real file. Use
[DimensionsModLoader](https://github.com/NeverCookFirst/DimensionsModLoader),
which can browse the archives and pull files out, or QuickBMS with the
`ttgames.bms` script.

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
