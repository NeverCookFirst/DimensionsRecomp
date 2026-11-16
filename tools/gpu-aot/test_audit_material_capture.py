"""Offline audit regressions: shader use, physical conversion, provenance."""
import json
from pathlib import Path
import runpy
import tempfile
import unittest

api = runpy.run_path(str(Path(__file__).with_name('audit_material_capture.py')))


class AuditTests(unittest.TestCase):
    def test_resource_masks_allow_sampler_and_address_conversion(self):
        h = [2, 0xA0000014, 0x7FE3FF, 0xD10, 0x280, 0xB0000200]
        a = h.copy()
        a[0] ^= 0x003FFC00
        a[1] = 0x00000814
        a[3] ^= 0x7FF80000
        a[4] ^= 0xFFFFFFFF
        a[5] = 0x010003FF
        self.assertEqual(api['mismatch'](a, h), [])
        a[2] = 0
        a[5] ^= 0x600
        self.assertEqual(api['mismatch'](a, h), [2, 5])

    def test_basemap_and_signed_bias(self):
        d = api['decode']([2, 0x100014, 0x7FE3FF, 2 << 23, (1023 << 12) | (10 << 6), 0x200200])
        self.assertEqual(d['bias'], -1/32)
        self.assertEqual(d['max_lod'], 0.25)

    def test_used_slots_and_simultaneous_provenance(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root/'capture-draws.draws.tsv').write_text(
                'draw=1 frame=5 VS=123 PS=456\n'
                'texture_slot=0 guest=AB fetch=00000002,00100014,007fe3ff,00000000,00000280,00200200\n'
                'texture_slot=1 guest=CD fetch=00000003,00000000,00000000,00000000,00000000,00000000\n'
                'texture_header_slot=0 fetch=00000002,00100014,00000000,00000000,00000280,00200200\n'
                'texture_last_set_slot=0 ordinal=9 caller=82bce9b8 guest=AB changed_words=4 changed_resource_words=4\nsubmitted=1\n')
            index=root/'index.cpp'
            index.write_text('{ 0xAA, 0xBB, 0x123, 44, 0, 0x0 },\n{ 0xCC, 0xDD, 0x456, 48, 1, 0x1 },\n')
            report=api['audit'](root,api['shader_masks'](index))
            self.assertEqual(report['counts']['unused_texture_bindings_excluded'],1)
            self.assertEqual(len(report['findings']),1)
            self.assertEqual(report['findings'][0]['reference_kind'],'simultaneous_header')
            self.assertEqual(report['findings'][0]['resource_word_mismatches'],[2])
            self.assertIn('resource_fields_changed_after_SetTexture',report['findings'][0]['reasons'])
            self.assertEqual(report['findings'][0]['last_set_texture']['caller'],'82bce9b8')
            self.assertFalse(report['visual_fix_proven'])
            unknown=api['audit'](root,{})
            self.assertEqual(unknown['counts']['draws_without_complete_shader_masks'],1)
            self.assertEqual(unknown['counts']['audited_texture_bindings'],2)


if __name__ == '__main__':
    unittest.main()
