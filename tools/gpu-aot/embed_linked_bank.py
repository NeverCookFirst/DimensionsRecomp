#!/usr/bin/env python3
"""Stream a generated linked DXIL bank into metadata C++ and an MSVC ABI COFF blob.

The original bank remains the input/provenance artifact. Only its compressed
byte-array definition moves to a read-only object section; headers, tables and
size constants retain their original source text. No C++ payload is compiled.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile


MAX_LINE = 64 * 1024
MAX_METADATA = 16 * 1024 * 1024
DECLARATION = re.compile(rb"const uint8_t g_compressedLinkedDxilCache\[\] =\s*(\{)?\s*")
HEX_LITERAL = re.compile(rb'"(?:\\x[0-9A-Fa-f]{2})*"')
SYMBOL = "?g_compressedLinkedDxilCache@@3QBEB"
OUTPUTS = ("linked-bank-metadata.cpp", "linked-bank.bin", "linked-bank.s",
           "linked-bank.obj", "linked-bank-manifest.json")


def require(condition, message):
    if not condition:
        raise ValueError(message)


def identity(path):
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return {"bytes": path.stat().st_size, "sha256": digest.hexdigest()}


def stat_identity(stat):
    return stat.st_dev, stat.st_ino, stat.st_size, stat.st_mtime_ns


class Lines:
    def __init__(self, source):
        self.source = source
        self.digest = hashlib.sha256()
        self.bytes = 0
        self.number = 0

    def read(self):
        raw = self.source.readline(MAX_LINE + 1)
        require(len(raw) <= MAX_LINE, "Generated source line exceeds 64 KiB")
        self.digest.update(raw)
        self.bytes += len(raw)
        self.number += bool(raw)
        return raw, raw.rstrip(b"\r\n").strip()

    def next(self):
        raw, content = self.read()
        require(raw, "Truncated generated bank")
        return raw, content

    def expect(self, wanted):
        _, content = self.next()
        require(content == wanted,
                f"Unexpected generated structure on line {self.number}: expected {wanted!r}")


def numeric(lines, sink, terminator):
    digest = hashlib.sha256()
    count = 0
    while True:
        _, content = lines.next()
        if content == terminator:
            return count, digest.hexdigest()
        require(not content or content.endswith(b","), "Numeric initializer must end in a comma")
        pending = bytearray()
        for token in content.split(b",")[:-1]:
            token = token.strip()
            require(1 <= len(token) <= 3 and token.isdigit(), "Invalid numeric payload token")
            value = int(token)
            require(value <= 255, "Numeric payload byte exceeds 255")
            pending.append(value)
        sink.write(pending)
        digest.update(pending)
        count += len(pending)


def literals(lines, sink=None):
    digest = hashlib.sha256()
    count = 0
    while True:
        _, content = lines.next()
        if content == b"#if defined(__clang__)":
            break
        require(HEX_LITERAL.fullmatch(content), "Invalid escaped payload literal")
        decoded = bytes.fromhex(content[1:-1].replace(b"\\x", b"").decode("ascii"))
        if sink is not None:
            sink.write(decoded)
        digest.update(decoded)
        count += len(decoded)
    lines.expect(b"#pragma clang diagnostic pop")
    lines.expect(b"#endif")
    lines.expect(b"#endif")
    lines.expect(b";")
    return count, digest.hexdigest()


def extract(source_path, metadata_path, binary_path):
    before = source_path.stat()
    prefix = bytearray()
    footer = bytearray()
    literal_identity = None
    with source_path.open("rb") as source, metadata_path.open("wb") as metadata, \
            binary_path.open("wb") as binary:
        require(stat_identity(before) == stat_identity(os.fstat(source.fileno())),
                "Input changed before it could be opened")
        lines = Lines(source)
        while True:
            raw, content = lines.next()
            match = DECLARATION.fullmatch(content)
            if match:
                break
            prefix.extend(raw)
            require(len(prefix) <= MAX_METADATA, "Oversized linked table/header metadata")
            metadata.write(raw)
        if match[1]:
            source_format = "numeric"
            count, digest = numeric(lines, binary, b"};")
        else:
            source_format = "numeric-with-literal-fallback"
            lines.expect(b"#if defined(_MSC_VER) && !defined(__clang__)")
            lines.expect(b"{")
            count, digest = numeric(lines, binary, b"}")
            for wanted in (b"#else", b"#if defined(__clang__)",
                           b"#pragma clang diagnostic push",
                           b'#pragma clang diagnostic ignored "-Woverlength-strings"', b"#endif"):
                lines.expect(wanted)
            literal_identity = literals(lines)
        while True:
            raw, content = lines.read()
            if not raw:
                break
            require(not DECLARATION.fullmatch(content), "Duplicate compressed-array definition")
            footer.extend(raw)
            require(len(prefix) + len(footer) <= MAX_METADATA, "Oversized linked metadata")
            metadata.write(raw)
        binary.flush()
        os.fsync(binary.fileno())
        metadata.flush()
        os.fsync(metadata.fileno())
        source_identity = {"bytes": lines.bytes, "sha256": lines.digest.hexdigest()}
        require(stat_identity(before) == stat_identity(os.fstat(source.fileno())) ==
                stat_identity(source_path.stat()), "Input changed during extraction")

    def scalar(data, name):
        matches = re.findall(rb"const size_t " + name + rb"\s*=\s*(\d+)\s*;", data)
        require(len(matches) == 1, f"Expected exactly one {name.decode()} constant")
        return int(matches[0])

    entries = scalar(prefix, b"g_linkedShaderCacheEntryCount")
    compressed = scalar(footer, b"g_linkedDxilCacheCompressedSize")
    decompressed = scalar(footer, b"g_linkedDxilCacheDecompressedSize")
    if compressed == 0:
        # The C++ numeric emitter uses a zero sentinel for an empty array;
        # there are no compressed payload bytes or implicit string NULs to embed.
        require((count == 0 and digest == hashlib.sha256(b"").hexdigest()) or
                (count == 1 and digest == hashlib.sha256(b"\0").hexdigest()),
                "Invalid empty-array numeric sentinel")
        with binary_path.open("wb"):
            pass
        count, digest = 0, hashlib.sha256(b"").hexdigest()
    require(compressed == count, "Compressed size metadata differs from numeric payload length")
    require(literal_identity is None or literal_identity == (count, digest),
            "Numeric and literal payload bytes differ")
    require(b"g_compressedLinkedDxilCache" not in prefix + footer,
            "Metadata still contains a compressed-array definition/reference")
    return {"input_format": source_format, "input": {"path": str(source_path), **source_identity},
            "compressed_payload": {"bytes": count, "sha256": digest},
            "decompressed_bytes": decompressed, "linked_entries": entries,
            "numeric_and_literal_identical": literal_identity is not None,
            "metadata_preserved": True, "payload_type": "const uint8_t[]",
            "implicit_string_nul_embedded": False}


def assembly(binary_path):
    # Quoted assembler names preserve MSVC C++ array mangling. JSON quoting is
    # used only for assembler string syntax; subprocess never invokes a shell.
    return ('.section .rdata,"dr"\n.p2align 4\n'
            f'.globl "{SYMBOL}"\n"{SYMBOL}":\n'
            f'.incbin {json.dumps(str(binary_path), ensure_ascii=False)}\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path)
    parser.add_argument("output", type=Path, help="Directory for five generated linked-bank artifacts")
    parser.add_argument("--llvm-mc", default="llvm-mc")
    parser.add_argument("--arch", choices=("x86_64", "aarch64"), default="x86_64")
    parser.add_argument("--timeout", type=int, default=60, help="Assembly timeout in seconds")
    args = parser.parse_args()
    require(args.timeout > 0, "Assembly timeout must be positive")
    source = args.input.resolve(strict=True)
    output = args.output.resolve()
    require(source.is_file(), "Input must be a generated source file")
    require(source not in [output / name for name in OUTPUTS], "Output cannot replace the input")
    output.mkdir(parents=True, exist_ok=True)
    input_identity = stat_identity(source.stat())
    with tempfile.TemporaryDirectory(prefix=".linked-bank-stage-", dir=output) as temporary:
        stage = Path(temporary)
        record = extract(source, stage / OUTPUTS[0], stage / OUTPUTS[1])
        stage_assembly = stage / OUTPUTS[2]
        stage_assembly.write_text(assembly(stage / OUTPUTS[1]), encoding="utf-8")
        target = args.arch + "-pc-windows-msvc"
        subprocess.run([args.llvm_mc, "-triple=" + target, "-filetype=obj",
                        str(stage_assembly), "-o", str(stage / OUTPUTS[3])],
                       check=True, timeout=args.timeout)
        require((stage / OUTPUTS[3]).stat().st_size > 0, "Assembler produced an empty object")
        # The published assembly refers to the published blob, not its temporary
        # name. Assembly changes no payload/metadata and is not an input to this run.
        stage_assembly.write_text(assembly(output / OUTPUTS[1]), encoding="utf-8")
        record.update(target=target, coff_symbol=SYMBOL, assembler=args.llvm_mc,
                      outputs={name: identity(stage / name) for name in OUTPUTS[:-1]})
        (stage / OUTPUTS[4]).write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")
        require(stat_identity(source.stat()) == input_identity,
                "Input changed before publication; previous outputs preserved")
        # All parsing, comparison and assembly complete before publishing. Each
        # replacement is atomic; the manifest is the final completion marker.
        for name in OUTPUTS:
            os.replace(stage / name, output / name)
    print(json.dumps(record, indent=2))


if __name__ == "__main__":
    main()
