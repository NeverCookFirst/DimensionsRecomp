"""Differentially verify the production immutable fetch planner and dynamic calls."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess


def function(text, name):
    start = text.index('bool ' + name + '(')
    end = text.index('{', start) + 1
    depth = 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    parser.add_argument('--compiler', default='clang++')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[3]
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    draw = (root / 'source/rexlego/src/gpu_native/draw.cpp').read_text()
    names = ['PrepareDrawGeometryLegacy', 'PrepareDrawGeometry']
    bodies = {name: function(draw, name) for name in names}
    (output / 'vertex_fetch_plan_production.inc').write_text('\n\n'.join(bodies.values()))
    flags = subprocess.IDLE_PRIORITY_CLASS | subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0
    exe = output / 'vertex-fetch-plan.exe'
    command = [args.compiler, '-std=c++23', '-O2', '-march=x86-64-v3', '-DNOMINMAX',
        '-DWIN32_LEAN_AND_MEAN', '-D_CRT_SECURE_NO_WARNINGS', '-I', str(output),
        '-I', str(root / 'source/rexlego/src'), '-isystem', str(root / 'dev-assets/sdk-install/include'),
        str(Path(__file__).with_suffix('.cpp')), '-o', str(exe)]

    def compile_case(command, label):
        compiled = subprocess.run(command, capture_output=True, text=True, timeout=180, creationflags=flags)
        (output / (label + '.log')).write_text(compiled.stdout + compiled.stderr)
        if compiled.returncode:
            print(compiled.stdout + compiled.stderr)
            compiled.check_returncode()

    compile_case(command, 'compile')
    runs = []
    modes = [('1', '1', '1'), ('0', '1', '1'), ('1', '0', '1'), ('0', '0', '1'), ('1', '1', '0')]
    for windows, skip, signature, lru in [(*mode, lru) for lru in ('1', '0') for mode in modes]:
        label = f'windows-{windows}-skip-{skip}-signature-{signature}-lru-{lru}'
        env = dict(os.environ, LEGO_NATIVE_VERTEX_FETCH_PLAN='0' if windows == '0' else '1',
            LEGO_NATIVE_VERTEX_FETCH_PLAN_FILE=str(output / (label + '-control.txt')),
            LEGO_NATIVE_VERTEX_FETCH_PLAN_LRU=lru,
            LEGO_NATIVE_VERTEX_FETCH_PLAN_LRU_FILE=str(output / (label + '-cache-control.txt')),
            LEGO_NATIVE_BUFFER_WINDOWS=windows, LEGO_NATIVE_SKIP_UNUSED_VERTEX_STREAMS=skip,
            LEGO_NATIVE_INPUT_SIGNATURE_CACHE=signature)
        result = subprocess.run([str(exe)], env=env, capture_output=True, text=True, timeout=180, creationflags=flags)
        (output / (label + '.log')).write_text(result.stdout + result.stderr)
        print(label + ':\n' + result.stdout + result.stderr, end='')
        result.check_returncode()
        assert 'PASS 19209 production planner differential cases' in result.stdout
        assert 'PASS live control:' in result.stdout
        assert 'PASS live cache control:' in result.stdout
        assert 'PASS 20800 old/new cache semantic comparisons' in result.stdout
        runs.append(dict(mode=label, exit=result.returncode, output=result.stdout))

    helper = (root / 'source/rexlego/src/gpu_native/vertex_fetch_plan.h').read_text()
    mutations = [
        ('missing-width-check', 'if (!stream.widths_valid || stream.attribute_end > stride) return false;',
            'if (stream.attribute_end > stride) return false;'),
        ('missing-recency-update', 'found->second.used = used;', '/* deliberately omit recency */'),
        ('clear-all-instead-of-lru', 'entries_.erase(oldest);', 'entries_.clear();')]
    negatives = []
    for label, check, replacement in mutations:
        assert helper.count(check) == 1
        negative = output / label / 'gpu_native'
        negative.mkdir(parents=True, exist_ok=True)
        (negative / 'vertex_fetch_plan.h').write_text(helper.replace(check, replacement))
        negative_exe = output / (label + '.exe')
        mutant = command[:1] + ['-I', str(negative.parent)] + command[1:]
        mutant[-1] = str(negative_exe)
        compile_case(mutant, label + '-compile')
        result = subprocess.run([str(negative_exe)], capture_output=True, text=True,
            timeout=60, creationflags=flags)
        (output / (label + '.log')).write_text(result.stdout + result.stderr)
        assert result.returncode == 23, result.stdout + result.stderr
        print('PASS negative control: ' + label + ' was detected')
        negatives.append(dict(mutation=label, exit=result.returncode, stderr=result.stderr))
    report = dict(passed=True,
        source_hashes={name: hashlib.sha256(body.encode()).hexdigest() for name, body in bodies.items()},
        helper_sha256=hashlib.sha256(helper.encode()).hexdigest(), runs=runs,
        negative_controls=negatives,
        scope='Production planners, pure range oracle, old/new cache eviction semantics and retained owner lifetime; controlled resource callbacks verify every dynamic call and argument. No gameplay or GPU performance claim.')
    (output / 'verification.json').write_text(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
