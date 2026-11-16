"""Build companion motion shaders from already-preprocessed native HLSL.

The normal shader bank stays unchanged. The companion VS evaluates the exact
translated vertex program with current and previous float constants. Its
companion PS retains the original alpha/discard/depth behavior, but returns
velocity and a history-rejection mask instead of material colors.
Game shader sources and compiled outputs belong in a private output directory.
"""
import argparse
import re
from pathlib import Path


def split_entry(source):
    found = list(re.finditer(r'\bvoid\s+main\s*\(', source))
    if len(found) != 1:
        raise ValueError('Expected one void main entry')
    match = found[0]
    start = source.index('(', match.start())
    level, end = 1, start + 1
    while level and end < len(source):
        level += (source[end] == '(') - (source[end] == ')')
        end += 1
    if level:
        raise ValueError('Unterminated entry signature')
    end -= 1
    signature = source[start + 1:end]
    # Preprocessing must happen first: duplicate branches and macros cannot
    # safely be rewritten with a text-level signature parser.
    if '#' in signature:
        raise ValueError('Input must be preprocessed HLSL')
    params = []
    for item in signature.split(','):
        item = re.sub(r'\[\[.*?\]\]', '', item).strip()
        parsed = re.fullmatch(
            r'(in|out)\s+(?:(precise|centroid|noperspective)\s+)?'
            r'(float[1-4]?|uint[1-4]?|int[1-4]?|bool)\s+(\w+)\s*:\s*(\w+)', item)
        if not parsed:
            raise ValueError('Unsupported parameter: ' + item)
        params.append((parsed[1], parsed[3], parsed[4], parsed[5], item))
    body_start = source.index('{', end)
    depth = 1
    body_end = body_start + 1
    while depth and body_end < len(source):
        depth += (source[body_end] == '{') - (source[body_end] == '}')
        body_end += 1
    if depth or source[body_end:].strip():
        raise ValueError('Expected main to be the final complete function')
    prefix = re.sub(r'\[shader\("(?:vertex|pixel)"\)\]\s*$', '',
                    source[:match.start()])
    return prefix, params, source[body_start:body_end]


def build(source, stage):
    source = re.sub(r'^\s*#(?:line\b.*|\s*\d+.*)$', '', source, flags=re.M)
    prefix, params, body = split_entry(source)
    inputs = [p for p in params if p[0] == 'in']
    outputs = [p for p in params if p[0] == 'out']
    semantics = {p[3].upper() for p in params}
    if {'LEGOCURRENTCLIP0', 'LEGOPREVIOUSCLIP0'} & semantics:
        raise ValueError('Motion semantic collision')
    signature = ',\n'.join(p[4] for p in params)
    original = 'void LegoCurrent(' + signature + ')\n' + body
    if stage == 'vs':
        if [p[2] for p in outputs if p[3].upper() == 'SV_POSITION'] != ['oPos']:
            raise ValueError('Expected the translated vertex position oPos')
        previous = body.replace('g_FloatConstants', 'g_PreviousFloatConstants')
        prefix += ('\ncbuffer LegoPreviousConstants : register(b4, space4) {\n'
                   ' float4 g_PreviousFloatConstants[256] : packoffset(c0);\n'
                   ' uint g_PreviousValid : packoffset(c256.x);\n};\n')
        previous_function = 'void LegoPrevious(' + signature + ')\n' + previous
        locals_ = '\n'.join(p[1] + ' previous_' + p[2] + ';' for p in outputs)
        current_args = ','.join(p[2] for p in params)
        previous_args = ','.join(p[2] if p[0] == 'in' else 'previous_' + p[2]
                                 for p in params)
        wrapper = ('void main(' + signature + ',\n'
                   'out float4 oCurrentClip : LEGOCURRENTCLIP0,\n'
                   'out float4 oPreviousClip : LEGOPREVIOUSCLIP0) {\n'
                   + locals_ + '\nLegoCurrent(' + current_args + ');\n'
                   'LegoPrevious(' + previous_args + ');\n'
                   'oCurrentClip = oPos;\n'
                   'oPreviousClip = g_PreviousValid ? previous_oPos : 0.0;\n}\n')
        return prefix + original + '\n' + previous_function + '\n' + wrapper
    if stage not in ('ps', 'ps_mrt'):
        raise ValueError('Expected vs or ps')
    combined = stage == 'ps_mrt'
    for p in outputs:
        if not (re.fullmatch(r'SV_TARGET[0-3]', p[3].upper()) or
                p[3].upper() == 'SV_DEPTH'):
            raise ValueError('Unsupported pixel output: ' + p[3])
    locals_ = '' if combined else '\n'.join(p[1] + ' discarded_' + p[2] + ';'
                        for p in outputs if p[3].upper() != 'SV_DEPTH')
    args = ','.join(p[2] if combined or p[0] == 'in' or p[3].upper() == 'SV_DEPTH'
                    else 'discarded_' + p[2] for p in params)
    # Rasterizer-generated inputs must follow the interpolators. Keeping
    # SV_IsFrontFace before the new clips shifts their PS register numbers,
    # while the VS outputs have no front-face register: D3D12 rejects the PSO.
    generated_inputs = [p for p in inputs if p[3].upper() in {'SV_ISFRONTFACE', 'SV_PRIMITIVEID', 'SV_SAMPLEINDEX'}]
    wrapper_params = [p[4] for p in inputs if p not in generated_inputs]
    if combined:
        wrapper_params.extend(p[4] for p in outputs if p[3].upper() != 'SV_DEPTH')
    wrapper_params.extend([
        'in float4 iCurrentClip : LEGOCURRENTCLIP0',
        'in float4 iPreviousClip : LEGOPREVIOUSCLIP0',
        'out float2 oVelocity : SV_Target' + ('4' if combined else '0'),
        'out float oReactive : SV_Target' + ('5' if combined else '1')])
    wrapper_params.extend(p[4] for p in generated_inputs)
    wrapper_params.extend(p[4] for p in outputs if p[3].upper() == 'SV_DEPTH')
    wrapper = ('void main(' + ',\n'.join(wrapper_params) + ') {\n'
               + locals_ + '\nLegoCurrent(' + args + ');\n'
               'bool valid = all(isfinite(iCurrentClip)) && all(isfinite(iPreviousClip))'
               ' && iCurrentClip.w > 0.000001 && iPreviousClip.w > 0.000001;\n'
               'float2 currentUV = iCurrentClip.xy / max(iCurrentClip.w, 0.000001) * float2(0.5,-0.5);\n'
               'float2 previousUV = iPreviousClip.xy / max(iPreviousClip.w, 0.000001) * float2(0.5,-0.5);\n'
               'float2 velocity = previousUV - currentUV;\n'
               'valid = valid && all(abs(velocity) < 1.0);\n'
               'oVelocity = valid ? velocity : 0.0;\n'
               'oReactive = valid ? 0.0 : 1.0;\n}\n')
    return prefix + original + '\n' + wrapper


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('input', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--stage', choices=['vs', 'ps'], required=True)
    args = parser.parse_args()
    result = build(args.input.read_text(), args.stage)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(result)
