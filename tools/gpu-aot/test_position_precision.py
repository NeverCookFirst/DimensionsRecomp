"""Regression checks for safe preparation and selective shader-bank replacement."""
import runpy
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent
prepare = runpy.run_path(str(ROOT / 'prepare_position_precision.py'))
cache = runpy.run_path(str(ROOT / 'replace_dxil_cache_entries.py'))


class PositionPrecisionTest(unittest.TestCase):
    def test_preparation_preserves_non_native_and_rejects_ambiguous_input(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            file = root / 'shader_recompiler.cpp'
            file.write_text('        out += "\\tout float4 oPos : SV_Position";')
            prepare['apply'](root)
            text = file.read_text()
            self.assertIn('out precise float4 oPos : SV_Position', text)
            self.assertIn('#else\\n\\tout float4 oPos : SV_Position', text)
            with self.assertRaises(ValueError):
                prepare['apply'](root)

    def test_replacement_preserves_other_entries_and_roundtrips(self):
        base = {1: (1, b'DXBCoriginal'), 2: (2, b'DXBCuntouched')}
        updates = {1: (1, b'DXBCprecise'), 3: (1, b'DXBCother-bank')}
        result, matched = cache['replace'](base, updates)
        self.assertEqual(matched, [1])
        self.assertEqual(result[2], base[2])
        self.assertEqual(base[1][1], b'DXBCoriginal')
        with tempfile.TemporaryDirectory() as tmp:
            file = Path(tmp) / 'cache.cpp'
            cache['write'](file, result)
            self.assertEqual(cache['read'](file), result)

    def test_rejects_mask_change_and_no_matches(self):
        with self.assertRaises(ValueError):
            cache['replace']({1: (1, b'DXBC')}, {1: (0, b'DXBC')})
        with self.assertRaises(ValueError):
            cache['replace']({1: (1, b'DXBC')}, {2: (1, b'DXBC')})


if __name__ == '__main__':
    unittest.main()
