# My final release

I started Dimensions Recompiled to see whether a toys-to-life game could work as a normal PC program. Getting LEGO Dimensions to boot was only the start. The Toy Pad, saves, DLC, and a long list of rendering bugs made it a much bigger project than I expected.

I worked with Claude on the earlier recomp and with Codex on the later Native GPU work. I used both agents to investigate the game's code, write and revise patches, build installers, and work through logs and captures. I decided what to build, played the game, and brought back the scenes and steps that broke. Testers found things I missed and helped narrow down which fixes held up. I owe a lot to the LEGO Dimensions community, ReXGlue, the emulator developers, and everyone who spent time testing.

Native GPU 0.2.0 is my last release. I'm archiving this repository so the source, downloads, and notes stay available. I won't be pushing further updates here. If you want to carry the work forward, please fork it and take it in your own direction.

The Native GPU build is an experimental Windows x64 Direct3D 12 edition. It installs separately from the original build and starts from `legodimensions.exe`. Video and Effects live in the game menus alongside native Credits and Quit Game. FSR 3 starts off. The optional Russian mod also translates those menus. Startup pipeline preparation covers the shipped set of pipelines. First-use compilation, rendering bugs, and travel crashes can still happen. Linux and AMD parity were never fully validated.

- [Native GPU 0.2.0 release](https://github.com/NeverCookFirst/DimensionsRecomp/releases/tag/v0.2.0)
- [Latest non-native GPU build](https://github.com/NeverCookFirst/DimensionsRecomp/releases/tag/v0.1.38)
- [Native GPU source snapshot and build notes](https://github.com/NeverCookFirst/DimensionsRecomp/tree/v0.2.0)
- [Known bugs and follow-up notes](docs/native-gpu-known-bugs.md)
