import contextlib
import io
import importlib.util
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location(
    'audit_d3d_paths', Path(__file__).with_name('audit_d3d_paths.py'))
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
audit = module.audit


class AuditTests(unittest.TestCase):
    def test_direct_calls_hook_barrier_and_indirect_warning(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for path in ('tools/gpu-aot', 'rexlego/src/gpu_native', 'rexlego/generated/default'):
                (root / path).mkdir(parents=True)
            (root / 'tools/gpu-aot/d3d-sep13-matches.tsv').write_text(
                'Seed\t0x00000000\t0x83FAF180\td3d9:ring.obj\n'
                '# ambiguous ignored\t0x0\t0x1,0x2\tx\n')
            (root / 'rexlego/src/gpu_native/hooks.cpp').write_text('REX_HOOK_RAW(sub_83000002) {}')
            (root / 'rexlego/generated/default/test_recomp.cpp').write_text('''
DEFINE_REX_FUNC(sub_83000001) { sub_83FAF180(ctx, base); REX_CALL_INDIRECT_FUNC(ctx.ctr.u32); }
DEFINE_REX_FUNC(sub_83000002) { sub_83000001(ctx, base); }
DEFINE_REX_FUNC(sub_83000003) { sub_83000002(ctx, base); }
DEFINE_REX_FUNC(sub_83000004) { sub_83FCBF00(ctx, base); }
''')
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                audit(root)
            result = output.getvalue()
            self.assertIn('83000001\t0\t1\t83000001 -> 83FAF180', result)
            self.assertIn('83000002\t1\t0\t83000002 -> 83000001 -> 83FAF180', result)
            self.assertNotIn('83000003\t', result)
            self.assertIn('83000004\t0\t0\t83000004 -> 83FCBF00', result)
            self.assertIn('ambiguous_rows=1', result)


if __name__ == '__main__':
    unittest.main()
