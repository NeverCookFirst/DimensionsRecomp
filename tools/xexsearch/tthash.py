"""The TT Games name hash (sub_82A29A28 in LEGO Dimensions).

Names are not stored as strings at runtime: scenes, types and assets are
referred to by a 32-bit hash, so a constant like 0x470DAD6B in the code is a
name nobody wrote down. This reproduces the hash so candidate names can be
tested against one.

It is FNV-1a with two twists: lowercase letters are folded to uppercase before
mixing, and the accumulator is kept as a 64-bit value while each round's
multiply truncates it to int32 first. The high half is therefore not garbage -
the game passes the full 64-bit value around, and it is reproduced here so a
comparison against a logged return value matches exactly.

The second argument is a seed, and every call site has its own. The scene lookup
in the character-grid path uses 0x811C9DC5.

    py -3 tthash.py SlotSpecialsScene 811C9DC5
    py -3 tthash.py --find 470DAD6B 811C9DC5 SlotSpecialsScene PartyPlus ...

Verified against the running game: hash("SlotSpecialsScene", 0x811C9DC5)
== FF9E43D367B7FEA3, which is exactly what the title returned.
"""
import sys

PRIME = 0x01000193
MASK64 = 0xFFFFFFFFFFFFFFFF


def tthash(text, seed):
    h = seed & MASK64
    for ch in text.encode("ascii"):
        c = ch - 32 if 97 <= ch <= 122 else ch
        if c >= 0x80:
            c -= 0x100
        lo = h & 0xFFFFFFFF
        if lo >= 0x80000000:
            lo -= 0x100000000
        h = ((lo * PRIME) & MASK64) ^ (c & MASK64)
    return h & MASK64


def main(argv):
    if len(argv) >= 2 and argv[0] == "--find":
        target = int(argv[1], 16)
        seed = int(argv[2], 16)
        for name in argv[3:]:
            h = tthash(name, seed)
            if (h & 0xFFFFFFFF) == (target & 0xFFFFFFFF):
                print("MATCH  %-40s %016X" % (name, h))
        return 0
    if len(argv) != 2:
        print(__doc__)
        return 1
    print("%016X" % tthash(argv[0], int(argv[1], 16)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
