"""Exercise EXEC traversal independently of real game shader assets."""
import struct
import importlib.util
from pathlib import Path

spec = importlib.util.spec_from_file_location('texture_audit',
    Path(__file__).with_name('audit_texture_instructions.py'))
audit = importlib.util.module_from_spec(spec)
spec.loader.exec_module(audit)
fetches = audit.fetches


def container(instructions):
    code = b''.join(struct.pack('>III', *words) for words in instructions)
    header = bytearray(36)
    struct.pack_into('>III', header, 0, 0x102A1100, 36, len(code))
    struct.pack_into('>I', header, 24, 28)
    struct.pack_into('>II', header, 28, 0, len(code))
    return bytes(header) + bytes(4) + code


def pair(a, b):
    return a & 0xffffffff, (a >> 32) | ((b & 65535) << 16), b >> 16


def execute(address, count, sequence, opcode=1):
    return (opcode << 44) | (sequence << 16) | (count << 12) | address


# Both 48-bit halves execute, and duplicate EXEC references are deduplicated.
# Instruction 3 resembles a texture fetch, but EXEC identifies it as ALU.
texture = (1 | (7 << 20) | (1 << 25), (3 << 12) | (2 << 14) | (1 << 16),
           (2 << 14) | 1 | (0x78 << 2))
data = container([pair(execute(2, 3, 0b010001), execute(2, 1, 1, 3)),
                  pair(execute(5, 1, 1, 13), 0),
                  texture, texture, (0, 0, 0), (24, 0, 0)])
stage, records = fetches(data)
assert stage == 0
assert [r['instruction'] for r in records] == [2, 5]
assert [r['opcode'] for r in records] == [1, 24]
assert records[0]['slot'] == 7 and records[0]['denormalized']
assert records[0]['dimension'] == 2 and records[0]['register_gradients']
assert records[0]['instruction_lod_bias'] == -0.5
assert [records[0][f] for f in ('mag_filter', 'min_filter', 'mip_filter')] == [3, 2, 1]

for broken in (data[:-1], container([pair(execute(2, 1, 1), 0)])):
    try:
        fetches(broken)
    except ValueError:
        pass
    else:
        raise AssertionError('Malformed extent accepted')
print('PASS: paired EXEC, conditional EXEC, deduplication, ALU/vertex exclusion, fields and malformed extents')
