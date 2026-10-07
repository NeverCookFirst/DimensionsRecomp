#!/usr/bin/env python3
"""Prepare isolated legitimate checkpoints and an exact launch descriptor.

Never launches a process, sends a Toypad packet, or provides desktop input.
"""
import argparse
import errno
import fcntl
import hashlib
import json
import mmap
import os
from pathlib import Path
import re
import shutil
import struct
import tempfile
import time
import tomllib

HERE = Path(__file__).resolve().parent
WORKSPACE = Path.cwd()
FICLONE = 0x40049409
MAX_TREE_ENTRIES = 20000
TITLE = '5752084B'
def save_path(xuid):
    require(re.fullmatch(r'[0-9A-Fa-f]{16}', xuid), 'XUID must be sixteen hexadecimal digits')
    return Path(xuid.upper()) / TITLE / '00000001/savegame_1'

BINARY_NAMES = ('legodimensions.exe', 'rexruntime.dll', 'dxcompiler.dll', 'dxil.dll')
SUPPORT_NAMES = ('FiraSans-Regular.ttf', 'achievement_unlocked.wav', 'gamecontrollerdb.txt')
SLOTS = {'batman': (2, 4), 'gandalf': (2, 0), 'wyldstyle': (3, 6), 'batmobile': (1, 1)}
SANITIZED_PREFIXES = ('LEGO_NATIVE_', 'LEGO_GPU_', 'LEGO_DUMP_', 'REX_')
RECIPE = {
    'name': 'operator-confirmed-checkpoint',
    'steps': ['Start the exact verified descriptor after every existing game closes.',
              'Load the recorded private persistent figures using normal game protocol.',
              'Load the legitimate saved game; confirm the scene and camera with external evidence.'],
    'comparison_gate': 'Compare only operator-confirmed same scene, camera, figures, build and flags.',
    'automatic_input': False,
}



def require(value, message):
    if not value:
        raise ValueError(message)


def digest(path):
    before = path.stat()
    sha = hashlib.sha256()
    with path.open('rb') as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b''):
            sha.update(chunk)
    after = path.stat()
    require((before.st_size, before.st_mtime_ns, before.st_ino) ==
            (after.st_size, after.st_mtime_ns, after.st_ino), f'Input changed while hashing: {path}')
    return {'bytes': after.st_size, 'sha256': sha.hexdigest()}


def windows(path):
    return 'Z:' + str(path.absolute()).replace('/', '\\')


def configured_host(value, install):
    require(isinstance(value, str) and value, 'Missing configured asset directory')
    if value.startswith(('Z:\\', 'z:\\')):
        path = Path(value[2:].replace('\\', '/'))
    else:
        require(not re.match(r'^[a-zA-Z]:', value), 'Only local Z: asset references are supported')
        path = Path(value)
        if not path.is_absolute():
            path = install / path
    return path.resolve(strict=True)


def pe_tables(path, maximum_bytes=64*1024**2):
    """Bounded static PE32+ import/export inspection; never loads a DLL."""
    require(0 < path.stat().st_size <= maximum_bytes, 'PE dependency exceeds inspection bound')
    with path.open('rb') as source, mmap.mmap(source.fileno(), 0, access=mmap.ACCESS_READ) as data:
        def unpack(fmt, offset):
            require(0 <= offset <= len(data) - struct.calcsize(fmt), 'PE field outside file')
            return struct.unpack_from(fmt, data, offset)
        require(data[:2] == b'MZ', 'Dependency is not a PE file')
        pe, = unpack('<I', 0x3c)
        require(data[pe:pe+4] == b'PE\0\0', 'Missing PE signature')
        machine, count = unpack('<HH', pe+4)
        optional_size, = unpack('<H', pe+20)
        optional = pe+24
        require(machine == 0x8664 and 0 < count <= 96 and optional_size >= 240 and
                unpack('<H', optional)[0] == 0x20b, 'Expected bounded x64 PE32+ dependency')
        require(unpack('<I', optional+108)[0] >= 2, 'Missing PE import/export directories')
        sections = []
        for index in range(count):
            start = optional+optional_size+40*index
            virtual_size, va, raw_size, raw = unpack('<IIII', start+8)
            require(raw <= len(data) and raw_size <= len(data)-raw, 'PE section outside file')
            sections.append((va, virtual_size, raw, raw_size))
        def offset(rva, size):
            for va, virtual_size, raw, raw_size in sections:
                delta = rva-va
                if 0 <= delta < max(virtual_size, raw_size):
                    require(delta <= raw_size and size <= raw_size-delta, 'PE RVA lacks file-backed bytes')
                    return raw+delta
            raise ValueError('PE RVA outside sections')
        def name(rva, limit=2048):
            at = offset(rva, 1)
            end = data.find(b'\0', at, min(len(data), at+limit))
            require(end != -1, 'Unterminated PE name')
            offset(rva, end-at+1)
            return data[at:end].decode('ascii')
        imports = {}
        rva, size = unpack('<II', optional+120)
        require(rva and 20 <= size <= 1024**2, 'Missing or oversized import table')
        terminated = False
        for index in range(min(size//20, 256)):
            lookup, stamp, forward, name_rva, address = unpack('<IIIII', offset(rva+20*index, 20))
            if not any((lookup, stamp, forward, name_rva, address)):
                terminated = True
                break
            dll = name(name_rva, 256).lower()
            require(re.fullmatch(r'[a-z0-9_.-]+\.dll', dll), 'Unsafe import DLL name')
            require(dll not in imports, 'Duplicate import descriptor')
            symbols = []
            for entry in range(20000):
                thunk, = unpack('<Q', offset((lookup or address)+8*entry, 8))
                if not thunk:
                    break
                symbols.append('#'+str(thunk & 0xffff) if thunk >> 63 else name(thunk+2))
            else:
                raise ValueError('Unterminated import lookup table')
            imports[dll] = symbols
        require(terminated, 'Unterminated import descriptors')
        exports = set()
        export_rva, export_size = unpack('<II', optional+112)
        if export_rva:
            require(40 <= export_size <= 16*1024**2, 'Invalid export table size')
            at = offset(export_rva, 40)
            count, = unpack('<I', at+24)
            names, = unpack('<I', at+32)
            require(count <= 100000, 'Oversized export name table')
            for index in range(count):
                named_rva, = unpack('<I', offset(names+4*index, 4))
                exports.add(name(named_rva))
        return {'imports': imports, 'exports': exports}


def require_sha(value):
    require(isinstance(value, str) and re.fullmatch(r'[0-9a-f]{64}', value),
            'Expected an explicit lowercase SHA256 pin')


def validate_dependencies(install, baseline, staging):
    """Validate pinned primary PE files and their recorded DLL dependencies.

    DLL names and rexruntime symbols are checked statically. OS/DXGI forwarded
    symbols and internal plugin ABI compatibility still require provenance and
    real startup evidence; name closure alone does not establish either.
    """
    tables = {}
    for name in BINARY_NAMES:
        source = baseline/name if name == 'legodimensions.exe' else install/name
        pin = staging['binaries'][name]
        require(digest(source) == {'bytes': pin['size'], 'sha256': pin['sha256']},
                f'Pinned dependency mismatch: {name}')
        # The linked game includes its compressed shader bank and translated
        # CPU code; inspect a bounded mapping rather than copy its large image.
        tables[name] = pe_tables(source, 1024**3 if name.endswith('.exe') else 64*1024**2)
        require(set(tables[name]['imports']) == {dll.lower() for dll in staging['imports'][name]},
                f'Actual PE dependencies differ from provenance: {name}')
    exports = tables['rexruntime.dll']['exports']
    for name, table in tables.items():
        required = table['imports'].get('rexruntime.dll', [])
        require(set(required) <= exports, f'Runtime missing imported symbols: {name}')
    return {'imports': {name: table['imports'] for name, table in tables.items()},
            'runtime_symbols_checked': sum(len(t['imports'].get('rexruntime.dll', [])) for t in tables.values()),
            'scope': 'Exact pinned primary PE DLL names and named rexruntime imports; OS/DXGI symbol resolution requires launch.'}


def owned(path, root=None):
    root = WORKSPACE if root is None else root
    path = path.absolute()
    require(path.resolve().is_relative_to(root.resolve()), f'Output must be owned under {root}: {path}')
    # Avoid an existing symlink parent redirecting future writable output.
    for parent in (path, *path.parents):
        if parent == root.parent:
            break
        require(not parent.is_symlink(), f'Output contains a symlink component: {parent}')
    return path


def regular_tree(root):
    require(root.is_dir() and not root.is_symlink(), f'Missing regular source directory: {root}')
    files = []
    for count, path in enumerate(root.rglob('*'), 1):
        require(count <= MAX_TREE_ENTRIES, f'Source tree exceeds bounded inventory: {root}')
        require(not path.is_symlink(), f'Guest data may not alias a live file: {path}')
        require(path.is_dir() or path.is_file(), f'Unsupported source object: {path}')
        if path.is_file(): files.append(path)
    return sorted(files)


class Copier:
    def __init__(self):
        self.fallback_bytes = 0
        self.records = []

    def copy(self, source, destination, *, expected=None, hash_bytes=False):
        source = source.resolve(strict=True)
        before = source.stat()
        require(source.is_file() and before.st_size <= 4 * 1024**3, f'Invalid source file: {source}')
        if expected:
            require(digest(source) == expected, f'Pinned input mismatch: {source}')
        destination.parent.mkdir(parents=True, exist_ok=True)
        require(not destination.exists(), f'Refuse overwrite: {destination}')
        method = 'reflink'
        try:
            with source.open('rb') as src, destination.open('xb') as dst:
                try:
                    fcntl.ioctl(dst.fileno(), FICLONE, src.fileno())
                except OSError as error:
                    if error.errno not in (errno.EXDEV, errno.EOPNOTSUPP, errno.ENOTTY, errno.EINVAL):
                        raise
                    # Never turn an unsupported large clone into a large unbounded copy.
                    require(before.st_size <= 1024 * 1024 and
                            self.fallback_bytes + before.st_size <= 8 * 1024 * 1024,
                            f'Large input requires COW reflink support: {source}')
                    shutil.copyfileobj(src, dst, 1024 * 1024)
                    self.fallback_bytes += before.st_size
                    method = 'bounded_copy'
        except BaseException:
            destination.unlink(missing_ok=True)
            raise
        after, clone = source.stat(), destination.stat()
        require((before.st_size, before.st_mtime_ns, before.st_ino) ==
                (after.st_size, after.st_mtime_ns, after.st_ino), f'Input changed during clone: {source}')
        require(clone.st_size == after.st_size and
                (clone.st_dev, clone.st_ino) != (after.st_dev, after.st_ino),
                f'Clone aliases or truncates source: {source}')
        record = {'source': str(source), 'path': str(destination), 'bytes': clone.st_size,
                  'method': method, 'source_mtime_ns': after.st_mtime_ns,
                  'independent_inode': True}
        if hash_bytes or expected:
            record.update(digest(destination))
            if expected:
                require({key: record[key] for key in ('bytes', 'sha256')} == expected,
                        f'Cloned pinned bytes mismatch: {destination}')
        self.records.append(record)
        return record


def write_json(path, data, *, exclusive=False):
    descriptor, name = tempfile.mkstemp(prefix='.'+path.name+'-', dir=path.parent)
    temporary = Path(name)
    try:
        with os.fdopen(descriptor, 'w') as target:
            target.write(json.dumps(data, indent=2)+'\n')
        if exclusive:
            # Atomic publication that fails if an existing outcome owns this
            # name. Removing the temporary link leaves an independent inode.
            os.link(temporary, path)
        else:
            temporary.replace(path)
    finally:
        temporary.unlink(missing_ok=True)


def read_json(path, maximum_bytes=16*1024**2):
    require(not path.is_symlink() and path.is_file() and path.stat().st_size <= maximum_bytes,
            'JSON evidence is missing, aliased or exceeds its size bound: '+str(path))
    with path.open('rb') as source:
        data = source.read(maximum_bytes+1)
    require(len(data) <= maximum_bytes, 'JSON evidence grew beyond its read bound')
    return json.loads(data)


def config_text(original, replacements):
    data = tomllib.loads(original)
    data.update(replacements)
    lines = ['# Independent renderer regression setup; checkpoint is a real game save.']
    for key, value in data.items():
        require(re.fullmatch(r'[a-zA-Z_][a-zA-Z0-9_]*', key), f'Unsupported configuration key: {key}')
        require(isinstance(value, (str, bool, int, float)), f'Unsupported configuration value: {key}')
        lines.append(f'{key} = {json.dumps(value, ensure_ascii=False)}')
    text = '\n'.join(lines) + '\n'
    require(tomllib.loads(text) == data, 'Generated TOML disagrees with planned values')
    return text


def prepare(args):
    timer_wait = getattr(args, 'timer_wait', 'spin')
    require(timer_wait in ('spin', 'blocking'), 'Unknown timer wait mode')
    cadence = getattr(args, 'cadence_only', False)
    performance = getattr(args, 'performance', False) or cadence
    gpu_timestamps = getattr(args, 'gpu_timestamps', False)
    require(not gpu_timestamps or args.renderer == 'native',
            'GPU timestamp sidecar requires native route')
    command_slots = getattr(args, 'command_slots', None)
    require(command_slots is None or (cadence and command_slots in (3, 12)),
            'Explicit command slots require native cadence-only mode and count3 or12')
    require(not performance or (args.renderer == 'native' and not args.snapshots),
            'Performance preparation requires native route without snapshots')
    require(re.fullmatch(r'[a-z0-9][a-z0-9-]{0,63}', args.name), 'Invalid run name')
    require(1024 <= args.toypad_port <= 65535 and args.toypad_port != 9191,
            'Use a distinct private Toypad port in1024..65535')
    workspace = args.workspace.resolve(strict=True)
    runs = owned(args.runs, workspace)
    final = owned(runs / args.name, workspace)
    require(not final.exists(), f'Refuse overwrite of a prepared regression: {final}')
    install = args.install.resolve(strict=True)
    baseline = args.baseline.resolve(strict=True)
    checkpoint = args.checkpoint.resolve(strict=True)
    SAVE = save_path(args.xuid)
    for source in (install, baseline, checkpoint, args.tags.resolve(strict=True)):
        require(not final.is_relative_to(source) and not source.is_relative_to(final),
                'Run output must be separate from every mutable source tree')
    for tool in (args.proton, args.proton_prefix, args.steam_client):
        require(tool.exists() and not tool.is_symlink(), f'Missing regular launch prerequisite: {tool}')
    require(args.proton.is_file() and args.proton_prefix.is_dir() and args.steam_client.is_dir(),
            'Proton must be a file; prefix and Steam client must be directories')
    source_config = install / 'legodimensions.toml'
    config_identity = digest(source_config)
    original_config = source_config.read_text()
    require(hashlib.sha256(original_config.encode()).hexdigest() == config_identity['sha256'],
            'Source configuration changed while reading')
    original_values = tomllib.loads(original_config)
    require(original_values.get('mods', '') == '' and original_values.get('modcli_path', '') == '',
            'Renderer comparison preparation requires the recorded no-custom-mod configuration')
    mutable_asset_trees = {}
    for key, directory in (('update_data_root', 'update'), ('mods_update_root', 'update-mods'),
                           ('mods_root', 'mods')):
        source_root = configured_host(original_values[key], install)
        files = regular_tree(source_root)
        require(sum(p.stat().st_size for p in files) <= 24*1024**3,
                f'{key} exceeds bounded logical snapshot size')
        mutable_asset_trees[key] = (directory, source_root, files)
    seed = install / 'content'
    source_files = regular_tree(seed)
    require(sum(p.stat().st_size for p in source_files) <= 24 * 1024**3, 'Content seed exceeds24GiB logical bound')
    metadata = json.loads((checkpoint / 'save-metadata.json').read_text())
    saves = regular_tree(checkpoint / 'savegame_1')
    require({'GAME01', 'V2GAME01', 'OPTS01'} <= {p.name for p in saves}, 'Incomplete legitimate checkpoint')
    require(len(saves) == len(metadata), 'Checkpoint metadata/file set mismatch')
    pinned_saves = {}
    for path in saves:
        require(path.parent == checkpoint / 'savegame_1', 'Checkpoint must contain only flat save files')
        expected = {key: metadata[path.name][key] for key in ('bytes', 'sha256')}
        require(digest(path) == expected, f'Checkpoint is not its recorded legitimate snapshot: {path}')
        pinned_saves[path.name] = expected
    frozen = json.loads((baseline / 'baseline-manifest.json').read_text())
    require(re.fullmatch(r'[0-9a-f]{16}', frozen['build_fingerprint']), 'Invalid frozen build identity')
    staging = json.loads((install / 'native-staging-manifest.json').read_text())
    runtime_pin = frozen.get('frozen_dependencies', {}).get('rexruntime.dll', {}).get('sha256')
    require_sha(runtime_pin)
    require(staging['binaries']['rexruntime.dll']['sha256'] == runtime_pin,
            'Staged runtime differs from the frozen baseline ABI dependency')
    require(staging['binaries']['legodimensions.exe']['sha256'] == frozen['executable_sha256'],
            'Installed dependency manifest belongs to a different frozen build')
    require(digest(baseline / 'legodimensions.exe')['sha256'] == frozen['executable_sha256'],
            'Frozen EXE mismatch')
    plugin_provenance = None
    dependency_proof = validate_dependencies(install, baseline, staging)
    if args.renderer == 'pm4':
        require(args.pm4_plugin is not None and args.pm4_compatibility_proof is not None,
                'PM4 requires plugin and explicit compatibility provenance')
        require(not args.buffer_windows and not args.snapshots, 'Native diagnostic flags do not apply to PM4')
        require_sha(args.pm4_runtime_sha256)
        require_sha(args.pm4_plugin_sha256)
        proof_path = args.pm4_compatibility_proof.resolve(strict=True)
        proof = json.loads(proof_path.read_text())
        require(proof.get('plugin_sha256') == args.pm4_plugin_sha256 and
                proof.get('runtime_sha256') == args.pm4_runtime_sha256 and
                isinstance(proof.get('evidence'), str) and proof['evidence'].strip(),
                'PM4 needs explicit provenance for this exact plugin/runtime ABI pair')
        require(staging['binaries']['rexruntime.dll']['sha256'] == args.pm4_runtime_sha256,
                'PM4 plugin requires its verified matching runtime')
        plugin_identity = digest(args.pm4_plugin)
        require(plugin_identity['sha256'] == args.pm4_plugin_sha256, 'PM4 plugin is not the verified frozen source')
        tables = pe_tables(args.pm4_plugin)
        exports = pe_tables(install / 'rexruntime.dll')['exports']
        require('rexruntime.dll' in tables['imports'], 'PM4 plugin lacks its runtime dependency')
        require(set(tables['imports']['rexruntime.dll']) <= exports,
                'PM4 runtime is missing plugin import symbols')
        existing = {dll.lower() for names in staging['imports'].values() for dll in names}
        existing.update(name.lower() for name in BINARY_NAMES)
        require(set(tables['imports']) <= existing, 'PM4 introduces an unstaged/unreviewed import DLL')
        plugin_provenance = {'source': str(args.pm4_plugin.resolve()), **plugin_identity,
                             'matching_runtime_sha256': args.pm4_runtime_sha256,
                             'compatibility_proof': {'source': str(proof_path), **digest(proof_path), 'declaration': proof},
                             'imports': tables['imports'],
                             'runtime_import_symbols_verified': len(tables['imports']['rexruntime.dll']),
                             'static_import_closure': 'Every plugin DLL dependency is local or already required by the frozen binary set. rexruntime symbols are checked against actual exports. System/DXGI symbol resolution and plugin startup still require a real launch.'}
    tags = args.tags.resolve(strict=True)
    for name in SLOTS:
        require((tags / (name + '.bin')).is_file() and
                (tags / (name + '.bin')).stat().st_size == 180, f'Missing normal persistent tag: {name}')
    runs.mkdir(parents=True, exist_ok=True)
    stage = owned(runs / ('.' + args.name + '-preparing'), workspace)
    require(not stage.exists(), f'Previous incomplete preparation requires inspection: {stage}')
    stage.mkdir()
    copier = Copier()
    try:
        for name in BINARY_NAMES:
            source = baseline / name if name == 'legodimensions.exe' else install / name
            entry = staging['binaries'][name]
            copier.copy(source, stage / name, expected={'bytes': entry['size'], 'sha256': entry['sha256']})
        for name, target in staging['graphics'].items():
            require(name in ('dxgi.dll', 'd3d12.dll', 'd3d12core.dll'), f'Unexpected graphics dependency: {name}')
            copier.copy(Path(target), stage / name, hash_bytes=True)
        for name in SUPPORT_NAMES:
            copier.copy(install / name, stage / name, hash_bytes=True)
        copier.copy(install / 'native-staging-manifest.json', stage / 'native-staging-manifest.json', hash_bytes=True)
        if plugin_provenance:
            copier.copy(args.pm4_plugin, stage / 'rexgpu-xenos.dll',
                        expected={key: plugin_provenance[key] for key in ('bytes', 'sha256')})
        for key, (directory, source_root, files) in mutable_asset_trees.items():
            (stage / directory).mkdir()
            for source in files:
                copier.copy(source, stage / directory / source.relative_to(source_root), hash_bytes=True)
        for source in source_files:
            relative = source.relative_to(seed)
            if relative.is_relative_to(SAVE):
                continue
            copier.copy(source, stage / 'content' / relative, hash_bytes=source.stat().st_size <= 1024 * 1024)
        for name, expected in pinned_saves.items():
            copier.copy(checkpoint / 'savegame_1' / name, stage / 'content' / SAVE / name, expected=expected)
        for name in SLOTS:
            copier.copy(tags / (name + '.bin'), stage / 'toypad-tags' / (name + '.bin'), hash_bytes=True)
        for directory in ('logs', 'captures', 'shader-capture', 'texture-capture', 'cache', 'metadata', 'hotload', 'no-mod-game'):
            (stage / directory).mkdir()
        replacements = {
            'user_data_root': windows(final / 'content'), 'cache_root': windows(final / 'cache'),
            'metadata_root': windows(final / 'metadata'), 'log_file': windows(final / 'logs/game.log'),
            'hid_mappings_file': windows(final / 'gamecontrollerdb.txt'),
            'mods_content_root': windows(final / 'content/0000000000000000' / TITLE / '00000002'),
            **{key: windows(final / directory) for key, (directory, _, _) in mutable_asset_trees.items()},
            'toypad_app_autostart': False, 'toypad_emulation': True,
            'toypad_app_path': '', 'allow_game_relative_writes': False,
            'mods': '', 'modcli_path': '', 'mods_game_root': windows(final / 'no-mod-game'),
            'updates_check': False, 'fullscreen': False, 'gpu_native_pm4': args.renderer == 'pm4',
            'gpu_plugin': 'xenos' if args.renderer == 'pm4' else '',
            'perf_log_csv': windows(final / 'logs/sdk-frames.csv'),
        }
        target_path = getattr(args, 'render_target_path_d3d12', None)
        if target_path is not None:
            require(args.renderer == 'pm4', 'Render-target backend experiment requires PM4')
            require(target_path in ('rov', 'rtv'), 'Unsupported render-target backend')
            replacements['render_target_path_d3d12'] = target_path
        config = config_text(original_config, replacements)
        (stage / 'legodimensions.toml').write_text(config)
        overrides = {
            'STEAM_COMPAT_CLIENT_INSTALL_PATH': str(args.steam_client.resolve()),
            'STEAM_COMPAT_DATA_PATH': str(args.proton_prefix.resolve()),
            'PROTON_LOG': '0', 'PROTON_LOG_DIR': str(final / 'logs'), 'SteamAppId': '0', 'SteamGameId': '0',
            'WINEDLLOVERRIDES': 'dxgi,d3d12,d3d12core=n',
            'LEGO_NATIVE_PM4_REFERENCE': '1' if args.renderer == 'pm4' else '0',
            'REXGLUE_TOYPAD_PORT': str(args.toypad_port),
        }
        if timer_wait == 'blocking':
            overrides['REX_TIMER_WAIT_BLOCKING'] = '1'
        if args.renderer == 'native':
            overrides.update({
            'LEGO_NATIVE_STATIC_TEXTURE_WATCH': '1', 'LEGO_NATIVE_BUFFER_WATCH': '1',
            'LEGO_NATIVE_TIMING': '1', 'LEGO_NATIVE_STENCIL': '1', 'LEGO_NATIVE_VIEWPORT': '1', 'LEGO_NATIVE_CULL': '1',
            'LEGO_NATIVE_FRAME_METRICS': windows(final / 'logs/frames.csv'),
            'LEGO_DUMP_MISSING_SHADERS': windows(final / 'shader-capture'),
            'LEGO_NATIVE_SHADER_PACK': windows(final / 'hotload/next.pack'),
            'LEGO_NATIVE_SHADER_PACK_TRIGGER': windows(final / 'hotload/next.trigger'),
            })
            if not performance:
                overrides.update({
                    'LEGO_NATIVE_TRACE_DIR': windows(final / 'logs/trace'),
                    'LEGO_NATIVE_DRAW_TRACE_TRIGGER': windows(final / 'logs/draw-trigger'),
                })
            if cadence:
                overrides.pop('LEGO_NATIVE_TIMING')
                overrides['LEGO_NATIVE_COMMAND_SLOTS'] = str(command_slots or 3)
            if gpu_timestamps:
                overrides['LEGO_NATIVE_GPU_TIMESTAMPS'] = windows(final / 'logs/gpu-timestamps.csv')
        if args.buffer_windows:
            overrides['LEGO_NATIVE_BUFFER_WINDOWS'] = '1'
        if args.snapshots:
            overrides['LEGO_GPU_SNAPSHOT_DIR'] = windows(final / 'captures')
        # LOAD packets include a writable private backing file, never a template
        # or the live campaign's persistent figure file.
        figures = []
        for name, (pad, index) in SLOTS.items():
            path = final / 'toypad-tags' / (name + '.bin')
            data = (stage / 'toypad-tags' / path.name).read_bytes()
            backing = windows(path).encode('utf-8')
            require(len(backing) <= 65535, 'Toypad backing path exceeds actual SDK protocol')
            packet = bytes((1, pad, index, 0, 0)) + data + struct.pack('<H', len(backing)) + backing
            (stage / 'toypad-tags' / (name + '.load-packet')).write_bytes(packet)
            figures.append({'name': name, 'pad': pad, 'index': index, 'path': str(path),
                            'backing_path': windows(path), **digest(stage / 'toypad-tags' / path.name),
                            'uid': bytes(data[i] for i in (0, 1, 2, 4, 5, 6, 7)).hex(),
                            'load_packet': str(final / 'toypad-tags' / (name + '.load-packet'))})
        plan = {
            'cwd': str(final), 'argv': ['nice', '-n', '10', str(args.proton.resolve()), 'runinprefix',
                                      str(final / 'legodimensions.exe')],
            'environment_overrides': overrides, 'remove_inherited_prefixes': list(SANITIZED_PREFIXES),
            'remove_inherited_keys': ['XENIA_TOYPAD_PORT', 'REXGLUE_TOYPAD_PORT', 'PROTON_LOG',
                                     'VKD3D_SHADER_CACHE_PATH', 'DXVK_STATE_CACHE_PATH'],
            'execution_implemented': False,
            'execution_gate': 'Not launched. An operator must refuse any active legodimensions.exe before executing this descriptor; the operator owns launch and desktop. The preparation command does not implement or execute this gate.',
        }
        manifest = {
            'version': 1, 'status': 'prepared-not-launched', 'name': args.name, 'prepared_unix': time.time(),
            'run_root': str(final), 'workspace': str(workspace), 'baseline': str(baseline), 'frozen_build': frozen,
            'dependency_proof': dependency_proof,
            'renderer_requested': args.renderer, 'renderer_verified_by_launch': False,
            'performance_probe': performance,
            'cadence_only_probe': cadence,
            'gpu_timestamp_probe': gpu_timestamps,
            'command_slots': (command_slots or 3) if cadence else None,
            'pm4_plugin': plugin_provenance,
            'checkpoint': str(checkpoint), 'checkpoint_files': pinned_saves,
            'save_relative_path': str(SAVE), 'supporting_content_seed': str(seed),
            'content_snapshot_limit': 'Each supporting content file is a COW byte snapshot, not a globally atomic guest save-state. Large content is not rehashed; small and pinned inputs are hashed.',
            'copies': [{**r, 'path': str(final / Path(r['path']).relative_to(stage))} for r in copier.records],
            'fallback_copy_bytes': copier.fallback_bytes, 'config_sha256': hashlib.sha256(config.encode()).hexdigest(),
            'source_config_sha256': config_identity['sha256'],
            'configured_asset_references': {key: value for key, value in tomllib.loads(config).items()
                                           if key in ('game_data_root', 'update_data_root', 'mods_root',
                                                      'mods_update_root', 'mods_game_root', 'mods_content_root')},
            'figures': figures, 'launch_plan': plan, 'recipe': RECIPE,
            'profile_xuid': args.xuid.upper(),
            'toypad_port': args.toypad_port,
            'timer_wait': timer_wait,
            'limitations': ['No process, socket or input action has occurred.', 'Recipe is not yet executed or scene-validated.',
                            'A shared existing Proton prefix is specified; launch requires exclusive game ownership.',
                            'Prepared tags preserve current legitimate bytes; no model, UID or template is reset.',
                            'SDK LOAD connection closure is not confirmation of placement; future logs/screenshots must verify figures.',
                            'Shader packs must be independently verified and explicitly published for this frozen build.',
                            'Game bytes remain a configured shared asset reference, mounted with allow_game_relative_writes=false. TU, mod-update, mods and DLC packages are private independent COW files.',
                            'Only EXE/source fingerprints are from the frozen baseline. Runtime, support files, configuration, tags and supporting content are pinned fresh at preparation.',
                            'Custom mods/modcli are disabled; mods_game_root is a private empty search directory. This recipe does not cover modded content.',
                            'The optional additive shader pack starts absent; verified packs require a separate explicit publish.',
                            'Report verifies a pristine prepared setup, not guest save files after a launch.'],
        }
        require(digest(source_config) == config_identity, 'Source configuration changed during preparation')
        write_json(stage / 'regression-manifest.json', manifest)
        stage.rename(final)
    except BaseException:
        # Preserve failed preparation for review instead of deleting evidence.
        if stage.exists():
            write_json(stage / 'preparation-failed.json', {'status': 'incomplete-do-not-launch', 'unix': time.time()})
        raise
    return verify(final, workspace)


def verify(run, workspace=None):
    workspace = (workspace or WORKSPACE).resolve(strict=True)
    run = owned(run, workspace)
    for name in ('logs', 'cache', 'metadata', 'hotload', 'captures',
                 'shader-capture', 'texture-capture', 'toypad-tags', 'no-mod-game',
                 'content', 'update', 'update-mods', 'mods'):
        directory = run / name
        owned(directory, run)
        require(directory.is_dir() and not directory.is_symlink(),
                f'Writable directory must be private and regular: {name}')
        for path in regular_tree(directory):
            require(path.stat().st_nlink == 1, 'Writable run file has a hardlink alias: '+str(path))
    manifest = read_json(run / 'regression-manifest.json')
    require(manifest['version'] == 1 and manifest['run_root'] == str(run), 'Manifest run identity mismatch')
    require(manifest['workspace'] == str(workspace), 'Workspace differs from prepared provenance')
    SAVE = save_path(manifest['profile_xuid'])
    require(manifest['save_relative_path'] == str(SAVE), 'Invalid private save/profile path')
    config_path = run / 'legodimensions.toml'
    require(digest(config_path)['sha256'] == manifest['config_sha256'], 'Prepared configuration changed')
    config = tomllib.loads(config_path.read_text())
    require(config['user_data_root'] == windows(run / 'content') and
            config['cache_root'] == windows(run / 'cache') and
            config['metadata_root'] == windows(run / 'metadata') and
            config['log_file'] == windows(run / 'logs/game.log') and
            config['mods_content_root'] == windows(run / 'content/0000000000000000' / TITLE / '00000002') and
            config['update_data_root'] == windows(run / 'update') and
            config['mods_update_root'] == windows(run / 'update-mods') and
            config['mods_root'] == windows(run / 'mods') and
            config['mods_game_root'] == windows(run / 'no-mod-game') and
            config['mods'] == '' and config['modcli_path'] == '' and
            config['allow_game_relative_writes'] is False and
            config['toypad_app_autostart'] is False and config['toypad_app_path'] == '',
            'Writable roots escaped the run')
    require(manifest['status'] == 'prepared-not-launched' and
            manifest['launch_plan']['execution_implemented'] is False,
            'This verifier only accepts a prepared, unlaunched setup')
    renderer = manifest['renderer_requested']
    require(renderer in ('native', 'pm4'), 'Unknown requested renderer')
    env = manifest['launch_plan']['environment_overrides']
    allowed = {'STEAM_COMPAT_CLIENT_INSTALL_PATH', 'STEAM_COMPAT_DATA_PATH', 'PROTON_LOG',
               'PROTON_LOG_DIR', 'SteamAppId', 'SteamGameId', 'WINEDLLOVERRIDES',
               'LEGO_NATIVE_PM4_REFERENCE', 'REXGLUE_TOYPAD_PORT'}
    if manifest['timer_wait'] == 'blocking':
        allowed.add('REX_TIMER_WAIT_BLOCKING')
        require(env.get('REX_TIMER_WAIT_BLOCKING') == '1', 'Blocking timer descriptor must explicitly set1')
    else:
        require(manifest['timer_wait'] == 'spin' and 'REX_TIMER_WAIT_BLOCKING' not in env,
                'Spin timer descriptor must omit the presence-based flag')
    if renderer == 'native':
        allowed.update({'LEGO_NATIVE_STATIC_TEXTURE_WATCH', 'LEGO_NATIVE_BUFFER_WATCH', 'LEGO_NATIVE_TIMING',
                        'LEGO_NATIVE_STENCIL', 'LEGO_NATIVE_VIEWPORT', 'LEGO_NATIVE_CULL',
                        'LEGO_NATIVE_FRAME_METRICS', 'LEGO_DUMP_MISSING_SHADERS', 'LEGO_NATIVE_SHADER_PACK',
                        'LEGO_NATIVE_GPU_TIMESTAMPS',
                        'LEGO_NATIVE_SHADER_PACK_TRIGGER', 'LEGO_NATIVE_BUFFER_WINDOWS',
                        'LEGO_NATIVE_COMMAND_SLOTS', 'LEGO_NATIVE_TRACE_DIR',
                        'LEGO_NATIVE_DRAW_TRACE_TRIGGER', 'LEGO_GPU_SNAPSHOT_DIR'})
    require(set(env) <= allowed and env['REXGLUE_TOYPAD_PORT'] == str(manifest['toypad_port']) and
            1024 <= manifest['toypad_port'] <= 65535 and manifest['toypad_port'] != 9191,
            'Unexpected environment override or private Toypad port')
    path_overrides = {'PROTON_LOG_DIR': run/'logs', 'LEGO_NATIVE_FRAME_METRICS': run/'logs/frames.csv',
                      'LEGO_NATIVE_GPU_TIMESTAMPS': run/'logs/gpu-timestamps.csv',
                      'LEGO_DUMP_MISSING_SHADERS': run/'shader-capture',
                      'LEGO_NATIVE_SHADER_PACK': run/'hotload/next.pack',
                      'LEGO_NATIVE_SHADER_PACK_TRIGGER': run/'hotload/next.trigger',
                      'LEGO_NATIVE_TRACE_DIR': run/'logs/trace',
                      'LEGO_NATIVE_DRAW_TRACE_TRIGGER': run/'logs/draw-trigger',
                      'LEGO_GPU_SNAPSHOT_DIR': run/'captures'}
    for key, target in path_overrides.items():
        if key in env:
            require(env[key] == (str(target) if key == 'PROTON_LOG_DIR' else windows(target)),
                    'Diagnostic output escaped the private run: '+key)
    timestamp_probe = manifest.get('gpu_timestamp_probe', False)
    require(type(timestamp_probe) is bool and
            ('LEGO_NATIVE_GPU_TIMESTAMPS' in env) is timestamp_probe and
            (not timestamp_probe or renderer == 'native'),
            'GPU timestamp descriptor disagrees with declared probe')
    if manifest.get('performance_probe'):
        require(renderer == 'native' and not any(key in env for key in (
            'LEGO_NATIVE_TRACE_DIR', 'LEGO_NATIVE_DRAW_TRACE_TRIGGER', 'LEGO_GPU_SNAPSHOT_DIR',
            'LEGO_NATIVE_MESH_TRACE', 'LEGO_NATIVE_AUDIT_BUFFER_WATCH', 'LEGO_NATIVE_AUDIT_TEXTURE_WATCH')),
            'Performance descriptor enabled heavy diagnostics')
    if manifest.get('cadence_only_probe'):
        require(renderer == 'native' and manifest.get('performance_probe') and
                'LEGO_NATIVE_TIMING' not in env and
                manifest.get('command_slots') in (3, 12) and
                env.get('LEGO_NATIVE_COMMAND_SLOTS') == str(manifest['command_slots']) and
                env.get('LEGO_NATIVE_FRAME_METRICS') == windows(run/'logs/frames.csv'),
                'Cadence descriptor timing/slot/metrics settings disagree')
    require(env['LEGO_NATIVE_PM4_REFERENCE'] == ('1' if renderer == 'pm4' else '0') and
            config['gpu_native_pm4'] is (renderer == 'pm4') and
            config['gpu_plugin'] == ('xenos' if renderer == 'pm4' else ''), 'Requested renderer route disagrees')
    require(manifest['launch_plan']['cwd'] == str(run) and
            manifest['launch_plan']['argv'][-1] == str(run / 'legodimensions.exe'),
            'Launch descriptor escaped frozen executable')
    if renderer == 'pm4':
        expected_flags = {'LEGO_NATIVE_PM4_REFERENCE'}
        if manifest['timer_wait'] == 'blocking': expected_flags.add('REX_TIMER_WAIT_BLOCKING')
        require(set(key for key in env if key.startswith(SANITIZED_PREFIXES)) == expected_flags,
                'PM4 descriptor inherits native experimental flags')
        require(digest(run / 'rexgpu-xenos.dll')['sha256'] == manifest['pm4_plugin']['sha256'] and
                digest(run / 'rexruntime.dll')['sha256'] == manifest['pm4_plugin']['matching_runtime_sha256'],
                'PM4 plugin/runtime identity changed')
    for record in manifest['copies']:
        path = owned(Path(record['path']), run)
        require(path.is_file() and not path.is_symlink() and path.stat().st_size == record['bytes'],
                f'Prepared file missing/truncated/aliased: {path}')
        if 'sha256' in record:
            require(digest(path)['sha256'] == record['sha256'], f'Prepared pinned bytes changed: {path}')
        source = Path(record['source'])
        if source.exists():
            require((source.stat().st_dev, source.stat().st_ino) != (path.stat().st_dev, path.stat().st_ino),
                    f'Prepared mutable file hardlinked to source: {path}')
    for name, expected in manifest['checkpoint_files'].items():
        require(digest(run / 'content' / SAVE / name) == expected, f'Checkpoint changed: {name}')
    for figure in manifest['figures']:
        require(figure['name'] in SLOTS and tuple((figure['pad'], figure['index'])) == SLOTS[figure['name']],
                'Figure placement differs from the recipe')
        require(figure['path'] == str(run / 'toypad-tags' / (figure['name'] + '.bin')) and
                figure['backing_path'] == windows(Path(figure['path'])) and
                figure['load_packet'] == str(run / 'toypad-tags' / (figure['name'] + '.load-packet')),
                'Toypad path escaped private backing files')
        data = (run / 'toypad-tags' / (figure['name'] + '.bin')).read_bytes()
        packet_path = owned(Path(figure['load_packet']), run)
        require(not packet_path.is_symlink(), 'Toypad packet is a symlink')
        packet = packet_path.read_bytes()
        backing = figure['backing_path'].encode()
        require(packet == bytes((1, figure['pad'], figure['index'], 0, 0)) + data +
                struct.pack('<H', len(backing)) + backing, 'Toypad packet/backing identity mismatch')
    require({f['name'] for f in manifest['figures']} == set(SLOTS), 'Missing normal persistent figures')
    require(digest(run / 'legodimensions.exe')['sha256'] == manifest['frozen_build']['executable_sha256'],
            'Prepared EXE no longer matches frozen identity')
    report = {
        'status': 'prepared-identity-verified-not-launched', 'run_root': str(run),
        'build_fingerprint': manifest['frozen_build']['build_fingerprint'],
        'renderer_requested': renderer, 'renderer_verified_by_launch': False,
        'route_authority': 'A future actual GPU route log decides renderer identity; embedded native fingerprint alone does not.',
        'executable_sha256': digest(run / 'legodimensions.exe')['sha256'],
        'checkpoint_files_verified': len(manifest['checkpoint_files']),
        'independent_content_files': sum('/content/' in r['path'] for r in manifest['copies']),
        'reflink_files': sum(r['method'] == 'reflink' for r in manifest['copies']),
        'fallback_copy_bytes': manifest['fallback_copy_bytes'],
        'toypad_port': manifest['launch_plan']['environment_overrides']['REXGLUE_TOYPAD_PORT'],
        'recipe_executed': False, 'rendering_verified': False, 'launch_plan': manifest['launch_plan'],
        'limits': manifest['limitations'],
    }
    write_json(run / 'preparation-verification.json', report)
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='mode', required=True)
    p = sub.add_parser('prepare')
    p.add_argument('name')
    p.add_argument('--workspace', type=Path, required=True)
    p.add_argument('--baseline', type=Path, required=True)
    p.add_argument('--checkpoint', type=Path, required=True)
    p.add_argument('--xuid', required=True, help='Legitimate sixteen-hex-digit profile XUID')
    p.add_argument('--install', type=Path, required=True)
    p.add_argument('--tags', type=Path, required=True)
    p.add_argument('--runs', type=Path, required=True)
    p.add_argument('--proton', type=Path, required=True)
    p.add_argument('--proton-prefix', type=Path, required=True)
    p.add_argument('--steam-client', type=Path, required=True)
    p.add_argument('--toypad-port', type=int, required=True)
    p.add_argument('--timer-wait', choices=('spin', 'blocking'), default='spin',
                   help='Restart-only SDK experiment; spin omits flag, blocking explicitly sets1')
    p.add_argument('--buffer-windows', action='store_true')
    p.add_argument('--performance', action='store_true',
                   help='Native timing CSV probe without LongProbe/draw triggers or snapshots')
    p.add_argument('--cadence-only', action='store_true',
                   help='Native frame CSV with detailed timing flag absent; implies performance mode')
    p.add_argument('--command-slots', type=int, choices=(3, 12),
                   help='Explicit private cadence experiment slot count (default3)')
    p.add_argument('--gpu-timestamps', action='store_true',
                   help='Opt-in native per-submission queue timestamps in a separate private CSV')
    p.add_argument('--snapshots', action='store_true',
                   help='Explicitly enable bounded native DDS readbacks; changes timing workload')
    p.add_argument('--renderer', choices=('native', 'pm4'), default='native')
    p.add_argument('--render-target-path-d3d12', choices=('rov', 'rtv'),
                   help='Explicit PM4 render-target backend experiment; restart required')
    p.add_argument('--pm4-plugin', type=Path)
    p.add_argument('--pm4-plugin-sha256')
    p.add_argument('--pm4-runtime-sha256')
    p.add_argument('--pm4-compatibility-proof', type=Path)
    p = sub.add_parser('report')
    p.add_argument('run', type=Path)
    p.add_argument('--workspace', type=Path, required=True)
    args = parser.parse_args()
    report = prepare(args) if args.mode == 'prepare' else verify(args.run, args.workspace)
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    try:
        main()
    except (ValueError, OSError, KeyError, json.JSONDecodeError) as error:
        raise SystemExit(f'Regression preparation rejected: {error}')
