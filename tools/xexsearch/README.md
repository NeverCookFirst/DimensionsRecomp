# xexsearch

Four small scripts for finding things in the decrypted guest image
(`xexdump/dump/default.bin`, mapped at `0x82000000`).

They exist because a plain byte search for an address finds nothing. PowerPC has
no 32-bit immediate: an address is built from two instructions, `lis` for the
top half and `addi`/`ori` for the bottom, so the value never appears as a word
anywhere in the code.

| script | finds |
|---|---|
| `ppcref.py` | code that **takes** an address (`lis` + `addi`/`ori`) |
| `dform.py` | code that **reads or writes** a global (`lis` + `lwz`/`stw`/`lbz`/`stb`), and says which |
| `xref.py` | 4-byte big-endian pointers in data — vtables, jump and class tables |
| `strs.py` | printable strings in a range of guest addresses |

All take hex guest addresses, with or without `0x`:

```
py -3 ppcref.py 8230CC6C          # who takes the address of "Character Grid"
py -3 dform.py  84A1D800          # who reads and who writes that global
py -3 xref.py   8368F3B8          # which tables point at this function
py -3 strs.py   8230D700 8230E200 # strings in that range
```

`dform.py` reporting dozens of hits is itself a finding: it means the address is
a core singleton read all over the engine, not the one-off flag you hoped for.

Once an address is known, the fastest cross-reference is not this at all: the
recompiler has already translated every function to readable C++ under
`rexlego/generated/default/`, so `grep sub_83689658` beats any disassembler.

For live values, the console (backtick) has `peek` and `poke` — see
`rexglue-sdk/src/ui/overlay/console_commands.cpp`. A poke lasts until the game
restarts, and a level load reinitialises the guest's own globals over it.
