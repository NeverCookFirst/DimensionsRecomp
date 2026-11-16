"""Differentially check the title constant publisher against its generated PPC body."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess


def function(text, name):
    start = text.index('DEFINE_REX_FUNC(' + name + ') {')
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
    parser.add_argument('--benchmark', action='store_true')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[3]
    generated = root / 'source/rexlego/generated/default'
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    specifications = [
        (142, 'sub_82B7ABF0', '__imp__sub_82B7ABF0'),
        (154, '__savegprlr_24', '__savegprlr_24'),
        (271, '__restgprlr_24', '__restgprlr_24'),
        (415, 'sub_83FB6480', 'oracle_vertex_constants'),
        (178, 'sub_83FB6558', 'oracle_pixel_constants'),
    ]
    bodies, hashes = [], {}
    for number, name, replacement in specifications:
        path = generated / f'legodimensions_recomp.{number}.cpp'
        body = function(path.read_text(), name)
        hashes[name] = dict(path=str(path.relative_to(root)), sha256=hashlib.sha256(body.encode()).hexdigest())
        bodies.append(body.replace('DEFINE_REX_FUNC(' + name + ')', 'DEFINE_REX_FUNC(' + replacement + ')', 1))
    (output / 'native_constants_oracles.inc').write_text('\n\n'.join(bodies))
    exe = output / 'native-constants.exe'
    flags = subprocess.IDLE_PRIORITY_CLASS | subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0
    command = [args.compiler, '-std=c++23', '-O2', '-march=x86-64-v3', '-DNOMINMAX', '-DWIN32_LEAN_AND_MEAN',
        '-DSPDLOG_COMPILED_LIB', '-DSPDLOG_FMT_EXTERNAL', '-fms-runtime-lib=dll', '-Wl,/nodefaultlib:libcmt',
        '-I', str(generated), '-I', str(output), '-I', str(root / 'source/rexlego/src'),
        '-isystem', str(root / 'dev-assets/sdk-install/include'),
        str(Path(__file__).with_suffix('.cpp')), str(root / 'source/rexlego/src/native_constants.cpp'), '-o', str(exe)]
    compiled = subprocess.run(command, capture_output=True, text=True, timeout=180, creationflags=flags)
    (output / 'compile.log').write_text(compiled.stdout + compiled.stderr)
    if compiled.returncode:
        print(compiled.stdout + compiled.stderr)
        compiled.check_returncode()
    results = []
    for mode in ('1', '0'):
        run = subprocess.run([str(exe), *(['--benchmark'] if args.benchmark and mode == '1' else [])],
            env=dict(os.environ, LEGO_NATIVE_CONSTANT_PUBLISHER=mode,
                LEGO_NATIVE_CONSTANT_PUBLISHER_FILE=str(output / f'control-{mode}.txt')), capture_output=True, text=True,
            timeout=180, creationflags=flags)
        (output / f'mode-{mode}.log').write_text(run.stdout + run.stderr)
        print(run.stdout + run.stderr, end='')
        run.check_returncode()
        assert 'PASS 2400 differential cases' in run.stdout
        assert 'PASS live control:' in run.stdout
        results.append(dict(native=mode, exit=run.returncode, stdout=run.stdout))
    source = (root / 'source/rexlego/src/native_constants.cpp').read_text()
    clear = 'REX_STORE_U8(u32(r.r11) + 4, ctx.r24.u8);'
    assert source.count(clear) == 1
    mutant = output / 'missing-dirty-clear.cpp'
    mutant.write_text(source.replace(clear, '(void)0;'))
    mutant_exe = output / 'missing-dirty-clear.exe'
    mutant_command = command.copy()
    mutant_command[-3] = str(mutant)
    mutant_command[-1] = str(mutant_exe)
    compiled = subprocess.run(mutant_command, capture_output=True, text=True, timeout=180, creationflags=flags)
    (output / 'negative-compile.log').write_text(compiled.stdout + compiled.stderr)
    if compiled.returncode:
        print(compiled.stdout + compiled.stderr)
        compiled.check_returncode()
    negative = subprocess.run([str(mutant_exe)], env=dict(os.environ, LEGO_NATIVE_CONSTANT_PUBLISHER='1'),
        capture_output=True, text=True, timeout=60, creationflags=flags)
    (output / 'negative.log').write_text(negative.stdout + negative.stderr)
    assert negative.returncode == 23, negative.stdout + negative.stderr
    print('PASS negative control: missing dirty clear rejected')
    report = dict(passed=True, oracles=hashes, runs=results,
        negative_control=dict(missing_dirty_clear=True, exit=negative.returncode, stderr=negative.stderr),
        scope='Exact generated publisher and VS/PS copy helpers; controlled stateful prelude and callback mutation; full context and guest-memory comparison, not a gameplay result')
    (output / 'verification.json').write_text(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
