"""Trace captured Dimensions container IDs to local Avengers material families.

Uses existing private extracted assets, index and XXH3 helper. Never downloads,
launches a game, or writes copyrighted bytecode into source control. A shared
name signature is a family candidate, not proof of identical shader semantics.
"""
import argparse
import ctypes
import hashlib
import json
from pathlib import Path
import re
import runpy
import struct


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--research', type=Path, required=True)
    p.add_argument('--hash-dll', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('hashes', nargs='+')
    a = p.parse_args()
    a.output.mkdir(parents=True, exist_ok=True)
    targets = {int(h, 16) for h in a.hashes}
    dll = ctypes.CDLL(str(a.hash_dll.resolve()))
    dll.Hash.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
    dll.Hash.restype = ctypes.c_uint64
    helper = runpy.run_path(str(a.research / 'avgref.py'))
    matches = []
    for source in sorted((a.research / 'all/dim').rglob('*')):
        if not source.is_file():
            continue
        data = source.read_bytes()
        for n, (stage, off, blob) in enumerate(helper['xenos_shaders'](data)):
            if len(blob) < 0x24:
                continue
            virtual, physical = struct.unpack_from('>II', blob, 4)
            if virtual < 0x24 or virtual > len(blob) or physical > len(blob)-virtual:
                continue
            marker = 4 if len(blob)-virtual >= 4 and struct.unpack_from('>I', blob, virtual)[0] == physical else 0
            size = virtual + marker + physical
            if size > len(blob):
                continue
            exact = blob[:size]
            key = dll.Hash(exact, len(exact))
            if key in targets:
                matches.append(dict(hash=f'{key:016X}', asset=helper['asset_key'](str(source), str(a.research/'all/dim'), '_nxg.360_shaders'),
                    n=n, off=off, stage=stage, source=str(source.resolve()), source_sha256=hashlib.sha256(data).hexdigest()))
    index = helper['load']()
    dim = {(e['asset'], e['off']): e for e in index['dim']}
    pc = {}
    for e in index['avg']:
        pc.setdefault(e['sig'], []).append(e)
    def family_for(signature):
        if signature.endswith(':'):
            return [], 'empty_signature', []
        if signature in pc:
            return pc[signature], 'exact_name_signature', []
        # These local color families differ only by the cbuffer aggregate name,
        # with every individual material/light/fog/sampler name still equal.
        # Keep the relaxation explicit; it is not semantic equivalence.
        stage, names = signature.split(':', 1)
        wanted = set(names.split(','))
        candidates = []
        for other, entries in pc.items():
            other_stage, other_names = other.split(':', 1)
            if other_stage == stage and (wanted ^ set(other_names.split(','))) == {'g_MiscGroupPS'}:
                candidates.extend(entries)
        return candidates, 'aggregate_name_variant', ['g_MiscGroupPS'] if candidates else []
    families = {}
    # Report every matched asset, but disassemble one candidate per captured ID.
    selected = set()
    for m in matches:
        entry = dim.get((m['asset'], m['off']))
        if entry is None:
            m['error'] = 'not in saved signature index'
            continue
        if entry['sig'] not in families:
            families[entry['sig']] = family_for(entry['sig'])
        family, match_kind, name_delta = families[entry['sig']]
        m['signature'] = entry['sig']
        m['pc_family_candidates'] = len(family)
        m['match_kind'] = match_kind
        m['name_delta'] = name_delta
        if not family or m['hash'] in selected:
            continue
        candidates = sorted(family, key=lambda e: (e['asset'] != m['asset'], abs(e['size']-entry['size'])))
        twin = candidates[0]
        source = a.research / twin['file']
        data = source.read_bytes()
        blob = data[twin['off']:twin['off']+twin['size']]
        disassembly = helper['disassemble'](blob)
        if disassembly.startswith('(no d3dcompiler'):
            raise RuntimeError(disassembly)
        dest = a.output / (m['hash'] + '-pc-family.asm.txt')
        dest.write_text(disassembly, encoding='utf-8')
        m['selected_candidate'] = dict(twin, source_sha256=hashlib.sha256(data).hexdigest(), bytecode_sha256=hashlib.sha256(blob).hexdigest(), disassembly=str(dest.resolve()))
        selected.add(m['hash'])
    report = dict(game_launched=False, semantic_equivalence_proven=False,
        hash_dll_sha256=hashlib.sha256(a.hash_dll.read_bytes()).hexdigest(),
        reference_helper_sha256=hashlib.sha256((a.research/'avgref.py').read_bytes()).hexdigest(),
        research_index_sha256=hashlib.sha256((a.research/'index.json').read_bytes()).hexdigest(),
        hashes_requested=sorted(f'{x:016X}' for x in targets), matches=matches)
    (a.output/'material-families.json').write_text(json.dumps(report, indent=2)+'\n')
    print(f'{len(matches)} matching Dimensions container occurrences; {len(selected)}/{len(targets)} shader IDs have disassembled PC family candidates')


if __name__ == '__main__':
    main()
