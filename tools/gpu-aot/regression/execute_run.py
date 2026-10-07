#!/usr/bin/env python3
"""Execute one verified private renderer run; refuse concurrent game processes."""
import sys
sys.dont_write_bytecode = True
import argparse
import fcntl
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import time

HERE = Path(__file__).resolve().parent
INTRINSIC_PREFIXES = ('LEGO_NATIVE_', 'LEGO_GPU_', 'LEGO_DUMP_', 'REX_')
INTRINSIC_KEYS = ('XENIA_TOYPAD_PORT', 'REXGLUE_TOYPAD_PORT', 'PROTON_LOG',
                  'VKD3D_SHADER_CACHE_PATH', 'DXVK_STATE_CACHE_PATH', 'PLUME_D3D12_STATE_CACHE')


def active_games():
    found = []
    for process in Path('/proc').iterdir():
        if not process.name.isdecimal():
            continue
        try:
            arguments = (process / 'cmdline').read_bytes().split(b'\0')
        except (FileNotFoundError, PermissionError, ProcessLookupError):
            continue
        if any(arg.decode(errors='replace').replace('\\', '/').rsplit('/', 1)[-1].lower()
               == 'legodimensions.exe' for arg in arguments):
            found.append(int(process.name))
    return sorted(found)


def environment(plan):
    result = dict(os.environ)
    prefixes = tuple(plan['remove_inherited_prefixes']) + INTRINSIC_PREFIXES
    keys = tuple(plan['remove_inherited_keys']) + INTRINSIC_KEYS
    for key in list(result):
        if key.startswith(prefixes) or key in keys:
            result.pop(key)
    for key, value in plan['environment_overrides'].items():
        if not isinstance(value, str) or '\0' in value:
            raise ValueError('Invalid planned environment value')
        result[key] = value
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('run', type=Path)
    parser.add_argument('--launch', action='store_true')
    parser.add_argument('--workspace', type=Path, required=True)
    args = parser.parse_args()
    spec = importlib.util.spec_from_file_location('regression', HERE / 'prepare_run.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    run = args.run.resolve(strict=True)
    verification = module.verify(run, args.workspace)
    for name in ('logs', 'cache', 'metadata', 'hotload', 'captures',
                 'shader-capture', 'texture-capture', 'toypad-tags', 'no-mod-game'):
        directory = run / name
        if directory.is_symlink() or not directory.is_dir() or directory.resolve().parent != run:
            raise ValueError('Writable run directory is not a private directory: ' + name)
    plan = verification['launch_plan']
    if Path(plan['cwd']).resolve() != run:
        raise ValueError('Launch cwd differs from the verified private run')
    if (not isinstance(plan['argv'], list) or len(plan['argv']) != 6
            or not all(isinstance(value, str) for value in plan['argv'])):
        raise ValueError('Invalid planned launch command')
    if plan['argv'] != ['nice', '-n', '10', str(Path(plan['argv'][3]).resolve()),
                        'runinprefix', str(run / 'legodimensions.exe')]:
        raise ValueError('Launch command differs from the expected private executable')
    env = environment(plan)
    active = active_games()
    report = {'unix': time.time(), 'run': str(run), 'active_game_pids': active,
              'route': env.get('LEGO_NATIVE_PM4_REFERENCE'), 'argv': plan['argv'],
              'cwd': plan['cwd'], 'prepared_identity_verified': True,
              'launch_requested': args.launch, 'environment_overrides': plan['environment_overrides']}
    if not args.launch:
        print(json.dumps(report))
        return
    lock_root = args.workspace.resolve(strict=True)
    lock_path = lock_root / '.renderer-regression-execution.lock'
    if lock_path.is_symlink():
        raise ValueError('Execution lock may not be a symlink')
    lock = os.fdopen(os.open(lock_path, os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW, 0o600), 'a')
    try:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        raise RuntimeError('Another regression runner owns the game execution lock')
    active = active_games()
    if active:
        raise RuntimeError('Existing game process must close before an isolated run: ' + str(active))
    evidence = run / 'logs/execution.json'
    if evidence.exists():
        raise RuntimeError('Run already has execution evidence; prepare a fresh run')
    module.write_json(evidence, report, exclusive=True)
    with (run / 'logs/launcher.log').open('x') as output:
        child = subprocess.Popen(plan['argv'], cwd=plan['cwd'], env=env,
                                 stdout=output, stderr=subprocess.STDOUT, start_new_session=True)
        report['launcher_pid'] = child.pid
        report['started_unix'] = time.time()
        module.write_json(evidence, report)
        print(json.dumps({'run': str(run), 'launcher_pid': child.pid, 'route': report['route']}), flush=True)
        report['exit_code'] = child.wait()
        report['finished_unix'] = time.time()
        module.write_json(evidence, report)
    raise SystemExit(report['exit_code'])


if __name__ == '__main__':
    main()
