"""Match statically linked Xbox 360 SDK functions from a MAP-labelled image.

The matcher deliberately ignores relocated immediates and branch displacements
while retaining instruction opcodes and register fields. This makes a function
compiled from the same XDK library comparable after the linker moves code and
data to different guest addresses.
"""

from __future__ import annotations

import argparse
import pathlib
import re
import struct

BASE = 0x82000000
MAP_FUNCTION = re.compile(
    r"^\s*\S+\s+(?P<name>\S+)\s+(?P<address>[0-9A-Fa-f]{8})\s+f\s+(?P<object>\S+)"
)


def normalize(word: int) -> int:
    opcode = word >> 26
    # Relative branches: target displacement is link-position dependent.
    if opcode == 18:
        return word & 0xFC000003
    if opcode == 16:
        return word & 0xFFFF0003
    # Immediate arithmetic, compare, logical and D-form load/store operations.
    if opcode in {
        7, 8, 10, 11, 12, 13, 14, 15,
        24, 25, 26, 27, 28, 29,
        *range(32, 56),
    }:
        return word & 0xFFFF0000
    return word


def normalized_words(image: bytes) -> list[int]:
    usable = len(image) & ~3
    return [normalize(word[0]) for word in struct.iter_unpack(">I", image[:usable])]


def read_sdk_symbols(path: pathlib.Path) -> list[tuple[str, int, str]]:
    result = []
    with path.open("r", encoding="latin-1", errors="replace") as stream:
        for line in stream:
            match = MAP_FUNCTION.match(line)
            if match and match.group("object").lower().startswith("d3d9:"):
                result.append(
                    (match.group("name"), int(match.group("address"), 16), match.group("object"))
                )
    return result


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--map", required=True, type=pathlib.Path)
    parser.add_argument("--reference", required=True, type=pathlib.Path)
    parser.add_argument("--target", required=True, type=pathlib.Path)
    parser.add_argument("--words", type=int, default=16)
    parser.add_argument("--filter", default="")
    parser.add_argument("--output", type=pathlib.Path)
    args = parser.parse_args()

    reference = normalized_words(args.reference.read_bytes())
    target = normalized_words(args.target.read_bytes())
    width = args.words
    if width < 4:
        parser.error("--words must be at least 4")

    selected = re.compile(args.filter, re.IGNORECASE) if args.filter else None
    symbols = []
    signatures: dict[int, dict[tuple[int, ...], list[int]]] = {}
    for name, address, obj in read_sdk_symbols(args.map):
        if selected and not selected.search(name):
            continue
        offset = (address - BASE) // 4
        if offset < 0 or offset + width > len(reference):
            continue
        signature = tuple(reference[offset : offset + width])
        symbol_index = len(symbols)
        symbols.append((name, address, obj, signature))
        by_signature = signatures.setdefault(signature[0], {})
        by_signature.setdefault(signature, []).append(symbol_index)

    hits: list[list[int]] = [[] for _ in symbols]
    for i in range(len(target) - width + 1):
        by_signature = signatures.get(target[i])
        if not by_signature:
            continue
        signature = tuple(target[i : i + width])
        for symbol_index in by_signature.get(signature, ()):
            hits[symbol_index].append(BASE + i * 4)

    lines = []
    matched = ambiguous = missing = 0
    for (name, address, obj, _), symbol_hits in zip(symbols, hits):
        if len(symbol_hits) == 1:
            matched += 1
            lines.append(f"{name}\t0x{address:08X}\t0x{symbol_hits[0]:08X}\t{obj}")
        elif len(symbol_hits) > 1:
            ambiguous += 1
            joined = ",".join(f"0x{hit:08X}" for hit in symbol_hits[:8])
            lines.append(f"# ambiguous {name}\t0x{address:08X}\t{joined}\t{obj}")
        else:
            missing += 1

    lines.append(f"# matched={matched} ambiguous={ambiguous} missing={missing}")
    text = "\n".join(lines) + "\n"
    if args.output:
        args.output.write_text(text, encoding="utf-8")
    else:
        print(text, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
