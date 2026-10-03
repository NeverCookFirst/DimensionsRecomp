"""Audit TU23 sampler initializer data and direct branches without running it.

Uses the mapped PPC image and unchanged generated functions. The image must
have the expected 0x82000000 mapping. This is static evidence, not a live
device initialization or visual test.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', type=Path, default=Path('xexdump/dump/default.bin'))
    parser.add_argument('--generated', type=Path, default=Path('rexlego/generated/default'))
    parser.add_argument('--initializer', type=Path,
                        default=Path('rexlego/src/gpu_native/hooks_device.cpp'))
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    image = args.image.read_bytes()
    base = 0x82000000

    def word(at):
        assert base <= at <= base + len(image) - 4
        return struct.unpack_from('>I', image, at - base)[0]

    native = args.initializer.read_text()
    allowed_text = re.search(r'allowed\{([^}]+)\}', native).group(1)
    allowed = {int(h, 16) for h in re.findall(r'0x([A-Fa-f0-9]+)', allowed_text)}
    table = [tuple(word(0x847F9FD8 + k * 12 + d * 4) for d in range(3))
             for k in range(20)]
    assert {t[1] for t in table} == allowed and len(allowed) == 20
    bodies = {}
    for path in args.generated.glob('*.cpp'):
        source = path.read_text()
        for setter in allowed:
            start = source.find(f'DEFINE_REX_FUNC(sub_{setter:08X}) {{')
            if start < 0:
                continue
            assert setter not in bodies, f'Duplicate function {setter:08X}'
            end = source.find('\nDEFINE_REX_FUNC(', start + 1)
            bodies[setter] = source[start:end if end >= 0 else len(source)]
    assert set(bodies) == allowed
    helper_targets = {0x83E44F7C, 0x83E44FCC}  # __save/__restgprlr_29
    records = []
    for k, (getter, setter, default) in enumerate(table):
        body = bodies[setter]
        instructions = re.findall(r'^\s*// ([^\n]+)', body, re.M)
        assert instructions
        branches = []
        for index, assembly in enumerate(instructions):
            at = setter + 4 * index
            value = word(at)
            opcode = value >> 26
            # Direct branches must match the generated branch destination.
            if opcode == 18:
                displacement = value & 0x03FFFFFC
                if displacement & 0x02000000:
                    displacement -= 0x04000000
                target = (displacement if value & 2 else at + displacement) & 0xFFFFFFFF
                match = re.fullmatch(r'b[l]? 0x([0-9a-f]+)', assembly.strip())
                assert match and int(match[1], 16) == target, (hex(at), assembly)
                assert setter <= target < setter + 4 * len(instructions) or target in helper_targets
                if not setter <= target < setter + 4 * len(instructions):
                    branches.append(f'{target:08X}')
            elif opcode == 16:
                displacement = value & 0xFFFC
                if displacement & 0x8000:
                    displacement -= 0x10000
                target = (displacement if value & 2 else at + displacement) & 0xFFFFFFFF
                assert setter <= target < setter + 4 * len(instructions), (hex(at), assembly)
            elif opcode == 19:
                # bclr is a return; bcctr would imply an indirect call/queue path.
                assert ((value >> 1) & 1023) != 528, (hex(at), assembly)
            # Branch comments are also checked in the other direction so a
            # mismatched image cannot hide a generated direct call.
            if re.fullmatch(r'b[l]? 0x[0-9a-f]+', assembly.strip()):
                assert opcode == 18, (hex(at), assembly)
        generated_calls = set(re.findall(r'\b(sub_[0-9A-F]+|__\w+)\(ctx, base\)', body))
        assert generated_calls <= {'__savegprlr_29', '__restgprlr_29'}, generated_calls
        records.append({'index': k, 'getter': f'{getter:08X}', 'setter': f'{setter:08X}',
                        'default': f'{default:08X}', 'instruction_count': len(instructions),
                        'external_branch_targets': branches,
                        'image_code_sha256': hashlib.sha256(
                            image[setter-base:setter-base+4*len(instructions)]).hexdigest(),
                        'generated_body_sha256': hashlib.sha256(body.encode()).hexdigest()})
    constants = {f'{at:08X}': struct.unpack_from('>f', image, at-base)[0]
                 for at in (0x8202776C, 0x8204EA60)}
    assert constants == {'8202776C': 32.0, '8204EA60': -8.0}
    report = {
        'note': 'Static TU23 data/branch audit; not proof of runtime initialization or visuals',
        'image': str(args.image.resolve()),
        'image_sha256': hashlib.sha256(image).hexdigest(),
        'image_base': f'{base:08X}',
        'sampler_table': '847F9FD8',
        'table_sha256': hashlib.sha256(image[0x847F9FD8-base:0x847F9FD8-base+240]).hexdigest(),
        'exact_whitelist_match': True,
        'no_external_queue_branches': True,
        'float_constants': constants,
        'initializers': records,
    }
    args.output.write_text(json.dumps(report, indent=2), encoding='utf-8')
    print('TU23 sampler defaults audited: 20 exact whitelist entries, leaf/save-restore branches only')


if __name__ == '__main__':
    main()
