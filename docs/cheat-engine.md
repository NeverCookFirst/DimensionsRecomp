# Using Cheat Engine with Dimensions Recomp

Cheat Engine attaches to the game fine, and writing works. Two things make a
plain scan come up empty, and both are settings on the Cheat Engine side.

## 1. Turn on scanning of mapped memory

Guest memory is not an ordinary allocation: it is a file mapping, aliased into
several windows so the guest's 0xA0000000/0xC0000000 views point at the same
bytes. Windows reports those pages as `MEM_MAPPED`, and Cheat Engine skips
`MEM_MAPPED` by default, so the entire game state is invisible to a scan.

Cheat Engine -> Edit -> Settings -> Scan Settings, and tick **MEM_MAPPED**
(leave MEM_PRIVATE and MEM_IMAGE as they are). Without this, nothing else here
matters.

## 2. Scan as big-endian

The game is Xbox 360 code, so every value in guest memory is stored
big-endian. `100` studs is `00 00 00 64`, which Cheat Engine's normal 4-byte
scan reads as 1677721600.

Value type dropdown -> **Define new custom type (Lua)** -> paste one of the
scripts from `tools/cheatengine/`:

| File | Type it defines |
| --- | --- |
| `BigEndian4Bytes.lua` | `Big Endian 4 Bytes` — counters, IDs, flags |
| `BigEndian2Bytes.lua` | `Big Endian 2 Bytes` |
| `BigEndianFloat.lua` | `Big Endian Float` — health, timers, positions |

They stay in Cheat Engine's settings after that; you only add them once.
Byte-array and string scans need no custom type — but text in the title is
Latin-1, not UTF-16.

## 3. Guest addresses vs. what Cheat Engine shows

Cheat Engine shows host addresses. The guest arena is placed wherever the
64-bit address space had room (usually `0x0000000100000000`, but not
guaranteed), so:

    host address = guest membase + guest address

Open the in-game console (F4) and run **`membase`** to print the base for the
current run. `membase 82395D14` converts a guest address to a host one, and
`membase 1_8239_5D14` — any host address — converts back. The same line is
written to `game.log` at startup as "Guest memory arena mapped".

So a guest pointer you found in a disassembly, say `0x82395D14`, is at
`membase + 0x82395D14` in Cheat Engine, and a Cheat Engine hit at
`0x182395D14` is guest `0x82395D14`.

Guest pointers stored *in* memory are 32-bit and big-endian, so Cheat Engine's
pointer scanner cannot follow them. Chase them by hand with `membase`, or use
the console's `peek`/`poke`/`findword`, which already work in guest addresses.

## Caveats

* The arena base changes every run. Table entries are best saved as
  "membase + guest offset" notes rather than absolute addresses.
* Addresses in the `0xA0000000`–`0xE0000000` range are GPU-visible physical
  memory. The runtime tracks writes to those pages itself; editing them from
  outside can be missed or overwritten, and is not a good place to cheat.
* Freezing a value the game writes every frame costs performance under a
  debugger attach — expect some stutter while Cheat Engine is active.
