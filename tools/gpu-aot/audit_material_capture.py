"""Audit saved native draws against simultaneous headers and upload metadata.

Read-only, offline, no game or graphics API. Uploads from another frame are
comparison candidates, never evidence of a simultaneous header mutation.
"""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import re


def words(text):
    result = [int(x, 16) for x in text.rstrip(',').split(',')]
    if len(result) != 6 or any(x > 0xFFFFFFFF for x in result):
        raise ValueError('Expected six unsigned fetch dwords')
    return result


def decode(f):
    dimension = (f[5] >> 9) & 3
    if dimension == 0:
        width, height, depth = (f[2] & 0xFFFFFF) + 1, 1, 1
    elif dimension == 2:
        width, height, depth = (f[2] & 2047) + 1, ((f[2] >> 11) & 2047) + 1, (f[2] >> 22) + 1
    else:
        width, height, depth = (f[2] & 8191) + 1, ((f[2] >> 13) & 8191) + 1, 1
    size_max = max(width, height, depth).bit_length() - 1
    minimum = min((f[4] >> 2) & 15, size_max) if f[5] & 0x1FFFF000 else 0
    maximum = max(minimum, min((f[4] >> 6) & 15, size_max)) if f[5] & 0x1FFFF000 else 0
    if maximum and not f[1] & 0x1FFFF000:
        minimum = max(minimum, 1)
    maximum = max(minimum, maximum)
    mip_filter = (f[3] >> 23) & 3
    if mip_filter == 2:
        maximum = minimum + (0 if (f[3] >> 25) & 7 else 0.25)
    bias = (f[4] >> 12) & 1023
    return dict(type=f[0] & 3, dimension=dimension, width=width, height=height,
                depth=depth, format=f[1] & 63, base=f[1] & 0x1FFFF000,
                mip=f[5] & 0x1FFFF000, swizzle=(f[3] >> 1) & 4095,
                requested_mip_min=(f[4] >> 2) & 15,
                requested_mip_max=(f[4] >> 6) & 15,
                min_lod=minimum, max_lod=maximum,
                bias=(bias - 1024 if bias >= 512 else bias) / 32,
                mip_filter=mip_filter)


def mismatch(active, header):
    # Exact resource bits copied by TU23 SetTexture, excluding sampler fields,
    # mip requests and CPU->physical address conversion.
    masks = (0xFFC003FF, 0x7FF, 0xFFFFFFFF, 0x8007FFFF, 0, 0xE00)
    return [i for i, mask in enumerate(masks) if (active[i] ^ header[i]) & mask]


def shader_masks(index):
    """Native draw hashes use the corpus's third hash column (container ID)."""
    rows = re.findall(r'\{\s*0x[0-9A-Fa-f]+,\s*0x[0-9A-Fa-f]+,\s*0x([0-9A-Fa-f]+),\s*\d+,\s*([01]),\s*0x([0-9A-Fa-f]+)\s*\}', index.read_text())
    if not rows:
        raise ValueError('No shader texture masks found in corpus index')
    result = {}
    for shader, stage, mask in rows:
        key = (int(stage), int(shader, 16))
        result[key] = result.get(key, 0) | int(mask, 16)
    return result


def audit(directory, masks=None):
    uploads = {}
    for p in sorted(directory.rglob('texture-uploads/**/*.txt')):
        text = p.read_text()
        values = dict(re.findall(r'(\w+)=([^\s]+)', text))
        if 'guest' in values and 'fetch' in values:
            uploads[int(values['guest'], 16)] = dict(path=str(p.resolve()),
                sha256=hashlib.sha256(p.read_bytes()).hexdigest(),
                frame=int(values.get('frame', -1)), fetch=words(values['fetch']))
    findings, files, counts = [], [], Counter()
    for p in sorted(directory.glob('capture-draws*.draws.tsv')):
        files.append(dict(path=str(p.resolve()), sha256=hashlib.sha256(p.read_bytes()).hexdigest()))
        draw, slots, headers, history = {}, {}, {}, {}

        def finish():
            if not draw:
                return
            counts['draws'] += 1
            counts['submitted' if draw.get('submitted') == '1' else 'submission_unknown_or_failed'] += 1
            used = 0
            known = masks is not None
            for stage, key in ((0, 'VS'), (1, 'PS')):
                shader = int(draw.get(key, '0'), 16)
                if masks is None or (stage, shader) not in masks:
                    known = False
                else:
                    used |= masks[stage, shader]
            if not known:
                counts['draws_without_complete_shader_masks'] += 1
            for slot, (guest, active) in slots.items():
                counts['texture_bindings'] += 1
                if known and not used & (1 << slot):
                    counts['unused_texture_bindings_excluded'] += 1
                    continue
                counts['audited_texture_bindings'] += 1
                d = decode(active)
                reasons = []
                previous = history.get(slot)
                if previous and int(previous.get('changed_resource_words', '0'), 16):
                    reasons.append('resource_fields_changed_after_SetTexture')
                if previous and int(previous['guest'], 16) != guest:
                    reasons.append('bound_pointer_differs_from_last_SetTexture')
                if guest and d['type'] != 2:
                    reasons.append('bound_pointer_with_nontexture_fetch_type')
                    counts['nontexture_fetch_type_' + str(d['type'])] += 1
                    if not any(active[i] for i in (1, 2)):
                        counts['nontexture_fetch_without_resource_address_or_size'] += 1
                if guest and d['requested_mip_max'] and not d['max_lod']:
                    reasons.append('requested_mips_clamped_to_zero')
                reference, kind = None, None
                if slot in headers:
                    reference, kind = headers[slot], 'simultaneous_header'
                elif guest in uploads:
                    reference, kind = uploads[guest]['fetch'], 'different_frame_upload'
                diff = mismatch(active, reference) if reference else []
                if diff:
                    reasons.append('resource_fields_differ_' + kind)
                if reasons:
                    for reason in reasons:
                        counts[reason] += 1
                    findings.append(dict(file=p.name, draw=draw, slot=slot,
                        guest=f'{guest:08X}', active=[f'{w:08X}' for w in active],
                        decoded=d, shader_usage_known=known, reasons=reasons, resource_word_mismatches=diff,
                        reference_kind=kind,
                        reference=decode(reference) if reference else None,
                        upload=uploads.get(guest)))
                    findings[-1]['last_set_texture'] = previous
        for line in p.read_text().splitlines():
            if line.startswith('draw='):
                finish()
                draw = dict(re.findall(r'(\w+)=([^\s]+)', line))
                slots, headers, history = {}, {}, {}
            elif line.startswith('texture_slot='):
                m = re.fullmatch(r'texture_slot=(\d+) guest=([0-9a-fA-F]+) fetch=([0-9a-fA-F,]+)', line)
                if not m:
                    raise ValueError(f'Malformed texture row in {p}: {line}')
                slots[int(m[1])] = int(m[2], 16), words(m[3])
            elif line.startswith('texture_header_slot='):
                m = re.fullmatch(r'texture_header_slot=(\d+) fetch=([0-9a-fA-F,]+)', line)
                if m:
                    headers[int(m[1])] = words(m[2])
            elif line.startswith('texture_last_set_slot='):
                values = dict(re.findall(r'(\w+)=([^\s]+)', line))
                history[int(values.pop('texture_last_set_slot'))] = values
            elif line.startswith('submitted='):
                draw['submitted'] = line.split('=', 1)[1]
        finish()
    return dict(game_launched=False, visual_fix_proven=False,
        warning='Different-frame uploads cannot establish a simultaneous header mismatch. '
                'Unreferenced slots and zero-fetch native-owned headers may be legitimate.',
        counts=dict(counts), files=files, findings=findings)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--index', type=Path, help='Saved corpus shader index, to exclude unused slots')
    args = parser.parse_args()
    report = audit(args.capture, shader_masks(args.index) if args.index else None)
    if args.index:
        report['shader_index'] = dict(path=str(args.index.resolve()), sha256=hashlib.sha256(args.index.read_bytes()).hexdigest())
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report['counts'], sort_keys=True))
