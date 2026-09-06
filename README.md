<p align="center">
  <img src="docs/logo.png" alt="Dimensions Recompiled" width="820">
</p>

<p align="center">
  A native PC build of LEGO Dimensions, made by statically recompiling the Xbox 360 release.
</p>

<p align="center">
  <a href="https://github.com/NeverCookFirst/DimensionsRecomp/releases/latest"><b>Download</b></a>
  &nbsp;&middot;&nbsp;
  <a href="docs/how-to-mod.md">Mods</a>
  &nbsp;&middot;&nbsp;
  <a href="README-dev.md">Build it yourself</a>
  &nbsp;&middot;&nbsp;
  <a href="https://discord.com/invite/PuXpBMFE4P">LEGO Dimensions Discord</a>
</p>

---

## Why this exists

I started this to answer one question: can a toys-to-life game be recompiled, and
is a native PC port of LEGO Dimensions actually realistic?

The answer is yes, and this is the proof. It boots, plays, saves, loads DLC, and
talks to a Toy Pad, as a normal Windows program.

This is not a claim to be the one correct way to play on PC. Emulation works, and
the emulator projects did the groundwork that made this possible. If
[xenia](https://github.com/xenia-canary/xenia-canary) or
[RPCS3](https://github.com/RPCS3/rpcs3) serves you better, use them. I respect
every project around this game and none of them are competitors.

## How Claude was used

I would rather you hear this from me than guess.

> [!WARNING]
> [Claude](https://claude.ai) was used heavily as a development tool. It read
disassembly and file formats with me, and wrote large parts of the runtime glue,
the installer, the mod tooling and this page.

**What it did not do** is decide what to build or judge whether the result was any
good. The goal and the scope were mine. The domain knowledge about this game,
its Toy Pad, its saves and its archives came from years of community work, not
from a model. And above all I played the game. Every "this is broken", "this
feels off" and "this is finally right" came from a person in front of the actual
thing. A model cannot tell you a world failed to load or a cutscene hung. That
loop is what turned something that compiled into something that plays.

If you are skeptical about AI being used this way, that is fair and I will not
argue with you. Judge it by whether it works.

## Install

**No game data is included here.** You bring your own, and the installer checks
that it is the right one before it does anything.

Have ready:

- **The Xbox 360 disc, dumped and extracted.** Dump your disc to an ISO, then
  extract that ISO to a folder with a tool like
  [extract-xiso](https://github.com/XboxDev/extract-xiso) or Xbox Image Browser.
  The installer wants **the folder, not the ISO**. It is right when the folder
  contains `Default.xex` alongside `GAME.DAT` and `GAME.HDR`. Only the original
  release works, and the installer will tell you if yours is a different build.

- **Title Update 23. Not 24, not 22.** The recomp was built from the TU23
  executable and will not run on anything else, so this one is checked by hash
  and a wrong update is refused. Give it either the raw package file, which is
  named `tu00000003_00000000` and has no extension, or a folder holding
  `Default.xexp`, `PATCH.DAT` and `PATCH.HDR`.

- **DLC, optional.** Point the installer at a folder holding your DLC packages
  and it takes all of them at once. You can also skip this and re-run the
  installer later to add them.

Then just run
[`DimensionsRecompiled-Setup.exe`](https://github.com/NeverCookFirst/DimensionsRecomp/releases/latest)
and follow the wizard. It writes a `README.txt` next to the game with the
hotkeys, settings and save locations.

Updates are handled in game. Turn them off with `F4` &rarr; Updates &rarr;
`updates_check`.

## The Toy Pad

Out of the box the installer sets up the
[LEGO Toypad app](https://github.com/harrysof/LegoToypad), which stands in for
the portal and lets you pick figures with a controller.

**Your real Toy Pad works too.** Windows hands it to its own driver, so swap that
once with [Zadig](https://zadig.akeo.ie): tick Options, List All Devices, pick
`LEGO READER V2.10` (check the ID reads `0E6F 0241`), choose `libusb0`, press
Replace Driver. Then in game press `F4`, open Input, untick `toypad_emulation`
and restart. The pad lights up and real figures work.

> [!NOTE]
> For now, only the Wii U, PlayStation 3 and PlayStation 4 toypads are compatible.

To hand the pad back to Windows later, uninstall that driver in Device Manager.

## Languages

The game ships its official translations, and the recomp picks one the way the
console did, with a language ID. Set `user_language` in `legodimensions.toml`
while the game is closed, or find it in `F4` under Kernel:

```toml
user_language = 4   # French
```

| ID | Language |
|---|---|
| 1 | English (default) |
| 3 | German |
| 4 | French |
| 5 | Spanish |
| 6 | Italian |
| - | Russian |

> [!NOTE]
> Russian is not one of the official languages. It exists as a community
> translation made by [maickdelaia](https://boosty.to/lego_dimensions_ru), shipped as a **mod** that replaces the English column.

## This is a demo build. Expect bugs.
> [!CAUTION]
> It is an early public build, not a finished port. Crashes, freezes, broken
graphics and lost audio are all expected, and finding them is why it is public.
How well it runs varies by machine, because the graphics path still has to
translate what the Xbox 360 GPU was asked to do.

Known issues:

- Starting a **new save** and quitting before the opening cutscenes finish can
  leave a save that will not load. Play until you are walking around first.
- **60 FPS** is an unlock the original never ran at, and most remaining bugs
  live there. Set `frame_rate` to 30 in `F4` before reporting anything odd.
- Some scenes render with **wrong colours or missing effects**.
- The **Vulkan** backend is broken. Direct3D 12 is the working one and the
  default.
- Not every code path has been visited, so an unexplored corner can still hit a
  hard stop rather than a graphical glitch.

## What is planned

Roughly in the order I want to get to it. No dates.

- **Fixing the crashes that keep coming back.** A handful of them are
  reproducible and hit a lot of people, so they matter more than anything new.
- **Xbox 360 and Xbox One ToyPads support**, so everyone can play the game how they want.
- **UI fixes, and layouts for other gamepads.** Right now the button prompts
  assume an Xbox pad. PlayStation, Switch and generic controllers should show
  their own.
- **Keyboard and mouse working out of the box.** It is already in there and
  fully rebindable, but it has to be switched on first, which almost nobody
  finds.
- **Updating the Discord Rich Presence** so it shows more than a static line.
- **Recompiling the GPU side too, in some form.** *Research, not a promise.*
  Graphics is the one part still done the emulator way: the game's draw calls
  are translated while it runs. Doing to them what was already done to the CPU
  code, translating ahead of time instead of live, is where the remaining
  performance and most of the visual bugs are. It is a large piece of work and
  it may turn out not to be practical. I would rather say that now than promise
  it.

> [!IMPORTANT]
> **This project needs people.** It is one person plus a lot of community
knowledge, and that is not enough for what is left. If you know graphics work,
profiling, or Xbox 360 internals, and you actually care about this game running
well, the optimisation work is wide open and I would genuinely welcome the help.
Come say hello in the [Discord](https://discord.com/invite/PuXpBMFE4P) or open an
issue.

## What "recompiled" means here

There is no emulator running underneath, and that is the point.

The Xbox 360 executable is PowerPC code. A tool translates it to C++ once, ahead
of time, and a normal compiler turns that into a native x86-64 Windows program.
Your CPU runs real x86 instructions. Nothing is interpreted at runtime.

What could not simply be translated is everything the game asked the console for:

| Part | How it works here |
|---|---|
| Game code (CPU) | Statically recompiled to native x86-64. No emulation. |
| Graphics | The Xbox 360 GPU command stream is translated at runtime to Direct3D 12. Inherited from emulator work, and where most visual bugs still are. |
| Kernel, saves, achievements, DLC | Reimplemented as host functions. Saves are real files. |
| Audio, input, windowing | Native. Any XInput controller, plus keyboard and mouse. |
| Toy Pad | A companion app over localhost, or a real portal over USB. |

The runtime providing all of that is the
[ReXGlue SDK](https://github.com/rexglue/rexglue-sdk), which derives from
[xenia](https://github.com/xenia-canary/xenia-canary). So: the CPU side is a
port, the graphics side still stands on emulator research. Saying that plainly is
more useful than overselling it.

Because there is no interpreter tax, it can run at 60 FPS and use modern
upscaling (FSR 1 and CAS) and higher internal resolutions.

## Tested on, and size

Everything was developed and tested on one machine: Ryzen 7 5700X3D, RTX 4060,
32 GB RAM, Windows 11, 1080p. A mid range desktop, not a high end one.

**That is the only machine this has been properly tested on, so I cannot promise
stable performance on any device, including that one.** This is a public beta.
Steam Deck and handhelds have not been tested at all, and neither has anything
running it through Proton or Wine.

> [!TIP]
> Minimum: a 64-bit CPU with AVX2 (Haswell or Zen 1 and newer) and a Direct3D 12
GPU at feature level 11_0.

| Installed | Size |
|---|---|
| Base game plus Title Update 23 | about 9 GB |
| All 30 DLC packages | about 15 GB more |
| **Everything** | **about 24 GB** |

## Reporting a bug

Open an [issue](https://github.com/NeverCookFirst/DimensionsRecomp/issues/new/choose).
The template asks for the few things that actually help: `game.log`, your
`frame_rate`, and your GPU.

---

## Thanks

- **[Unleashed Recompiled](https://github.com/hedge-dev/UnleashedRecomp)**, first
  and above all. This project exists because of how much I loved what they did.
  Seeing a console game turned into a real native PC build is what made me ask
  whether LEGO Dimensions could be next.
- **[ReXGlue](https://github.com/rexglue/rexglue-sdk)**, right after them. It is
  the toolkit that does the actual recompiling and provides the runtime this
  game sits on. Without it there would have been nothing to try in the first
  place.
- **The [LEGO Dimensions Discord](https://discord.com/invite/PuXpBMFE4P)**, for
  years of accumulated knowledge about this game and for encouraging every one of
  these experiments instead of dismissing them.
- **[harrysof](https://github.com/harrysof)**, for the
  [LEGO Toypad app](https://github.com/harrysof/LegoToypad) and the Toy Pad
  protocol work everything portal related here builds on.
- **[xenia](https://github.com/xenia-canary/xenia-canary)**, whose research into
  the Xbox 360 GPU makes the graphics side possible at all.
- Everyone who tested a broken build or said it was a good idea before it
  obviously was one.

## Related repositories

| Repository | What it is |
|---|---|
| [DimensionsModLoader](https://github.com/NeverCookFirst/DimensionsModLoader) | Mod manager that injects files into the game's DAT archives |
| [DimensionsSaveConverter](https://github.com/NeverCookFirst/DimensionsSaveConverter) | Converts saves between console versions |
| [Xenia-Seamless-Toypad-Build](https://github.com/NeverCookFirst/Xenia-Seamless-Toypad-Build) | xenia fork with a built in emulated Toy Pad |
| [RPCS3-Seamless-Toypad-Build](https://github.com/NeverCookFirst/RPCS3-Seamless-Toypad-Build) | The same idea for the PS3 version |
| [shadPS4-Seamless-Toypad-Bridge](https://github.com/NeverCookFirst/shadPS4-Seamless-Toypad-Bridge) | Toy Pad bridge for the PS4 version |

## Antivirus

The installer is one big unsigned executable that unpacks a payload into your
game folder, which is exactly the shape heuristic scanners and SmartScreen do
not like. It comes back clean:
[VirusTotal scan](https://www.virustotal.com/gui/file/51b2d3a33affa5e22d75d0a9501c390653966f1364de17547943148f6929df10?nocache=1).

Windows will still put up **"Windows protected your PC"** the first time you run
it. That is normal for anything unsigned. Click **More info**, then **Run
anyway**.

## Licence and the legal bit

The code here is MIT licensed, see [LICENSE](LICENSE). That covers what I wrote.
It does not cover LEGO Dimensions or any of its data. Bring your own game.

Not affiliated with or endorsed by LEGO, TT Games or Warner Bros.

**There are no donations.** Nothing here is sold, and nobody should be asking you
for money for it.

Built by [NeverCookFirst](https://github.com/NeverCookFirst).
