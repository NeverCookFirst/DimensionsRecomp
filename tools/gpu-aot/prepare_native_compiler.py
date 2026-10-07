"""Reconstruct the current ABI624 compiler from a pinned, patched checkout.

Only writes the requested scratch directory. Does not patch the checkout,
build anything, install archives, or launch the game.
"""
import argparse
import hashlib
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
                str(scratch/'alpha'), '--checkout', str(checkout)], check=True, cwd=root)
subprocess.run([sys.executable, str(scripts/'prepare_viewport_candidate.py'),
                str(scratch/'alpha/source'), str(scratch/'viewport')], check=True, cwd=root)
subprocess.run([sys.executable, str(scripts/'prepare_gradients_candidate.py'),
                str(scratch/'viewport/source'), str(scratch/'gradients')], check=True, cwd=root)
reflection_patch = scripts/'patches/xenosrecomp-empty-reflection.patch'
# This tracked patch applies to the pinned compiler's unchanged reflection
# setup after the ABI/viewport/gradient preparation steps. The absolute scratch
# directory is explicitly selected; no checkout or installed bank is modified.
subprocess.run(['git', 'apply', '--unsafe-paths', '-p2',
                '--directory=' + str(scratch/'gradients/source'),
                str(reflection_patch)], check=True, cwd=root, timeout=10)
compiler_source = scratch/'gradients/source/shader_recompiler.cpp'
reflection_source = compiler_source.read_text()
if (reflection_source.count('static const ConstantTable emptyConstantTable{};') != 1 or
    'assert(shaderContainer->constantTableOffset != NULL);' in reflection_source):
    raise RuntimeError('Optional-reflection patch did not update the prepared compiler')
fetch_patch = scripts/'patches/xenosrecomp-unmapped-vertex-fetch.patch'
subprocess.run(['git', 'apply', '--unsafe-paths', '-p2',
                '--directory=' + str(scratch/'gradients/source'),
                str(fetch_patch)], check=True, cwd=root, timeout=10)
fetch_source = compiler_source.read_text()
if (fetch_source.count('no semantic input mapping') != 1 or
    'assert(findResult != vertexElements.end());' in fetch_source):
    raise RuntimeError('Vertex-fetch patch did not update the prepared compiler')
(scratch/'compiler-inputs.json').write_text(json.dumps({
    'xenosrecomp_revision': revision, 'shared_constants_bytes': 624,
    'source': str(scratch/'gradients/source'), 'built': False,
    'optional_reflection_patch': str(reflection_patch),
    'optional_reflection_patch_sha256': hashlib.sha256(reflection_patch.read_bytes()).hexdigest(),
    'unmapped_vertex_fetch_patch': str(fetch_patch),
    'unmapped_vertex_fetch_patch_sha256': hashlib.sha256(fetch_patch.read_bytes()).hexdigest(),
    'archives_installed': False, 'game_launched': False,
}, indent=2), encoding='utf-8')
print(scratch/'gradients/source')
