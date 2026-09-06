<p align="center">
  <img src="docs/logo.png" alt="Dimensions Recompiled" width="820">
</p>

<p align="center">
  A native PC build of LEGO Dimensions, made by statically recompiling the Xbox 360 release.
</p>

---

## Why this exists

I started this to answer one question: can a toys-to-life game be recompiled, and
is a native PC port of LEGO Dimensions actually realistic?

The answer turned out to be yes, and this repository is the proof. The game boots,
plays, saves, loads DLC, and talks to a Toy Pad, all as a normal Windows program.

This project does not claim to be the one correct way to play LEGO Dimensions on
PC. Emulation works, and the people who built and maintain those emulators did the
hard groundwork that made this possible in the first place. If xenia or RPCS3
serves you better, use them. I have a lot of respect for every project around this
game, and none of them are competitors.

## This is a public demo build. Expect bugs.

Please read this part before you install anything.

This is an early public build, not a finished port. The game can crash, freeze,
render something wrong, lose audio, or refuse to load a save. That is expected at
this stage, and it is exactly why the build is public: more machines mean more
bugs found.

How well it runs also depends a lot on your PC. The recompiled CPU code is native
and fast, but the graphics path still has to translate what the Xbox 360 GPU was
asked to do into something a modern GPU understands, and that part is where the
cost is. Two machines with similar paper specs can behave differently. If
something looks wrong, try setting `frame_rate` back to 30 in the F4 menu before
reporting it, because 60 FPS is an unlock the original game never ran at and most
of the remaining problems live there.

The installer contains no game data. It reads your own disc dump and your own
Title Update, checks that they are the right ones, and builds an install from
them. This project is not affiliated with or endorsed by LEGO, TT Games, or
Warner Bros.

## What "recompiled" means here, and what actually runs natively

There is no emulator running underneath this. That is the whole point, and it is
worth being precise about, because "recompilation" gets used loosely.

The Xbox 360 executable is PowerPC code. A tool reads that code once, ahead of
time, translates every function it can find into C++, and that C++ is then
compiled by a normal compiler into a native x86-64 Windows program. So when you
run the game, your CPU is executing real x86 instructions that were generated from
the original game logic. Nothing is being interpreted or JIT compiled at runtime,
and there is no guest CPU to emulate.

What could not simply be translated is everything the game asked the console to do
for it:

| Part | How it works here |
|---|---|
| Game code (CPU) | Statically recompiled to native x86-64. No emulation. |
| Graphics | The game still speaks to the Xbox 360 GPU. That command stream is translated at runtime into Direct3D 12. This is the part inherited from emulator work, and the part still responsible for most visual bugs. |
| System calls, kernel, saves, achievements, DLC | Reimplemented as host functions, so saves are real files and DLC is real content on your disk. |
| Audio, input, windowing | Native host code. Any XInput controller works, and so does keyboard and mouse. |
| Toy Pad | Either a companion app that stands in for the portal over localhost, or a real LEGO Dimensions Toy Pad connected over USB. |

The runtime that provides all of that is the ReXGlue SDK, which derives from the
xenia emulator project. So the honest summary is: the CPU side is a port, the
graphics side still stands on emulator research, and being able to say that out
loud is more useful than overselling it.

Practical consequences of doing it this way:

- It can run at 60 FPS, because there is no interpreter tax to pay.
- It can use modern upscaling (FSR 1 and CAS) and higher internal resolutions.
- Any function the translation missed shows up as a crash here, where an emulator
  would just have executed it. Those are the bugs worth reporting.

## What it was tested on, and how much space it needs

Everything was developed and tested on one machine:

- AMD Ryzen 7 5700X3D (8 cores, 16 threads)
- NVIDIA GeForce RTX 4060
- 32 GB RAM
- Windows 11, 1080p display

That is a mid range desktop from a few years ago, not a high end one. The game
renders internally at 1280x720, the same as the original, and is then upscaled to
your screen, so most of the headroom on that machine goes into 60 FPS and optional
supersampling rather than raw resolution.

Minimum requirements are a 64-bit CPU with AVX2 (Intel Haswell or AMD Zen 1 and
newer) and a GPU that supports Direct3D 12 at feature level 11_0.

Disk space, once installed:

| What | Size |
|---|---|
| Base game plus Title Update 23 | about 9 GB |
| All 30 DLC packages on top of that | about 15 GB |
| **Full install with everything** | **about 24 GB** |

You need a bit more than that free while installing, since the installer copies
and extracts rather than moving your original files.

## How Claude was used, and what came first

I want to be straightforward about this, because I would rather you hear it from
me than guess.

Claude, Anthropic's model, was used heavily throughout this project, as a
development tool. It read disassembly and file formats with me, wrote and rewrote
large amounts of the runtime glue, the installer, the mod tooling, and the
documentation you are reading. On a project with this much grinding through binary
formats and unfamiliar code, it saved an enormous amount of time.

What it did not do is decide what to build, or judge whether the result was any
good.

Everything started from my side. The goal, the scope, and the decision that this
was worth attempting at all were mine. I supplied the domain knowledge about LEGO
Dimensions, the Toy Pad protocol, the save format, and the archive formats, most
of which came from years of community work rather than from any model. I dumped
and prepared the data, ran the builds, and above all I played the game. Every
single "this is broken", "this is wrong", "this feels off", and "this is finally
correct" came from a person sitting in front of the actual game. A model cannot
tell you that a world failed to load, that a character animates wrongly, or that a
cutscene hangs. That loop of testing, reporting, and fixing is what turned a thing
that compiled into a thing that plays, and none of this would exist without it.

If you are skeptical about AI being used this way, that is a fair position and I
am not going to argue with you about it. I have no grievance with anyone who feels
that way. All I would ask is that you judge the project by whether it works: the
code is here, the method is described above, and the build either runs on your
machine or it does not.

## Thanks

This project genuinely would not exist without other people.

- **The LEGO Dimensions Discord community.** For years of accumulated knowledge
  about this game's formats, figures, and internals, for answering questions, and
  for encouraging every one of these experiments instead of dismissing them.
- **[harrysof](https://github.com/harrysof)**, for the LEGO Toypad companion app
  and for the Toy Pad protocol work that everything portal related here builds on.
- **The xenia project**, whose research into the Xbox 360 GPU is what makes the
  graphics side of this possible at all.
- Everyone who tested a broken build, filed a report, or just said it was a good
  idea when it was not obviously one yet.

## Repositories

Dimensions Recompiled is one part of a set of related projects:

| Repository | What it is |
|---|---|
| [DimensionsRecomp](https://github.com/NeverCookFirst/DimensionsRecomp) | This one: the recompiled game, the installer, and the updater |
| [DimensionsModLoader](https://github.com/NeverCookFirst/DimensionsModLoader) | Mod manager that injects files into the game's DAT archives |
| [DimensionsSaveConverter](https://github.com/NeverCookFirst/DimensionsSaveConverter) | Converts saves between the console versions |
| [Xenia-Seamless-Toypad-Build](https://github.com/NeverCookFirst/Xenia-Seamless-Toypad-Build) | xenia fork with a built in emulated Toy Pad |
| [RPCS3-Seamless-Toypad-Build](https://github.com/NeverCookFirst/RPCS3-Seamless-Toypad-Build) | The same idea for the PS3 version |
| [shadPS4-Seamless-Toypad-Bridge](https://github.com/NeverCookFirst/shadPS4-Seamless-Toypad-Bridge) | Toy Pad bridge for the PS4 version |

## Building it yourself

See [README-dev.md](README-dev.md). Be warned that you cannot build this from a
clean clone alone: the recompiled sources are generated from your own copy of the
game and are deliberately not distributed here.

## Licence

The code in this repository is MIT licensed, see [LICENSE](LICENSE). That covers
what I wrote. It does not cover LEGO Dimensions, any of its data, or anything else
owned by LEGO, TT Games, or Warner Bros. Bring your own game.

Built by [NeverCookFirst](https://github.com/NeverCookFirst).
