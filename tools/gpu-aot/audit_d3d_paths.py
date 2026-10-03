"""Read-only direct-call audit of generated TU23 code and native hook coverage.

This is a candidate map, not a proof of runtime reachability or correctness.
Indirect calls, data-flow-derived packet writers and hook semantics require review.
"""
import argparse
from contextlib import redirect_stdout
from collections import defaultdict, deque
from pathlib import Path
import re

FUNCTION = re.compile(r"DEFINE_REX_FUNC\(sub_([0-9A-Fa-f]{8})\)\s*\{")
CALL = re.compile(r"\b(?:__imp__)?sub_([0-9A-Fa-f]{8})\(ctx, base\)")
HOOK = re.compile(r"REX_HOOK(?:_RAW)?\(\s*sub_([0-9A-Fa-f]{8})")


def audit(root):
    symbols = defaultdict(list)
    ambiguous = 0
    for line in (root / 'tools/gpu-aot/d3d-sep13-matches.tsv').read_text().splitlines():
        if line.startswith('#'):
            ambiguous += 1
            continue
        fields = line.split('\t')
        if len(fields) == 4:
            symbols[int(fields[2], 16)].append(fields[0])
    hooks = set()
    for path in (root / 'rexlego/src/gpu_native').glob('*.cpp'):
        hooks.update(int(x, 16) for x in HOOK.findall(path.read_text()))
    callers = defaultdict(set)
    locations = {}
    indirect = set()
    for path in sorted((root / 'rexlego/generated/default').glob('*recomp*.cpp')):
        text = path.read_text()
        starts = list(FUNCTION.finditer(text))
        for index, match in enumerate(starts):
            address = int(match[1], 16)
            body = text[match.end():starts[index + 1].start() if index + 1 < len(starts) else len(text)]
            locations[address] = path.name
            for call in CALL.findall(body):
                callers[int(call, 16)].add(address)
            if 'REX_CALL_INDIRECT' in body:
                indirect.add(address)
    # Walk reverse direct calls, STOP at hooks: their original body isn't
    # necessarily executed. RAW hooks may explicitly call the original, so
    # even this barrier is not proof that the path is safe.
    # Verified symbol map: CBlocker::Check, plus the sample-count PM4 writer
    # at the last crash. This is NOT an exhaustive list of packet writers.
    seeds = (0x83FAF180, 0x83FCBF00)
    paths = {seed: [seed] for seed in seeds}
    todo = deque(seeds)
    while todo:
        target = todo.popleft()
        for caller in sorted(callers[target]):
            if caller not in paths:
                paths[caller] = [caller] + paths[target]
                if caller not in hooks:
                    todo.append(caller)
    print(f'# Functions={len(locations)} hooks={len(hooks)} named_addresses={len(symbols)} ambiguous_rows={ambiguous}')
    print(f'# Blocker/query-writer reverse-call candidates={len(paths)} (includes seeds and hook boundaries)')
    print('address\thooked\tindirect\tring_path\tdirect_callers\tsymbols')
    for address in sorted(set(symbols) | set(paths)):
        chain = ' -> '.join(f'{x:08X}' for x in paths.get(address, []))
        print(f'{address:08X}\t{int(address in hooks)}\t{int(address in indirect)}\t{chain}\t{len(callers[address])}\t' + '; '.join(symbols[address]))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument('--output', type=Path, help='Optional generated TSV report')
    args = parser.parse_args()
    if args.output:
        with args.output.open('w', encoding='utf-8', newline='') as report:
            with redirect_stdout(report):
                audit(args.root)
    else:
        audit(args.root)
