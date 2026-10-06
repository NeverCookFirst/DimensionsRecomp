"""Reconstruct the current ABI624 compiler from a pinned, patched checkout.

Only writes the requested scratch directory. Does not patch the checkout,
build anything, install archives, or launch the game.
"""
import argparse
import json
from pathlib import Path
import subprocess
import sys

PIN = '339af41df2c23dbe3256c1c377716b81a0e0fe6b'
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('checkout', type=Path)
p.add_argument('scratch', type=Path)
a = p.parse_args()
checkout, scratch = a.checkout.resolve(), a.scratch.resolve()
root = Path(__file__).resolve().parents[2]
revision = subprocess.check_output(
    ['git', '-C', str(checkout), 'rev-parse', 'HEAD'], text=True).strip()
if revision != PIN:
    p.error(f'Expected XenosRecomp {PIN}, got {revision}')
patch = root / 'tools/gpu-aot/patches/xenosrecomp-lego.patch'
subprocess.run(['git', '-C', str(checkout), 'apply', '--reverse', '--check', str(patch)], check=True)
if scratch.exists():
    p.error('Use a fresh scratch directory to avoid mixing compiler generations')
scripts = root / 'tools/gpu-aot'
subprocess.run([sys.executable, str(scripts/'prepare_alpha_candidate.py'),
                str(scratch/'alpha'), '--checkout', str(checkout)], check=True)
subprocess.run([sys.executable, str(scripts/'prepare_viewport_candidate.py'),
                str(scratch/'alpha/source'), str(scratch/'viewport')], check=True)
subprocess.run([sys.executable, str(scripts/'prepare_gradients_candidate.py'),
                str(scratch/'viewport/source'), str(scratch/'gradients')], check=True)
(scratch/'compiler-inputs.json').write_text(json.dumps({
    'xenosrecomp_revision': revision, 'shared_constants_bytes': 624,
    'source': str(scratch/'gradients/source'), 'built': False,
    'archives_installed': False, 'game_launched': False,
}, indent=2), encoding='utf-8')
print(scratch/'gradients/source')
