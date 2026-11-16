"""Normalize the audited static fog pass to its existing material partners.

Only the identified F3D6234789656815 program is accepted. The original world
position, fog, alpha, uniforms, inputs and viewport epilogue are preserved.
This is not a general precise-position policy or a depth bias.
"""
import argparse
from pathlib import Path
import re

HASH = 'F3D6234789656815'
MARKER = '// LEGO_CANONICAL_STATIC_POSITION_V1'
BLOCK = '''
// LEGO_CANONICAL_STATIC_POSITION_V1
// Keep the clip sum grouped like E9528B56C97B3453 and the static materials.
// r3 is the original world position already used by this pass's fog outputs.
float3 legoStaticWorld = r3.xyz;
float4 legoStaticC0 = max(LOAD_C(0), LOAD_C(0));
float4 legoStaticC1 = max(LOAD_C(1), LOAD_C(1));
float4 legoStaticC2 = max(LOAD_C(2), LOAD_C(2));
float4 legoStaticC3 = max(LOAD_C(3), LOAD_C(3));
float4 legoStaticClip = ((legoStaticWorld.y * legoStaticC1 +
                         legoStaticWorld.x * legoStaticC0) +
                         legoStaticWorld.z * legoStaticC2) + legoStaticC3;
oPos = max(legoStaticClip, legoStaticClip);
'''


def apply(text):
    match = re.search(r'// vertex shader\s+hash=0x([A-Fa-f0-9]+)', text[:300])
    if not match or match[1].upper() != HASH or MARKER in text:
        raise ValueError('Expected the unmodified audited static fog program')
    for expected in ['packoffset(c38)', 'out float4 oPos : SV_Position',
                     'r3.xyz = max(r0.yzw, r0.yzw);']:
        if text.count(expected) != 1:
            raise ValueError('Unexpected static fog program structure: '+expected)
    epilogue = '\n#ifdef LEGO_NATIVE_GPU\n\toPos = LegoViewportPosition(oPos, g_ViewportScale, g_ViewportMode);'
    if text.count(epilogue) != 1:
        raise ValueError('Expected one native viewport epilogue')
    return text.replace(epilogue, '\n'+BLOCK+epilogue)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    args.output.write_text(apply(args.source.read_text()))
