"""Verify nonblocking native settings receipt/rollback and frontend protocol.

Production transaction, service and frontend are executed. The whole-request
coordinator/OS/GPU completion boundary is injected; no title or window runs.
"""
from pathlib import Path
import argparse, hashlib, json, shutil, subprocess


def main():
    p = argparse.ArgumentParser(__doc__)
    p.add_argument('output', type=Path)
    p.add_argument('--compiler', default='clang++')
    a = p.parse_args()
    root = Path(__file__).resolve().parents[2]
    src = root / 'rexlego/src'
    sdk = root.parent / 'dev-assets/sdk-install'
    out = a.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    for name in ['rexruntime.dll', 'amd_fidelityfx_dx12.dll']:
        shutil.copy2(root.parent / 'build/worlds-candidate-v18' / name, out / name)
    fixture = Path(__file__).with_suffix('.cpp')
    flags = ['-std=c++23', '-O2', '-DWIN32_LEAN_AND_MEAN', '-DNOMINMAX', '-D_CRT_SECURE_NO_WARNINGS',
             '-Wno-deprecated-literal-operator', '-I' + str(src), '-I' + str(sdk / 'include')]
    objects = {}

    def compile(source, label):
        obj = out / (label + '.obj')
        result = subprocess.run([a.compiler, *flags, '-c', str(source), '-o', str(obj)], capture_output=True, text=True, timeout=90)
        (out / (label + '-compile.log')).write_text(result.stdout + result.stderr)
        result.check_returncode()
        return obj

    sources = [fixture, *[src / (name + '.cpp') for name in
               ['settings_transaction', 'settings_service', 'settings_frontend', 'settings_registry', 'settings_config']]]
    for source in sources:
        objects[source.stem] = compile(source, source.stem)

    def run(label, replacement=None):
        selected = dict(objects)
        if replacement:
            name, old, new = replacement
            text = (src / (name + '.cpp')).read_text()
            assert text.count(old) == 1, (label, text.count(old))
            mutant = out / (label + '.cpp')
            mutant.write_text(text.replace(old, new))
            selected[name] = compile(mutant, label)
        exe = out / (label + '.exe')
        result = subprocess.run([a.compiler, *map(str, selected.values()), str(sdk / 'lib/rexruntime.lib'), '-o', str(exe)],
                                capture_output=True, text=True, timeout=90)
        (out / (label + '-link.log')).write_text(result.stdout + result.stderr)
        result.check_returncode()
        result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=20)
        (out / (label + '.log')).write_text(result.stdout + result.stderr)
        return result

    positive = run('production')
    positive.check_returncode()
    print(positive.stdout, end='', flush=True)
    negative = []
    for label, unit, old, new in [
        ('accept-stale-receipt', 'settings_transaction', 'if (result.revision != revision_) return;', 'if (false) return;'),
        ('confirm-before-apply-ack', 'settings_transaction',
         'if (result.state == RevisionState::kComplete) return FinishAsyncApply(now);\n    return true;',
         'return FinishAsyncApply(now);'),
        ('countdown-at-request', 'settings_transaction',
         'deadline_ = now + (needs_confirmation_ ? confirmation_ : std::chrono::seconds(15));',
         'deadline_ = operation_deadline_ - std::chrono::seconds(30) + (needs_confirmation_ ? confirmation_ : std::chrono::seconds(15));'),
        ('finish-before-rollback-ack', 'settings_transaction',
         'if (result.state == RevisionState::kComplete) return FinishAsyncRestore();\n    return true;',
         'return FinishAsyncRestore();'),
        ('exit-on-pending-cancel', 'settings_frontend',
         'reply_.ok && reply_.phase == Phase::kClosed &&',
         'reply_.ok && (reply_.phase == Phase::kClosed || reply_.phase == Phase::kRestoring) &&'),
        ('no-pending-frontend-poll', 'settings_frontend',
         'const bool asynchronous = reply_.phase == Phase::kApplying || reply_.phase == Phase::kRestoring;',
         'const bool asynchronous = false;'),
        ('focus-keeps-pending-apply', 'settings_service',
         'if (state_->transaction.pending() || state_->transaction.phase() == Phase::kPreview ||',
         'if (state_->transaction.phase() == Phase::kPreview ||'),
    ]:
        result = run(label, (unit, old, new))
        assert result.returncode == 23, (label, result.returncode, result.stdout, result.stderr)
        negative.append(dict(name=label, exit=result.returncode, error=result.stderr))
    files = sources + [Path(__file__).resolve(), *[src / (name + '.h') for name in
            ['settings_transaction', 'settings_service', 'settings_frontend', 'settings_registry', 'settings_config']]]
    (out / 'verification.json').write_text(json.dumps(dict(passed=True, game_launched=False, output=positive.stdout,
        negative=negative, sources={str(f): hashlib.sha256(f.read_bytes()).hexdigest() for f in files},
        scope=__doc__, limits=[
            'The coordinator is injected: live renderer/window submit/observe is not wired by this test.',
            'Physical window/resource readiness, renderer batch atomicity, full shutdown coordinator teardown and native title XML/dialog drawing require separate implementation/verification.',
        ]), indent=2))


if __name__ == '__main__':
    main()
