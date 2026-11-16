"""Exercise actual revision coordinator/transaction/service with injected actors.

Registry/window checks and render/presentation work are boundaries; this does
not run an app entrypoint, platform window, graphics device or the game.
"""
from pathlib import Path
import argparse, hashlib, json, subprocess


def main():
    p = argparse.ArgumentParser(__doc__)
    p.add_argument('output', type=Path)
    p.add_argument('--compiler', default='clang++')
    a = p.parse_args()
    root = Path(__file__).resolve().parents[2]
    src = root / 'rexlego/src'
    out = a.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    fixture = Path(__file__).with_suffix('.cpp')
    production = src / 'settings_revision.cpp'
    flags = ['-std=c++23', '-O2', '-I' + str(src)]
    objects = []
    for source in [fixture, src / 'settings_transaction.cpp', src / 'settings_service.cpp']:
        obj = out / (source.stem + '.obj')
        result = subprocess.run([a.compiler, *flags, '-c', str(source), '-o', str(obj)], capture_output=True, text=True, timeout=60)
        (out / (source.stem + '-compile.log')).write_text(result.stdout + result.stderr)
        result.check_returncode()
        objects.append(obj)

    def run(label, text):
        source, exe = out / (label + '.cpp'), out / (label + '.exe')
        source.write_text(text)
        result = subprocess.run([a.compiler, *flags, str(source), *map(str, objects), '-o', str(exe)],
                                capture_output=True, text=True, timeout=60)
        (out / (label + '-compile.log')).write_text(result.stdout + result.stderr)
        result.check_returncode()
        result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=20)
        (out / (label + '.log')).write_text(result.stdout + result.stderr)
        return result

    text = production.read_text()
    positive = run('production', text)
    positive.check_returncode()
    print(positive.stdout, end='', flush=True)
    negative = []
    for label, old, new in [
        ('complete-after-registry-only', 'acknowledged == kComplete ?', 'sealed ?'),
        ('accept-superseded-render-commit', 'state.sealed != ticket || ticket->id != state.sequence ||', ''),
        ('write-inline-submit', 'request->values[key] = value;', 'registry.write(key,value); request->values[key] = value;'),
        ('skip-mutation-lease', 'owns(!flag.test_and_set(std::memory_order_acquire))', 'owns(true)'),
        ('accept-foreign-window-receipt', ' && result.revision == id)', ')'),
        ('retain-stopped-window-callback', 'state.window_check = {};', ''),
    ]:
        assert text.count(old) == 1, (label, text.count(old))
        result = run(label, text.replace(old, new))
        assert result.returncode == 23, (label, result.returncode, result.stdout, result.stderr)
        negative.append(dict(name=label, exit=result.returncode, error=result.stderr))
    files = [production, src / 'settings_revision.h', src / 'settings_transaction.cpp', src / 'settings_transaction.h',
             src / 'settings_service.cpp', src / 'settings_service.h', fixture, Path(__file__).resolve()]
    (out / 'verification.json').write_text(json.dumps(dict(passed=True, game_launched=False, output=positive.stdout,
        negative=negative, sources={str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in files},
        scope=__doc__, limits=['Actual native renderer snapshot consumers, window probe, presentation callbacks and production host lifecycle still require wiring and separate verification.']), indent=2))


if __name__ == '__main__':
    main()
