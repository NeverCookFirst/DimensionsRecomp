#!/usr/bin/env python3
"""Asset-free filesystem and static-PE tests of the actual preparation helper.

Synthetic PE files exercise inspection only; they are never executed and do
not simulate a game or renderer. Real COW isolation is exercised when supported.
"""
import argparse
import csv
import errno
import importlib.util
import json
import os
from pathlib import Path
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import sample_cadence as sampler
import compare_samples as comparer
spec = importlib.util.spec_from_file_location('native_regression', HERE / 'prepare_run.py')
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)
runner_spec = importlib.util.spec_from_file_location('regression_runner', HERE / 'execute_run.py')
runner = importlib.util.module_from_spec(runner_spec)
runner_spec.loader.exec_module(runner)


def synthetic_pe(imports, exports=()):
    image = bytearray(65536)
    image[:2] = b'MZ'
    struct.pack_into('<I', image, 0x3c, 0x80)
    image[0x80:0x84] = b'PE\0\0'
    struct.pack_into('<HH', image, 0x84, 0x8664, 1)
    struct.pack_into('<H', image, 0x94, 240)
    optional = 0x98
    struct.pack_into('<H', image, optional, 0x20b)
    struct.pack_into('<I', image, optional+108, 16)
    section = optional+240
    struct.pack_into('<IIII', image, section+8, 65024, 0x1000, 65024, 512)
    cursor = 0x1200
    def alloc(data):
        nonlocal cursor
        rva = cursor
        at = 512+cursor-0x1000
        image[at:at+len(data)] = data
        cursor += len(data)
        return rva
    def put(rva, fmt, *values):
        struct.pack_into(fmt, image, 512+rva-0x1000, *values)
    descriptors = alloc(bytes(20*(len(imports)+1)))
    struct.pack_into('<II', image, optional+120, descriptors, 20*(len(imports)+1))
    for index, (dll, symbols) in enumerate(imports.items()):
        name = alloc(dll.encode()+b'\0')
        symbol_rvas = [alloc(b'\0\0'+symbol.encode()+b'\0') for symbol in symbols]
        lookup = alloc(struct.pack('<'+'Q'*(len(symbol_rvas)+1), *symbol_rvas, 0))
        put(descriptors+20*index, '<IIIII', lookup, 0, 0, name, lookup)
    if exports:
        table = alloc(bytes(40))
        names = alloc(struct.pack('<'+'I'*len(exports), *(alloc(name.encode()+b'\0') for name in exports)))
        put(table+24, '<I', len(exports))
        put(table+32, '<I', names)
        struct.pack_into('<II', image, optional+112, table, 40)
    return bytes(image)


class PreparationTest(unittest.TestCase):
    def setUp(self):
        self.root = Path(tempfile.mkdtemp(prefix='renderer-regression-test-'))
        self.workspace_pin = patch.object(m, 'WORKSPACE', self.root)
        self.workspace_pin.start()
        self.save = m.save_path('1234567890ABCDEF')
        self.install = self.root / 'install'
        self.baseline = self.root / 'baseline'
        self.checkpoint = self.root / 'checkpoint'
        self.tags = self.root / 'tags'
        for directory in (self.install, self.baseline, self.checkpoint / 'savegame_1', self.tags):
            directory.mkdir(parents=True)
        for name in m.BINARY_NAMES+m.SUPPORT_NAMES:
            (self.install / name).write_bytes(('fixture-'+name).encode())
        for name in ('legodimensions.exe', 'dxcompiler.dll', 'dxil.dll'):
            (self.install/name).write_bytes(synthetic_pe({'KERNEL32.dll': ['FixtureKernel'], **({'rexruntime.dll':['FixtureRuntime']} if name=='legodimensions.exe' else {})}))
        (self.install / 'rexruntime.dll').write_bytes(synthetic_pe({'KERNEL32.dll': ['FixtureKernel']}, ['FixtureRuntime']))
        (self.baseline / 'legodimensions.exe').write_bytes((self.install / 'legodimensions.exe').read_bytes())
        for directory in ('update', 'update-mods', 'mods'):
            (self.install / directory).mkdir()
        (self.install / 'update/PATCH3.DAT').write_bytes(b'legitimate-update-bytes')
        (self.install / 'update/PATCH3.DAT.off').write_bytes(b'old-private-fern-state')
        values = {'game_data_root': m.windows(self.install / 'game'),
                  'update_data_root': m.windows(self.install / 'update'),
                  'mods_update_root': m.windows(self.install / 'update-mods'),
                  'mods_root': m.windows(self.install / 'mods'), 'mods': '', 'modcli_path': '',
                  'vsync': False, 'fern': False}
        (self.install / 'legodimensions.toml').write_text(m.config_text('', values))
        self.plugin = self.root / 'rexgpu-xenos.dll'
        self.plugin.write_bytes(synthetic_pe({'rexruntime.dll': ['FixtureRuntime'], 'KERNEL32.dll': ['FixtureKernel']}))
        graphics = self.root / 'graphics'
        graphics.mkdir()
        for name in ('dxgi.dll', 'd3d12.dll', 'd3d12core.dll'):
            (graphics / name).write_bytes(name.encode())
        binaries = {name: {'size': (self.install/name).stat().st_size,
                           'sha256': m.digest(self.install/name)['sha256']} for name in m.BINARY_NAMES}
        self.staging = {'binaries': binaries, 'graphics': {p.name: str(p) for p in graphics.iterdir()},
                        'imports': {name: list(m.pe_tables(self.install/name)['imports']) for name in m.BINARY_NAMES}}
        (self.install / 'native-staging-manifest.json').write_text(json.dumps(self.staging))
        (self.baseline / 'baseline-manifest.json').write_text(json.dumps({
            'build_fingerprint': '0123456789abcdef', 'executable_sha256': binaries['legodimensions.exe']['sha256'],
            'frozen_dependencies': {'rexruntime.dll':{'sha256':binaries['rexruntime.dll']['sha256']}},
            'source_contains_uncommitted_changes': True}))
        savedir = self.install / 'content' / self.save
        savedir.mkdir(parents=True)
        (savedir / 'GAME01').write_bytes(b'live-save-must-not-be-replayed')
        extras = {'install/seed.bin': b'mandatory-install-already-finished',
                  'profile/settings': b'legitimate-profile',
                  '0000000000000000/5752084B/00000002/DLC01/package': b'dlc-bytes'}
        for relative, data in extras.items():
            target = self.install / 'content' / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
        metadata = {}
        for name in ('GAME01', 'V2GAME01', 'OPTS01', 'DLC01'):
            target = self.checkpoint / 'savegame_1' / name
            target.write_bytes(('frozen-legitimate-'+name).encode())
            metadata[name] = {**m.digest(target), 'mtime': 1234}
        (self.checkpoint / 'save-metadata.json').write_text(json.dumps(metadata))
        for index, name in enumerate(m.SLOTS):
            (self.tags / (name+'.bin')).write_bytes(bytes((index+13,))*180)
        self.plugin_pin = m.digest(self.plugin)['sha256']
        self.runtime_pin = binaries['rexruntime.dll']['sha256']
        self.compatibility = self.root/'compatibility.json'
        self.compatibility.write_text(json.dumps({'plugin_sha256':self.plugin_pin,'runtime_sha256':self.runtime_pin,'evidence':'Synthetic fixtures exercise static validation only; no actual compatibility claim.'}))
        (self.root/'never-executed-proton').write_bytes(b'not executed')
        (self.root/'prefix').mkdir()
        (self.root/'steam-client').mkdir()

    def tearDown(self):
        self.workspace_pin.stop()
        shutil.rmtree(self.root)

    def args(self, name='native', **changes):
        values = dict(name=name, runs=self.root/'runs', baseline=self.baseline, install=self.install,
                      checkpoint=self.checkpoint, tags=self.tags, toypad_port=19201,
                      proton=self.root/'never-executed-proton', buffer_windows=False,
                      snapshots=False, renderer='native', pm4_plugin=self.plugin,
                      workspace=self.root, xuid='1234567890ABCDEF',
                      proton_prefix=self.root/'prefix',steam_client=self.root/'steam-client',
                      pm4_plugin_sha256=m.digest(self.plugin)['sha256'],pm4_runtime_sha256=self.runtime_pin,
                      pm4_compatibility_proof=self.compatibility)
        values.update(changes)
        return argparse.Namespace(**values)

    def test_cadence_descriptors_preserve_frozen_bytes_and_absent_timing(self):
        identities = []
        for slots in (3, 12):
            report = m.prepare(self.args(name='cadence'+str(slots), cadence_only=True,
                                        command_slots=slots, buffer_windows=True))
            run = Path(report['run_root'])
            manifest = json.loads((run/'regression-manifest.json').read_text())
            env = manifest['launch_plan']['environment_overrides']
            identities.append((m.digest(run/'legodimensions.exe'), m.digest(run/'rexruntime.dll')))
            self.assertEqual(env['LEGO_NATIVE_COMMAND_SLOTS'], str(slots))
            self.assertNotIn('LEGO_NATIVE_TIMING', env)
            self.assertNotIn('LEGO_NATIVE_TRACE_DIR', env)
            self.assertNotIn('LEGO_NATIVE_DRAW_TRACE_TRIGGER', env)
            self.assertTrue(manifest['cadence_only_probe'])
            self.assertEqual(env['LEGO_NATIVE_FRAME_METRICS'], m.windows(run/'logs/frames.csv'))
            m.verify(run)
            # Timing uses environment presence, so even value"0" must reject.
            env['LEGO_NATIVE_TIMING'] = '0'
            (run/'regression-manifest.json').write_text(json.dumps(manifest))
            with self.assertRaisesRegex(Exception, 'Cadence descriptor'):
                m.verify(run)
            del env['LEGO_NATIVE_TIMING']
            env['LEGO_NATIVE_COMMAND_SLOTS'] = '12' if slots==3 else '3'
            (run/'regression-manifest.json').write_text(json.dumps(manifest))
            with self.assertRaisesRegex(Exception, 'Cadence descriptor'):
                m.verify(run)
        self.assertEqual(identities[0], identities[1])
        for changes in ({'renderer':'pm4'}, {'snapshots':True}, {'command_slots':4}):
            with self.assertRaises(Exception):
                m.prepare(self.args(name='invalid-cadence', cadence_only=True, **changes))

    def test_performance_preparation_omits_heavy_diagnostics_and_rejects_tampering(self):
        report = m.prepare(self.args(name='performance', performance=True, buffer_windows=True))
        run = Path(report['run_root'])
        manifest = json.loads((run/'regression-manifest.json').read_text())
        env = manifest['launch_plan']['environment_overrides']
        self.assertTrue(manifest['performance_probe'])
        for flag in ('LEGO_NATIVE_TIMING', 'LEGO_NATIVE_BUFFER_WINDOWS', 'LEGO_NATIVE_STATIC_TEXTURE_WATCH',
                     'LEGO_NATIVE_BUFFER_WATCH', 'LEGO_NATIVE_STENCIL', 'LEGO_NATIVE_VIEWPORT', 'LEGO_NATIVE_CULL'):
            self.assertEqual(env[flag], '1')
        self.assertIn('LEGO_DUMP_MISSING_SHADERS', env)
        self.assertIn('LEGO_NATIVE_FRAME_METRICS', env)
        self.assertNotIn('LEGO_NATIVE_TRACE_DIR', env)
        self.assertNotIn('LEGO_NATIVE_DRAW_TRACE_TRIGGER', env)
        m.verify(run)
        for flag in ('LEGO_NATIVE_TRACE_DIR', 'LEGO_NATIVE_DRAW_TRACE_TRIGGER', 'LEGO_NATIVE_AUDIT_BUFFER_WATCH',
                     'LEGO_NATIVE_AUDIT_TEXTURE_WATCH', 'LEGO_GPU_SNAPSHOT_DIR'):
            env[flag] = m.windows(run/'logs/unexpected')
            (run/'regression-manifest.json').write_text(json.dumps(manifest))
            with self.assertRaises(ValueError):
                m.verify(run)
            del env[flag]
        for changes in ({'renderer':'pm4'}, {'snapshots':True}):
            with self.assertRaisesRegex(Exception, 'without snapshots'):
                m.prepare(self.args(name='rejected-performance', performance=True, **changes))

    def test_actual_preparer_preserves_sources_and_routes_all_writable_paths(self):
        source_hashes = {p: m.digest(p) for p in self.root.rglob('*') if p.is_file()}
        with patch.object(subprocess, 'Popen', side_effect=AssertionError('process launched')), \
             patch.object(socket, 'socket', side_effect=AssertionError('socket opened')), \
             patch.object(os, 'system', side_effect=AssertionError('shell launched')):
            report = m.prepare(self.args())
        self.assertFalse(report['rendering_verified'])
        self.assertFalse(report['renderer_verified_by_launch'])
        run = Path(report['run_root'])
        self.assertEqual((run/'content'/self.save/'GAME01').read_bytes(), b'frozen-legitimate-GAME01')
        for path, identity in source_hashes.items():
            self.assertEqual(m.digest(path), identity)
        config = m.tomllib.loads((run/'legodimensions.toml').read_text())
        for key, suffix in (('update_data_root', 'update'), ('mods_update_root', 'update-mods'),
                            ('mods_root', 'mods'), ('mods_game_root', 'no-mod-game'),
                            ('user_data_root', 'content')):
            self.assertEqual(config[key], m.windows(run/suffix))
        self.assertFalse(config['allow_game_relative_writes'])
        self.assertFalse(config['toypad_app_autostart'])
        self.assertEqual(config['modcli_path'], '')
        # Actual independent files: private tag/update/save edits cannot affect sources.
        for target in (run/'update/PATCH3.DAT', run/'content'/self.save/'GAME01', run/'toypad-tags/batman.bin'):
            target.write_bytes(b'private mutation')
        for path, identity in source_hashes.items():
            self.assertEqual(m.digest(path), identity)
        with self.assertRaises(ValueError):
            m.verify(run)

    def test_pm4_twin_has_identical_seed_and_tags_but_explicit_route(self):
        first = Path(m.prepare(self.args())['run_root'])
        second = Path(m.prepare(self.args('pm4', renderer='pm4', install=first,
                                        tags=first/'toypad-tags', toypad_port=19202))['run_root'])
        self.assertEqual({p.relative_to(first/'content'): m.digest(p) for p in (first/'content').rglob('*') if p.is_file()},
                         {p.relative_to(second/'content'): m.digest(p) for p in (second/'content').rglob('*') if p.is_file()})
        for name in m.SLOTS:
            self.assertEqual(m.digest(first/'toypad-tags'/(name+'.bin')), m.digest(second/'toypad-tags'/(name+'.bin')))
        manifest = json.loads((second/'regression-manifest.json').read_text())
        env = manifest['launch_plan']['environment_overrides']
        self.assertEqual({key for key in env if key.startswith(m.SANITIZED_PREFIXES)}, {'LEGO_NATIVE_PM4_REFERENCE'})
        self.assertEqual(env['LEGO_NATIVE_PM4_REFERENCE'], '1')
        self.assertEqual(manifest['pm4_plugin']['runtime_import_symbols_verified'], 1)
        self.assertFalse(manifest['renderer_verified_by_launch'])
        self.assertNotIn('run-native-game.sh', manifest['launch_plan']['argv'])

    def test_private_packets_match_actual_sdk_wire_contract(self):
        run = Path(m.prepare(self.args())['run_root'])
        for name, expected in m.SLOTS.items():
            packet = (run/'toypad-tags'/(name+'.load-packet')).read_bytes()
            cmd, pad, index, old_pad, old_index = struct.unpack('<5B', packet[:5])
            self.assertEqual((cmd, pad, index, old_pad, old_index), (1, *expected, 0, 0))
            length, = struct.unpack('<H', packet[185:187])
            self.assertEqual(len(packet)-187, length)
            self.assertEqual(packet[5:185], (self.tags/(name+'.bin')).read_bytes())
            self.assertEqual(packet[187:].decode(), m.windows(run/'toypad-tags'/(name+'.bin')))

    def test_clone_fallback_is_bounded_and_failed_large_file_removed(self):
        small = self.root/'small'
        small.write_bytes(b'safe-small-data')
        copier = m.Copier()
        error = OSError(errno.EOPNOTSUPP, 'test unsupported reflink')
        with patch.object(m.fcntl, 'ioctl', side_effect=error):
            self.assertEqual(copier.copy(small, self.root/'copy')['method'], 'bounded_copy')
            big = self.root/'large'
            with big.open('wb') as output:
                output.truncate(1024*1024+1)
            with self.assertRaisesRegex(ValueError, 'requires COW'):
                copier.copy(big, self.root/'failed-large')
        self.assertFalse((self.root/'failed-large').exists())
        self.assertEqual(copier.fallback_bytes, len(b'safe-small-data'))

    def test_private_source_tree_symlink_and_output_escape_rejected(self):
        (self.install/'content/live-link').symlink_to(self.tags/'batman.bin')
        with self.assertRaisesRegex(ValueError, 'may not alias'):
            m.prepare(self.args())
        with self.assertRaises(ValueError):
            m.owned(Path('/tmp/native-regression-output'))
        redirected = self.root/'redirect'
        redirected.symlink_to(self.root/'install', target_is_directory=True)
        with self.assertRaises(ValueError):
            m.owned(redirected/'new-run')

    def test_corrupt_frozen_executable_and_checkpoint_rejected_before_output(self):
        (self.baseline/'legodimensions.exe').write_bytes(b'wrong binary')
        with self.assertRaisesRegex(ValueError, 'Frozen EXE'):
            m.prepare(self.args())
        (self.baseline/'legodimensions.exe').write_bytes((self.install/'legodimensions.exe').read_bytes())
        (self.checkpoint/'savegame_1/GAME01').write_bytes(b'wrong save')
        with self.assertRaisesRegex(ValueError, 'legitimate snapshot'):
            m.prepare(self.args())
        self.assertFalse((self.root/'runs/native').exists())

    def test_missing_pin_file_set_and_enabled_mods_rejected(self):
        metadata_path = self.checkpoint/'save-metadata.json'
        metadata = json.loads(metadata_path.read_text())
        metadata.pop('DLC01')
        metadata_path.write_text(json.dumps(metadata))
        with self.assertRaisesRegex(ValueError, 'file set'):
            m.prepare(self.args())
        config = self.install/'legodimensions.toml'
        config.write_text(config.read_text().replace('mods = ""', 'mods = "custom"'))
        with self.assertRaisesRegex(ValueError, 'no-custom-mod'):
            m.prepare(self.args())

    def test_no_overwrite_hardlink_or_descriptor_escape(self):
        run = Path(m.prepare(self.args())['run_root'])
        with self.assertRaisesRegex(ValueError, 'Refuse overwrite'):
            m.prepare(self.args())
        tag = run/'toypad-tags/batman.bin'
        tag.unlink()
        os.link(self.tags/'batman.bin', tag)
        with self.assertRaisesRegex(ValueError, 'hardlink'):
            m.verify(run)
        tag.unlink()
        shutil.copyfile(self.tags/'batman.bin', tag)
        manifest_path = run/'regression-manifest.json'
        manifest = json.loads(manifest_path.read_text())
        manifest['figures'][0]['backing_path'] = m.windows(self.tags/'batman.bin')
        manifest_path.write_text(json.dumps(manifest))
        with self.assertRaisesRegex(ValueError, 'escaped private'):
            m.verify(run)

    def test_native_flags_rejected_for_pm4_and_default_port_rejected(self):
        with self.assertRaisesRegex(ValueError, 'Native diagnostic'):
            m.prepare(self.args(renderer='pm4', buffer_windows=True))
        with self.assertRaisesRegex(ValueError, 'distinct private'):
            m.prepare(self.args(toypad_port=9191))

    def test_static_pe_rejects_wrong_machine_missing_symbol_and_unknown_dependency(self):
        image = bytearray(self.plugin.read_bytes())
        struct.pack_into('<H', image, 0x84, 0x14c)
        bad = self.root/'bad.dll'
        bad.write_bytes(image)
        with self.assertRaisesRegex(ValueError, 'x64 PE32'):
            m.pe_tables(bad)
        self.plugin.write_bytes(synthetic_pe({'rexruntime.dll': ['MissingRuntimeSymbol']}))
        with patch.object(m, 'require_sha', wraps=m.require_sha):
            proof=json.loads(self.compatibility.read_text());proof['plugin_sha256']=m.digest(self.plugin)['sha256'];self.compatibility.write_text(json.dumps(proof))
            with self.assertRaisesRegex(ValueError, 'missing plugin import'):
                m.prepare(self.args(renderer='pm4'))
        self.plugin.write_bytes(synthetic_pe({'rexruntime.dll': ['FixtureRuntime'], 'unknown.dll': ['Function']}))
        with patch.object(m, 'require_sha', wraps=m.require_sha):
            proof=json.loads(self.compatibility.read_text());proof['plugin_sha256']=m.digest(self.plugin)['sha256'];self.compatibility.write_text(json.dumps(proof))
            with self.assertRaisesRegex(ValueError, 'unstaged/unreviewed'):
                m.prepare(self.args(renderer='pm4'))

    def test_empty_writable_directory_symlinks_rejected_before_launch(self):
        # These roots contain no pinned copy records, so file-only identity
        # verification previously accepted redirection of subsequent writes.
        names = ('logs', 'cache', 'metadata', 'hotload', 'captures',
                 'shader-capture', 'texture-capture', 'no-mod-game')
        outside = self.root / 'live-writable-directory'
        outside.mkdir()
        sentinel = outside / 'sentinel'
        sentinel.write_bytes(b'live data must remain untouched')
        for index, name in enumerate(names):
            with self.subTest(directory=name):
                run = Path(m.prepare(self.args('bad-dir-'+str(index)))['run_root'])
                target = run / name
                self.assertEqual(list(target.iterdir()), [])
                target.rmdir()
                target.symlink_to(outside, target_is_directory=True)
                with self.assertRaises(ValueError):
                    m.verify(run)
                with patch.object(sys, 'argv', ['execute_run.py', str(run), '--workspace', str(self.root), '--launch']), \
                     patch.object(runner, 'active_games', return_value=[]), \
                     patch.object(runner.subprocess, 'Popen',
                                  side_effect=AssertionError('redirected run launched')) as launch:
                    with self.assertRaises(ValueError):
                        runner.main()
                launch.assert_not_called()
                self.assertEqual(list(outside.iterdir()), [sentinel])
                self.assertEqual(sentinel.read_bytes(), b'live data must remain untouched')

    def test_executor_sanitizes_sdk_cvars_even_for_an_old_descriptor(self):
        run = Path(m.prepare(self.args())['run_root'])
        plan = json.loads((run / 'regression-manifest.json').read_text())['launch_plan']
        # Simulate an already prepared descriptor from before the broad REX_
        # sanitizer. Runner-side protection must still override ambient values.
        plan['remove_inherited_prefixes'] = ['LEGO_NATIVE_', 'LEGO_GPU_', 'LEGO_DUMP_', 'REX_TIMER_']
        ambient = {
            'REX_LOG_FILE': 'Z:\\live\\game.log',
            'REX_USER_DATA_ROOT': 'Z:\\live\\save-data',
            'REX_RENDER_TARGET_PATH_D3D12': 'rov',
            'REX_GPU_PLUGIN': 'unexpected-plugin',
            'REX_TIMER_WAIT_BLOCKING': '1',
            'LEGO_NATIVE_PM4_REFERENCE': '1',
            'REXGLUE_TOYPAD_PORT': '9191',
            'PATH': '/fixture/path',
        }
        with patch.dict(os.environ, ambient, clear=True):
            actual = runner.environment(plan)
        for key in ambient:
            if key.startswith('REX_'):
                self.assertNotIn(key, actual)
        self.assertEqual(actual['LEGO_NATIVE_PM4_REFERENCE'], '0')
        self.assertEqual(actual['REXGLUE_TOYPAD_PORT'], '19201')
        self.assertEqual(actual['PATH'], '/fixture/path')

    def test_executor_intrinsic_sanitizers_survive_an_empty_descriptor_policy(self):
        plan={'remove_inherited_prefixes':[],'remove_inherited_keys':[],
              'environment_overrides':{'LEGO_NATIVE_PM4_REFERENCE':'0','REXGLUE_TOYPAD_PORT':'19207'}}
        ambient={'LEGO_NATIVE_AUDIT_BUFFER_WATCH':'1','LEGO_NATIVE_TRACE_DIR':'live-trace',
                 'LEGO_GPU_SNAPSHOT_DIR':'live-readback','LEGO_DUMP_MISSING_SHADERS':'live-capture',
                 'REX_TIMER_WAIT_BLOCKING':'0','REX_USER_DATA_ROOT':'live-save',
                 'XENIA_TOYPAD_PORT':'9191','REXGLUE_TOYPAD_PORT':'9191','PROTON_LOG':'1',
                 'VKD3D_SHADER_CACHE_PATH':'live-vkd3d','DXVK_STATE_CACHE_PATH':'live-dxvk','PATH':'ordinary-path'}
        with patch.dict(os.environ,ambient,clear=True):actual=runner.environment(plan)
        self.assertEqual(actual,{'PATH':'ordinary-path','LEGO_NATIVE_PM4_REFERENCE':'0',
                                 'REXGLUE_TOYPAD_PORT':'19207'})

    def test_preparer_requests_broad_sdk_cvar_sanitization(self):
        run = Path(m.prepare(self.args())['run_root'])
        prefixes = tuple(json.loads((run / 'regression-manifest.json').read_text())
                         ['launch_plan']['remove_inherited_prefixes'])
        for key in ('REX_LOG_FILE', 'REX_USER_DATA_ROOT', 'REX_RENDER_TARGET_PATH_D3D12'):
            self.assertTrue(key.startswith(prefixes), key)
        self.assertFalse('REXGLUE_TOYPAD_PORT'.startswith(prefixes))

    def test_rtv_is_explicit_and_preserved_only_for_pm4(self):
        run = Path(m.prepare(self.args('pm4-rtv', renderer='pm4',
                                       render_target_path_d3d12='rtv'))['run_root'])
        config = m.tomllib.loads((run / 'legodimensions.toml').read_text())
        self.assertEqual(config['render_target_path_d3d12'], 'rtv')
        self.assertTrue(config['gpu_native_pm4'])
        self.assertEqual(config['gpu_plugin'], 'xenos')
        report = m.verify(run)
        self.assertEqual(report['renderer_requested'], 'pm4')
        self.assertEqual(report['launch_plan']['environment_overrides']['LEGO_NATIVE_PM4_REFERENCE'], '1')
        self.assertFalse(report['renderer_verified_by_launch'])
        with patch.dict(os.environ, {'REX_RENDER_TARGET_PATH_D3D12': 'rov'}, clear=True):
            self.assertNotIn('REX_RENDER_TARGET_PATH_D3D12', runner.environment(report['launch_plan']))
        with self.assertRaises(ValueError):
            m.prepare(self.args('native-rtv', render_target_path_d3d12='rtv'))
        self.assertFalse((self.root / 'runs/native-rtv').exists())

    def test_runner_refuses_invalid_environment_without_spawning(self):
        plan = {'remove_inherited_prefixes': [], 'remove_inherited_keys': [],
                'environment_overrides': {'REXGLUE_TOYPAD_PORT': '19201\0extra'}}
        with self.assertRaisesRegex(ValueError, 'environment value'):
            runner.environment(plan)

    def live_fixture(self, name='sample', slots=3):
        run = Path(m.prepare(self.args(name, cadence_only=True, command_slots=slots))['run_root'])
        manifest = json.loads((run/'regression-manifest.json').read_text())
        execution = {'exit_code': None, 'launch_requested': True, 'route': '0',
                     'environment_overrides': manifest['launch_plan']['environment_overrides']}
        (run/'logs/execution.json').write_text(json.dumps(execution))
        (run/'logs/game.log').write_text('GPU route: detached native D3D12 renderer\n'
                                       f'Native GPU init: creating {slots} command slots\n')
        capture = run/'captures/operator-confirmation.bin'
        capture.write_bytes(b'fixture evidence; not actual renderer pixels')
        scene = {'run_root':str(run), 'build_fingerprint':'0123456789abcdef', 'renderer':'native',
                 'scene_id':'synthetic-stationary-scene', 'camera_id':'untouched-camera',
                 'stationary':True, 'camera_unchanged':True, 'inputs_released':True, 'workers_held':True,
                 'capture_path':str(capture), 'capture_sha256':m.digest(capture)['sha256'],
                 'figures':[{k:f[k] for k in ('name','pad','index','uid')}
                            for f in manifest['figures'] if f['name']!='batmobile']}
        scene_file = run/'scene.json'
        scene_file.write_text(json.dumps(scene))
        return run, scene_file

    def sample_fixture(self, name='sample', slots=3, event=None):
        run, scene = self.live_fixture(name, slots)
        ticks = [0.0]
        calls = [0]
        points = (60, 120, 120, 720)
        def sleep(seconds): ticks[0] += seconds
        def endpoint(_):
            index = calls[0]; calls[0] += 1
            if index == 3 and event:
                with (run/'logs/game.log').open('a') as target:target.write(event+'\n')
            rows = [{'frame':float(i), 'interval_ms':50.0, 'draw_calls':500.0,
                     'detailed_timing_enabled':0.0, 'frame_slot_wait_calls':8.0,
                     'callbacks_enqueued':42.0,'callbacks_executed':42.0,'callbacks_pending':8.0}
                    for i in range(1, points[index]+1)]
            return {'frame':points[index], 'log_bytes':(run/'logs/game.log').stat().st_size,
                    'monotonic':ticks[0], 'unix':ticks[0], 'csv_bytes':0,
                    'observed_workers':[]}, rows
        output = self.root/(name+'-out')
        report = sampler.sample(run, output, scene, self.root, take_endpoint=endpoint,
                                sleep=sleep, clock=lambda:ticks[0])
        return output, report

    def test_sampler_records_fixed_interval_and_preserves_all_rejected_windows(self):
        output, report = self.sample_fixture()
        self.assertEqual(report['status'], 'accepted')
        self.assertEqual(report['metrics']['frames'], 600)
        self.assertEqual(report['wall_seconds'], 30)
        self.assertEqual(report['metrics']['fps'], 20)
        self.assertFalse(report['metrics']['detailed_timing_enabled'])
        rejected, bad = self.sample_fixture('streaming', event='Native GPU: adopted guest texture 0x1234')
        self.assertEqual(bad['status'], 'rejected')
        self.assertTrue((rejected/'frames.csv').exists())
        self.assertIn('streaming', bad['rejection_reasons'][0])
        comparison = comparer.compare([output, rejected])
        self.assertEqual([row['status'] for row in comparison['outcomes']], ['accepted','rejected'])
        self.assertEqual(comparison['accepted_aggregate']['3']['windows'], 1)
        only_rejected=comparer.compare([rejected])
        self.assertIsNone(only_rejected['comparable_identity'])
        self.assertEqual(only_rejected['accepted_aggregate'],{})
        self.assertEqual(len(only_rejected['outcomes']),1)
        with self.assertRaisesRegex(ValueError, 'overwriting'):
            run, scene = self.live_fixture('second-run')
            sampler.sample(run, output, scene, self.root)

    def test_sampler_refuses_absent_scene_route_runtime_or_pack_proof(self):
        run, scene = self.live_fixture()
        data = json.loads(scene.read_text())
        data['workers_held'] = False
        scene.write_text(json.dumps(data))
        with self.assertRaisesRegex(ValueError, 'workers_held'):
            sampler.context(run, scene, self.root)
        data['workers_held'] = True;scene.write_text(json.dumps(data))
        (run/'logs/game.log').write_text('GPU route: PM4 reference\n')
        with self.assertRaisesRegex(ValueError, 'route/slot'):
            sampler.context(run, scene, self.root)
        (run/'logs/game.log').write_text('GPU route: detached native D3D12 renderer\n'
                                       'Native GPU init: creating 3 command slots\n'
                                       'loaded additive precompiled pack, generation=1\n')
        with self.assertRaisesRegex(ValueError, 'publication provenance'):
            sampler.context(run, scene, self.root)
        (run/'rexruntime.dll').write_bytes(b'changed runtime')
        with self.assertRaisesRegex(ValueError, 'dependencies differ'):
            sampler.context(run, scene, self.root)

    def test_sampler_failed_settling_retains_rejection_and_never_selects_a_fast_window(self):
        run, scene = self.live_fixture()
        ticks = [0.0];frame = [60]
        def sleep(seconds):ticks[0]+=seconds
        def endpoint(_):
            with (run/'logs/game.log').open('a') as log:log.write('Native GPU: adopted guest texture\n')
            frame[0]+=60
            rows=[{'frame':float(i),'interval_ms':1.0} for i in range(frame[0]-59,frame[0]+1)]
            return {'frame':frame[0],'log_bytes':(run/'logs/game.log').stat().st_size,
                    'observed_workers':[]},rows
        out=self.root/'rejected-warm'
        with self.assertRaisesRegex(ValueError,'settling criteria'):
            sampler.sample(run,out,scene,self.root,take_endpoint=endpoint,sleep=sleep,clock=lambda:ticks[0])
        report=json.loads((out/'sample.json').read_text())
        self.assertEqual(report['status'],'rejected')
        self.assertEqual(len(report['warmup']),12)
        self.assertNotIn('metrics',report)

    def test_comparison_rejects_confounded_identity_and_tampered_metrics(self):
        a,_=self.sample_fixture('three',3)
        b,_=self.sample_fixture('twelve',12)
        result=comparer.compare([a,b])
        self.assertTrue(result['comparable_identity'])
        self.assertEqual(result['candidate_vs_baseline_fps_percent'],0)
        self.assertFalse(result['source_default_changed'])
        path=b/'sample.json';report=json.loads(path.read_text())
        report['shader_packs']=[{'generation':1,'sha256':'a'*64}]
        path.write_text(json.dumps(report))
        self.assertFalse(comparer.compare([a,b])['comparable_identity'])
        report['metrics']['fps']=999;path.write_text(json.dumps(report))
        with self.assertRaisesRegex(ValueError,'CSV disagrees'):
            comparer.compare([a,b])

    def test_complete_csv_tail_excludes_partial_rows_and_rejects_invalid_metrics(self):
        path=self.root/'frames.csv'
        path.write_text('frame,interval_ms,draw_calls,detailed_timing_enabled\n1,50,500,0\n2,50,500,0\n3,')
        size,rows=sampler.complete_rows(path)
        self.assertEqual([r['frame'] for r in rows],[1,2])
        self.assertLess(size,path.stat().st_size)
        path.write_text('frame,interval_ms,draw_calls,detailed_timing_enabled\n1,nan,500,0\n')
        with self.assertRaisesRegex(ValueError,'Non-finite'):
            sampler.complete_rows(path)

    def test_pm4_requires_exact_pair_provenance_not_just_import_symbols(self):
        self.compatibility.write_text(json.dumps({'plugin_sha256':'a'*64,'runtime_sha256':self.runtime_pin,
                                                 'evidence':'wrong ABI pair'}))
        with self.assertRaisesRegex(ValueError,'exact plugin/runtime ABI pair'):
            m.prepare(self.args(renderer='pm4'))

    def test_actual_primary_pe_dependencies_must_match_pinned_manifest(self):
        self.staging['imports']['legodimensions.exe'].append('unknown.dll')
        (self.install/'native-staging-manifest.json').write_text(json.dumps(self.staging))
        with self.assertRaisesRegex(ValueError,'PE dependencies differ'):
            m.prepare(self.args())

    def test_private_descriptor_rejects_extra_sdk_override_and_escaped_capture_path(self):
        run=Path(m.prepare(self.args(cadence_only=True))['run_root'])
        path=run/'regression-manifest.json';manifest=json.loads(path.read_text())
        env=manifest['launch_plan']['environment_overrides']
        env['REX_USER_DATA_ROOT']='Z:\\live\\profile'
        path.write_text(json.dumps(manifest))
        with self.assertRaisesRegex(ValueError,'Unexpected environment'):m.verify(run)
        del env['REX_USER_DATA_ROOT'];env['LEGO_DUMP_MISSING_SHADERS']='Z:\\live\\capture'
        path.write_text(json.dumps(manifest))
        with self.assertRaisesRegex(ValueError,'escaped the private run'):m.verify(run)

    def test_timer_mode_uses_presence_and_does_not_change_frozen_bytes(self):
        identities=[]
        for mode in ('spin','blocking'):
            run=Path(m.prepare(self.args('timer-'+mode,timer_wait=mode,cadence_only=True))['run_root'])
            manifest=json.loads((run/'regression-manifest.json').read_text())
            env=manifest['launch_plan']['environment_overrides']
            identities.append((m.digest(run/'legodimensions.exe'),m.digest(run/'rexruntime.dll')))
            self.assertEqual(manifest['timer_wait'],mode)
            self.assertEqual(env.get('REX_TIMER_WAIT_BLOCKING'),'1' if mode=='blocking' else None)
            env['REX_TIMER_WAIT_BLOCKING']='0'
            (run/'regression-manifest.json').write_text(json.dumps(manifest))
            with self.assertRaises(ValueError):m.verify(run)
        self.assertEqual(identities[0],identities[1])

    def test_comparison_must_vary_only_one_declared_setting(self):
        a,_=self.sample_fixture('timer-a',3)
        b,_=self.sample_fixture('timer-b',3)
        path=b/'sample.json';report=json.loads(path.read_text());report['timer_wait']='blocking'
        report['flags']['REX_TIMER_WAIT_BLOCKING']='1';path.write_text(json.dumps(report))
        self.assertFalse(comparer.compare([a,b])['comparable_identity'])
        self.assertTrue(comparer.compare([a,b],varying='timer-wait')['comparable_identity'])
        report['flags']['LEGO_NATIVE_COMMAND_SLOTS']='12';report['slots']=12
        path.write_text(json.dumps(report))
        self.assertFalse(comparer.compare([a,b],varying='timer-wait')['comparable_identity'])

    def test_inventory_limit_stops_iteration_before_materialization_and_refuses_aliases(self):
        directory=self.root/'bounded-tree';directory.mkdir()
        for index in range(5):(directory/str(index)).write_bytes(b'x')
        with patch.object(m,'MAX_TREE_ENTRIES',4):
            with self.assertRaisesRegex(ValueError,'bounded inventory'):m.regular_tree(directory)
        original=directory/'0'
        def entries(_,*args):
            for _ in range(m.MAX_TREE_ENTRIES+1):yield original
            raise AssertionError('Unbounded iterator consumed before inventory guard')
        with patch.object(Path,'rglob',entries):
            with self.assertRaisesRegex(ValueError,'bounded inventory'):m.regular_tree(directory)
        (directory/'alias').symlink_to(original)
        with self.assertRaisesRegex(ValueError,'may not alias'):m.regular_tree(directory)

    def test_json_evidence_reads_are_bounded_and_atomic_updates_keep_valid_json(self):
        path=self.root/'evidence.json';m.write_json(path,{'status':'first'})
        self.assertEqual(m.read_json(path,64),{'status':'first'})
        m.write_json(path,{'status':'finished','exit_code':0})
        self.assertEqual(m.read_json(path,64)['exit_code'],0)
        self.assertFalse(path.with_suffix('.json.staging').exists())
        with self.assertRaisesRegex(ValueError,'size bound'):m.read_json(path,4)
        with self.assertRaises(FileExistsError):m.write_json(path,{'overwrite':True},exclusive=True)
        self.assertEqual(m.read_json(path,64)['exit_code'],0)
        self.assertEqual(list(self.root.glob('.evidence.json-*')),[])

    def test_sampler_bounds_context_and_interval_logs_before_reading(self):
        run,scene=self.live_fixture()
        with patch.object(sampler,'MAX_CONTEXT_LOG_BYTES',4):
            with self.assertRaisesRegex(ValueError,'bounded evidence'):sampler.context(run,scene,self.root)
        with patch.object(sampler,'MAX_INTERVAL_LOG_BYTES',4):
            with self.assertRaisesRegex(ValueError,'bounded evidence'):
                sampler.interval_log(run,{'log_bytes':0},{'log_bytes':5})
        with self.assertRaisesRegex(ValueError,'rotated/truncated'):
            sampler.interval_log(run,{'log_bytes':10},{'log_bytes':9})

    def test_large_linked_executable_uses_bounded_mapping_not_small_dll_limit(self):
        source=self.root/'large.exe';source.write_bytes(synthetic_pe({'KERNEL32.dll':['FixtureKernel']}))
        with source.open('r+b') as target:target.truncate(64*1024**2+1)
        with self.assertRaisesRegex(ValueError,'inspection bound'):m.pe_tables(source)
        self.assertEqual(set(m.pe_tables(source,1024**3)['imports']),{'kernel32.dll'})


if __name__ == '__main__':
    unittest.main(verbosity=2)
