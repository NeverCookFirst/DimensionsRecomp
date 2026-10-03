"""Extract the unchanged TU23 SetTexture body for the differential unit test.

The test supplies a bounded BE guest-memory/CPU harness. The Xbox queue path
must not run; only the texture/fetch/dirty shadow writes are compared.
"""
import argparse
from pathlib import Path

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--generated', type=Path, default=Path('rexlego/generated/default'))
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    matches = []
    for path in args.generated.glob('*.cpp'):
        source = path.read_text()
        begin = source.find('DEFINE_REX_FUNC(sub_83FB58A8) {')
        if begin == -1:
            continue
        end = source.find('\nDEFINE_REX_FUNC(', begin + 1)
        matches.append(source[begin:end if end != -1 else len(source)])
    if len(matches) != 1:
        raise SystemExit(f'Expected one TU23 function, found {len(matches)}')
    args.output.write_text(matches[0], encoding='utf-8')
    print(f'Extracted unchanged SetTexture into {args.output}')
