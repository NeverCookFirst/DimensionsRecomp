#!/usr/bin/env python3
"""Record one fixed native cadence interval; never capture, launch or send input."""
import sys
sys.dont_write_bytecode = True
import argparse
from collections import deque
import csv
import hashlib
import io
import json
import math
from pathlib import Path
import re
import statistics
import time
import tomllib

from prepare_run import digest, owned, read_json, require, require_sha, windows, write_json

RESOURCE_EVENTS = re.compile(
    r'adopted guest texture|placement buffer relocation|refreshed borrowed|'
    r'dumped missing shader|loaded additive precompiled pack')
FAILURE_EVENTS = re.compile(r'\[(?:error|critical)\]|Native GPU:.*(?:failed|failure|device removed)', re.I)
HEAVY_FLAGS = ('LEGO_NATIVE_TRACE_DIR', 'LEGO_NATIVE_DRAW_TRACE_TRIGGER',
               'LEGO_GPU_SNAPSHOT_DIR', 'LEGO_NATIVE_MESH_TRACE',
               'LEGO_NATIVE_AUDIT_BUFFER_WATCH', 'LEGO_NATIVE_AUDIT_TEXTURE_WATCH')
CRITERIA = {'resource_quiet_seconds': 10, 'cadence_rows': 60,
            'median_halves_ratio_max': 1.25, 'maximum_warmup_seconds': 120,
            'interval_seconds': 30}
MAX_CONTEXT_LOG_BYTES = 16*1024**2
MAX_INTERVAL_LOG_BYTES = 8*1024**2


def complete_rows(path):
    # A long-lived CSV cannot grow this sampler's memory without bound. Seek to
    # a bounded tail, discard its initial partial row, and keep at most10k rows.
    with path.open('rb') as source:
        header = source.readline(65536)
        require(header.endswith(b'\n'), 'Missing bounded CSV header')
        source.seek(0, 2)
        end = source.tell()
        begin = max(len(header), end-4*1024**2)
        source.seek(begin)
        if begin != len(header): source.readline()
        payload_start = source.tell()
        raw = source.read(4*1024**2)
    last_newline = raw.rfind(b'\n')
    require(last_newline >= 0, 'No complete frame metrics yet')
    size = payload_start+last_newline+1
    raw = header+raw[:last_newline+1]
    rows = deque(maxlen=10000)
    for row in csv.DictReader(io.StringIO(raw.decode())):
        require(None not in row and all(v is not None for v in row.values()), 'Malformed complete CSV row')
        values = {k: float(v) for k, v in row.items()}
        require(all(math.isfinite(v) for v in values.values()), 'Non-finite metric')
        require(values['frame'].is_integer() and values['frame'] >= 0 and values['interval_ms'] > 0,
                'Invalid frame/interval metric')
        rows.append(values)
    rows = list(rows)
    require(rows, 'No complete frame metrics yet')
    require(all(a['frame'] < b['frame'] for a, b in zip(rows, rows[1:])), 'Non-monotonic frame IDs')
    return size, list(rows)


def workers():
    """Endpoint observations supplement, rather than prove, the operator quiet hold."""
    result = []
    for process in Path('/proc').iterdir():
        if not process.name.isdecimal():
            continue
        try:
            args = (process/'cmdline').read_bytes().split(b'\0')
            command = args[0].decode(errors='replace').replace('\\', '/').rsplit('/', 1)[-1].lower()
            if command in ('clang', 'clang++', 'clang-cl', 'ninja', 'llvm-mc', 'dotnet',
                           'gradient_compiler.exe', 'lego_shader_prelink.exe', 'lego_shader_indexer.exe'):
                result.append({'pid': int(process.name), 'command': command})
        except OSError:
            continue
    return result


def endpoint(run):
    size, rows = complete_rows(run/'logs/frames.csv')
    return {'unix': time.time(), 'monotonic': time.monotonic(), 'frame': int(rows[-1]['frame']),
            'csv_bytes': size, 'log_bytes': (run/'logs/game.log').stat().st_size,
            'observed_workers': workers()}, rows


def interval_log(run, first, last):
    require(last['log_bytes'] >= first['log_bytes'], 'Game log rotated/truncated during scope')
    delta = last['log_bytes']-first['log_bytes']
    require(delta <= MAX_INTERVAL_LOG_BYTES, 'Game interval log exceeds bounded evidence size')
    with (run/'logs/game.log').open('rb') as source:
        source.seek(first['log_bytes'])
        data = source.read(delta)
    require(len(data) == delta, 'Game interval log truncated during evidence read')
    return data.decode(errors='replace')


def stats(rows):
    require(rows, 'Empty selected interval')
    times = sorted(r['interval_ms'] for r in rows)
    mean = lambda k: statistics.fmean(r[k] for r in rows)
    result = {'frames': len(rows), 'recorded_seconds': sum(times)/1000,
              'fps': 1000/mean('interval_ms'), 'p50_ms': times[(len(times)-1)//2],
              'p95_ms': times[int((len(times)-1)*.95)], 'p99_ms': times[int((len(times)-1)*.99)],
              'maximum_ms': times[-1], 'draw_calls_mean': mean('draw_calls')}
    for key in ('frame_slot_wait_calls', 'callbacks_enqueued', 'callbacks_executed', 'callbacks_pending'):
        if key in rows[0]:
            result[key+'_mean'] = mean(key)
    result['detailed_timing_enabled'] = any(r['detailed_timing_enabled'] != 0 for r in rows)
    return result


def context(run, scene_file, workspace):
    run = owned(run, workspace).resolve(strict=True)
    manifest = read_json(run/'regression-manifest.json')
    require(manifest['run_root'] == str(run) and manifest['workspace'] == str(workspace.resolve()),
            'Prepared run/workspace identity differs')
    require(manifest.get('cadence_only_probe') and manifest['renderer_requested'] == 'native',
            'Cadence sampling requires a native cadence-only descriptor')
    execution = read_json(run/'logs/execution.json', 128*1024)
    require(execution.get('exit_code') is None and execution.get('launch_requested') and
            execution.get('route') == '0', 'No live native execution evidence')
    env = execution['environment_overrides']
    require(env == manifest['launch_plan']['environment_overrides'], 'Launch environment differs from preparation')
    require('LEGO_NATIVE_TIMING' not in env and not any(k in env for k in HEAVY_FLAGS),
            'Cadence scope contains detailed/heavy instrumentation')
    mode = manifest['timer_wait']
    require((mode == 'spin' and 'REX_TIMER_WAIT_BLOCKING' not in env) or
            (mode == 'blocking' and env.get('REX_TIMER_WAIT_BLOCKING') == '1'), 'Timer mode disagrees')
    require(env['LEGO_NATIVE_COMMAND_SLOTS'] == str(manifest['command_slots']) and
            env['LEGO_NATIVE_FRAME_METRICS'] == windows(run/'logs/frames.csv'), 'Slot/CSV target differs')
    log_path = run/'logs/game.log'
    require(not log_path.is_symlink() and log_path.stat().st_size <= MAX_CONTEXT_LOG_BYTES,
            'Context game log exceeds bounded evidence size or aliases another path')
    with log_path.open('rb') as source: log_bytes = source.read(MAX_CONTEXT_LOG_BYTES+1)
    require(len(log_bytes) <= MAX_CONTEXT_LOG_BYTES, 'Context game log grew beyond its read bound')
    log = log_bytes.decode(errors='replace')
    require('GPU route: detached native D3D12 renderer' in log and
            f'Native GPU init: creating {manifest["command_slots"]} command slots' in log,
            'Actual runtime route/slot initialization evidence is missing')
    scene = read_json(scene_file, 128*1024)
    require(scene.get('run_root') == str(run) and
            scene.get('build_fingerprint') == manifest['frozen_build']['build_fingerprint'] and
            scene.get('renderer') == 'native', 'Scene evidence does not identify this native run')
    for key in ('scene_id', 'camera_id'):
        require(isinstance(scene.get(key), str) and bool(scene[key].strip()), 'Missing scene/camera identity')
    for key in ('stationary', 'camera_unchanged', 'inputs_released', 'workers_held'):
        require(scene.get(key) is True, 'Operator must explicitly confirm '+key)
    require(isinstance(scene.get('figures'), list) and scene['figures'], 'Missing loaded-figure confirmation')
    expected = {f['name']: {k: f[k] for k in ('name', 'pad', 'index', 'uid')} for f in manifest['figures']}
    require(len({f['name'] for f in scene['figures']}) == len(scene['figures']) and
            all(f == expected.get(f['name']) for f in scene['figures']), 'Confirmed figures differ from prepared tags')
    capture = Path(scene['capture_path']).resolve(strict=True)
    require_sha(scene['capture_sha256'])
    require(digest(capture)['sha256'] == scene['capture_sha256'], 'Scene capture no longer matches its evidence')
    pins = {Path(r['path']).name: r for r in manifest['copies'] if Path(r['path']).parent == run}
    binaries = {name: digest(run/name) for name in pins if name.lower().endswith(('.dll', '.exe'))}
    require(all(identity['sha256'] == pins[name]['sha256'] for name, identity in binaries.items()),
            'Live executable/DLL dependencies differ from preparation')
    require(binaries['legodimensions.exe']['sha256'] == manifest['frozen_build']['executable_sha256'],
            'Live binary differs from frozen EXE')
    require(binaries['rexruntime.dll']['sha256'] == pins['rexruntime.dll']['sha256'], 'Live runtime differs from preparation')
    require(digest(run/'legodimensions.toml')['sha256'] == manifest['config_sha256'], 'Live config changed')
    config = tomllib.loads((run/'legodimensions.toml').read_text())
    config = {key: value.replace(windows(run), '<PRIVATE-RUN>') if isinstance(value, str) else value
              for key, value in config.items()}
    supporting = {}
    for record in manifest['copies']:
        relative = Path(record['path']).relative_to(run)
        if relative.parts[0] not in ('content', 'update', 'update-mods', 'mods', 'toypad-tags'):
            continue
        supporting[str(relative)] = ({'bytes': record['bytes'], 'sha256': record['sha256']}
                                     if 'sha256' in record else
                                     {'bytes': record['bytes'], 'source': record['source'],
                                      'source_mtime_ns': record['source_mtime_ns'],
                                      'content_sha256_verified': False})
    pack_file = run/'hotload/publications.json'
    publications = read_json(pack_file, 1024*1024) if pack_file.exists() else []
    accepted = [int(v) for v in re.findall(r'loaded additive precompiled pack, generation=(\d+)', log)]
    require([entry['generation'] for entry in publications] == accepted,
            'Pack publication provenance differs from actual accepted generations')
    packs = []
    for entry in publications:
        source = Path(entry['source'])
        require_sha(entry['sha256'])
        require(digest(source)['sha256'] == entry['sha256'], 'Published pack source changed')
        with source.open('rb') as stream: header = stream.read(160)
        require(header[:8] == b'LEGODX1\0' and header[32:48].decode() == manifest['frozen_build']['build_fingerprint'],
                'Published pack belongs to a different frozen build')
        packs.append({'generation': entry['generation'], 'sha256': entry['sha256']})
    return {'run': str(run), 'scene': scene, 'scene_confirmation': {'source': str(scene_file.resolve()), **digest(scene_file)},
            'binaries': binaries, 'slots': manifest['command_slots'], 'criteria': dict(CRITERIA),
            'timer_wait': mode,
            'flags': env, 'normalized_config': config, 'shader_packs': packs,
            'supporting_state': supporting,
            'checkpoint': manifest['checkpoint_files'],
            'status': 'warming', 'warmup': [],
            'limits': ['Operator evidence confirms scene/input/quiet ownership; this tool cannot inspect gameplay state.',
                       'Ordinary desktop activity remains; endpoint worker observations are not continuous profiling.',
                       'CSV buffering shifts recorded duration relative to wall endpoints.',
                       'Disabled timing, conversion and hash columns do not measure zero work.']}


def sample(run, output, scene_file, workspace, *, take_endpoint=endpoint,
           sleep=time.sleep, clock=time.monotonic):
    require(not output.exists(), 'Refuse overwriting any prior sample outcome')
    report = context(run, scene_file, workspace)
    owned(output, workspace).mkdir(parents=True)
    write_json(output/'sample.json', report)
    deadline = clock()+CRITERIA['maximum_warmup_seconds']
    try:
        while True:
            first, _ = take_endpoint(run)
            sleep(CRITERIA['resource_quiet_seconds'])
            last, rows = take_endpoint(run)
            text = interval_log(run, first, last)
            tail = rows[-CRITERIA['cadence_rows']:]
            require(len(tail) == CRITERIA['cadence_rows'], 'Too few cadence rows for warm criterion')
            times = [r['interval_ms'] for r in tail]
            medians = [statistics.median(times[:30]), statistics.median(times[30:])]
            ratio = max(medians)/min(medians)
            check = {'start': first, 'end': last, 'resource_events': len(RESOURCE_EVENTS.findall(text)),
                     'cadence_median_halves_ratio': ratio}
            report['warmup'].append(check)
            write_json(output/'sample.json', report)
            if FAILURE_EVENTS.search(text):
                raise ValueError('Render/runtime failure during warmup')
            if (not check['resource_events'] and last['frame'] > first['frame'] and
                    not first['observed_workers'] and not last['observed_workers'] and
                    ratio <= CRITERIA['median_halves_ratio_max']):
                break
            require(clock() < deadline, 'Fixed settling criteria not achieved before deadline')
        first, _ = take_endpoint(run)
        report['start'] = first
        report['status'] = 'sampling'
        write_json(output/'sample.json', report)
        sleep(CRITERIA['interval_seconds'])
        last, rows = take_endpoint(run)
        selected = [r for r in rows if first['frame'] < r['frame'] <= last['frame']]
        require([int(r['frame']) for r in selected] == list(range(first['frame']+1, last['frame']+1)),
                'Selected frames are not contiguous')
        require(selected and all(r['detailed_timing_enabled'] == 0 for r in selected), 'Detailed timing was enabled')
        text = interval_log(run, first, last)
        (output/'game-interval.log').write_text(text)
        with (output/'frames.csv').open('x', newline='') as target:
            writer = csv.DictWriter(target, fieldnames=list(selected[0]))
            writer.writeheader()
            writer.writerows(selected)
        reasons = []
        if RESOURCE_EVENTS.search(text): reasons.append('Resource/pack/shader streaming during interval')
        if FAILURE_EVENTS.search(text): reasons.append('Render/runtime failure during interval')
        if first['observed_workers'] or last['observed_workers']: reasons.append('Compiler/scanner observed at endpoint')
        report.update(end=last, wall_seconds=last['monotonic']-first['monotonic'], metrics=stats(selected),
                      rejection_reasons=reasons, status='rejected' if reasons else 'accepted')
    except BaseException as error:
        report.update(status='rejected', rejection_reasons=[type(error).__name__+': '+str(error)])
        write_json(output/'sample.json', report)
        raise
    write_json(output/'sample.json', report)
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('run', type=Path)
    parser.add_argument('--workspace', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--scene-confirmation', type=Path, required=True)
    args = parser.parse_args()
    print(json.dumps(sample(args.run, args.output, args.scene_confirmation, args.workspace), indent=2))


if __name__ == '__main__':
    main()
