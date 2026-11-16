"""Compile and execute companion motion shaders without starting the game."""
import argparse
import hashlib
import json
import re
from pathlib import Path
import runpy
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    parser.add_argument('--dxc', type=Path, required=True)
    parser.add_argument('--host-compiler', default='clang-cl')
    parser.add_argument('--captured-hlsl', type=Path, action='append', default=[])
    parser.add_argument('--vertex-textures', action='store_true',
                        help='Bind controlled static texels for captured vertex texture programs')
    parser.add_argument('--depth-pair', type=int, nargs=2, action='append', default=[],
                        metavar=('PREPASS_INDEX','MATERIAL_INDEX'))
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    scripts = Path(__file__).resolve().parent
    generator = runpy.run_path(str(scripts/'prepare_temporal_motion.py'))
    build = generator['build']
    vs = '''cbuffer VertexShaderConstants : register(b0, space4) { float4 g_FloatConstants[256]; };
void main(in float4 iPosition0 : POSITION0, out precise float4 oPos : SV_Position,
          out float4 oTexCoord0 : TEXCOORD0) {
 float4 p = iPosition0;
 p.x += g_FloatConstants[4].x;
 p.x += g_FloatConstants[5].x * iPosition0.y;
 oPos = p.x*g_FloatConstants[0] + p.y*g_FloatConstants[1]
      + p.z*g_FloatConstants[2] + p.w*g_FloatConstants[3];
 oTexCoord0 = iPosition0;
}'''
    ps = '''cbuffer PixelShaderConstants : register(b1, space4) { float4 g_FloatConstants[224]; };
void main(in float4 iPosition : SV_Position, in float4 iTexCoord0 : TEXCOORD0,
          out float4 oC0 : SV_Target0, out float4 oC1 : SV_Target1,
          out float oDepth : SV_Depth) {
 if(iTexCoord0.x < g_FloatConstants[0].x) discard;
 oC0 = float4(0.25,iTexCoord0.y,0.75,1.0); oC1 = 0.125;
 oDepth = iPosition.z + iTexCoord0.y*0.1;
}'''
    logs = []
    def compile_(source, name, stage):
        path = args.output/(name+'.hlsl')
        path.write_text(source)
        out = args.output/(name+'.dxil')
        call = subprocess.run([str(args.dxc), str(path), '-T', stage+'_6_0',
            '-E', 'main', '-HV', '2021', '-Wno-ignored-attributes', '-Fo', str(out)],
            capture_output=True, text=True, timeout=120)
        logs.append({'name':name,'exit_code':call.returncode,'stdout':call.stdout,'stderr':call.stderr})
        if call.returncode:
            raise RuntimeError(name+' compilation failed: '+call.stderr)
        return out
    original_vs = compile_(vs, 'original-vs', 'vs')
    original_ps = compile_(ps, 'original-ps', 'ps')
    motion_vs_source = build(vs, 'vs')
    motion_ps_source = build(ps, 'ps')
    motion_vs = compile_(motion_vs_source, 'motion-vs', 'vs')
    motion_ps = compile_(motion_ps_source, 'motion-ps', 'ps')
    exe = args.output/'test-temporal-motion.exe'
    call = subprocess.run([args.host_compiler, str(scripts/'test_temporal_motion_gpu.cpp'),
        '/std:c++20','/O2','/MD','/EHsc','/Fo'+str(args.output/'fixture.obj'),
        '/Fe'+str(exe), '/link','d3d12.lib','dxgi.lib','dxguid.lib'],
        capture_output=True, text=True, timeout=120)
    if call.returncode:
        raise RuntimeError('Fixture compilation failed: '+call.stdout+call.stderr)
    def execute(vs_path, ps_path):
        return subprocess.run([str(exe),str(original_vs),str(original_ps),str(vs_path),str(ps_path)],
                              capture_output=True, text=True, timeout=60)
    positive = execute(motion_vs,motion_ps)
    print(positive.stdout)
    if positive.returncode:
        raise RuntimeError('Motion GPU fixture failed: '+positive.stdout+positive.stderr)
    # Both mutations compile. The GPU must reject wrong previous transforms and
    # alpha coverage, rather than treating compilation success as validation.
    mutated_vs = compile_(motion_vs_source.replace(
        'g_PreviousFloatConstants[','g_FloatConstants[').replace(
        'float4 g_FloatConstants[256] : packoffset(c0);',
        'float4 unusedPreviousConstants[256] : packoffset(c0);'), 'mutated-current-vs', 'vs')
    negative_motion = execute(mutated_vs,motion_ps)
    if negative_motion.returncode != 7:
        raise RuntimeError('Previous=current mutation did not fail motion validation: '+negative_motion.stdout)
    mutated_ps = compile_(motion_ps_source.replace(
        'if(iTexCoord0.x < g_FloatConstants[0].x) discard;', ''), 'mutated-alpha-ps', 'ps')
    negative_alpha = execute(motion_vs,mutated_ps)
    if negative_alpha.returncode != 5:
        raise RuntimeError('Removed alpha discard did not fail depth/coverage validation: '+negative_alpha.stdout)
    real = []
    real_gpu = []
    vertices = {}
    for index, path in enumerate(args.captured_hlsl):
        text = path.read_text()
        stage = 'vs' if 'vertex shader' in text[:200] else 'ps' if 'pixel shader' in text[:200] else None
        if stage is None:
            raise RuntimeError('Missing captured stage marker: '+str(path))
        for spec in range(4 if stage=='ps' else 2):
            name = 'captured-'+str(index)+'-'+str(spec)
            prepared = args.output/(name+'-input.hlsl')
            prepared.write_text(text.replace('uint g_SpecConstants();',
                'uint g_SpecConstants() { return '+str(spec)+'; }'))
            expanded = args.output/(name+'-preprocessed.hlsl')
            subprocess.run([str(args.dxc),str(prepared),'-T',stage+'_6_0',
                '-DREBLUE_RECOMP','-DLEGO_NATIVE_GPU','-HV','2021',
                '-P','-Fi',str(expanded)],check=True,capture_output=True,timeout=120)
            result = compile_(build(expanded.read_text(),stage),name,stage)
            real.append({'input':str(path),'stage':stage,'specialization':spec,
                         'dxil_sha256':hashlib.sha256(result.read_bytes()).hexdigest()})
            if stage == 'vs':
                # Match the original physical signature slots. Removing unused
                # TEXCOORD/COLOR declarations shifts the custom clip registers
                # and makes DXIL stage linkage invalid on D3D12.
                _, params, _ = generator['split_entry'](
                    re.sub(r'^\s*#(?:line\b.*|\s*\d+.*)$', '',
                           expanded.read_text(), flags=re.M))
                inputs = ['in '+p[1]+' input_'+p[2]+' : '+p[3]
                          for p in params if p[0]=='out']
                position = next('input_'+p[2] for p in params
                                if p[3].upper()=='SV_POSITION' and p[0]=='out')
                probe = ('void main('+','.join(inputs)+
                         ',out float2 color : SV_Target0,out float mask : SV_Target1,'
                         'out float depth : SV_Depth) { color=0; mask=0; depth='+position+'.z; }')
                probe_ps = compile_(probe,name+'-probe-ps','ps')
                probe_motion_ps = compile_(build(probe,'ps'),name+'-probe-motion-ps','ps')
                normal_vertex = compile_(expanded.read_text(),name+'-normal-vs','vs')
                vertices[index,spec] = normal_vertex, probe_ps
                vertex_mode = ('texture-real-packed' if spec else 'texture-real') if args.vertex_textures else 'real'
                gpu = subprocess.run([str(exe),str(normal_vertex),str(probe_ps),
                    str(result),str(probe_motion_ps),vertex_mode],capture_output=True,text=True,timeout=60)
                print(str(path)+':\n'+gpu.stdout)
                if gpu.returncode:
                    raise RuntimeError('Captured vertex GPU test failed: '+gpu.stdout+gpu.stderr)
                real_gpu.append({'input':str(path),'specialization':spec,
                                 'exit_code':gpu.returncode,'stdout':gpu.stdout,
                                 'geometry':('128 affine quads; attributes=(0,0,0,1); decoded tangent.w='+str(0 if spec else 1))
                                    if args.vertex_textures else 'controlled quad; zero unused attributes',
                                 'pixel_shader':'signature-matched depth probe; not game material'})
                if args.vertex_textures:
                    bad = build(expanded.read_text(),stage).replace(
                        'g_PreviousFloatConstants[','g_FloatConstants[').replace(
                        'float4 g_FloatConstants[256] : packoffset(c0);',
                        'float4 unusedPreviousConstants[256] : packoffset(c0);')
                    bad_vs=compile_(bad,name+'-current-only','vs')
                    rejected=subprocess.run([str(exe),str(normal_vertex),str(probe_ps),
                        str(bad_vs),str(probe_motion_ps),vertex_mode],capture_output=True,text=True,timeout=60)
                    if rejected.returncode!=7:
                        raise RuntimeError('Textured previous=current mutation escaped: '+rejected.stdout+rejected.stderr)
                    real_gpu[-1]['current_only_negative_exit']=rejected.returncode
                    real_gpu[-1]['textures']='controlled static RGBA mip chain, SRV/sampler 0; not captured assets'
                    if spec == 0:
                        # Target wind alone: a camera-motion failure cannot stand
                        # in for proof that c19 deformation uses previous values.
                        wind_bad = build(expanded.read_text(),stage).replace(
                            'g_PreviousFloatConstants[19]', 'g_FloatConstants[19]')
                        if wind_bad == build(expanded.read_text(),stage):
                            raise RuntimeError('Wind mutation did not find expanded c19 references')
                        wind_vs=compile_(wind_bad,name+'-current-wind-only','vs')
                        rejected_wind=subprocess.run([str(exe),str(normal_vertex),str(probe_ps),
                            str(wind_vs),str(probe_motion_ps),vertex_mode],capture_output=True,text=True,timeout=60)
                        if rejected_wind.returncode!=7 or 'motion mismatch wind' not in rejected_wind.stdout:
                            raise RuntimeError('Previous wind mutation escaped: '+rejected_wind.stdout+rejected_wind.stderr)
                        real_gpu[-1]['current_wind_only_negative_exit']=rejected_wind.returncode
                        real_gpu[-1]['current_wind_only_negative_stdout']=rejected_wind.stdout
    depth_pairs = []
    for a,b in args.depth_pair:
        for spec_a in range(2):
            for spec_b in range(2):
                vertex_a,pixel_a=vertices[a,spec_a]
                vertex_b,pixel_b=vertices[b,spec_b]
                pair=subprocess.run([str(exe),str(vertex_a),str(pixel_a),str(vertex_b),
                                     str(pixel_b),'pair'],capture_output=True,text=True,timeout=60)
                print('Depth pair '+str((a,spec_a,b,spec_b))+': '+pair.stdout)
                if pair.returncode:
                    raise RuntimeError('Depth pair mismatch: '+pair.stdout+pair.stderr)
                depth_pairs.append({'prepass':str(args.captured_hlsl[a]),
                    'material':str(args.captured_hlsl[b]),'specializations':[spec_a,spec_b],
                    'exit_code':pair.returncode,'stdout':pair.stdout,
                    'scope':'128 controlled affine transforms; not captured scene geometry'})
    # Ensure the separate pair check detects an incorrect position transform.
    if args.depth_pair:
        a,_=args.depth_pair[0]
        vertex_a,pixel_a=vertices[a,0]
        expanded=(args.output/('captured-'+str(a)+'-0-preprocessed.hlsl')).read_text()
        source=re.sub(r'^\s*#(?:line\b.*|\s*\d+.*)$','',expanded,flags=re.M)
        prefix,params,body=generator['split_entry'](source)
        signature=','.join(p[4] for p in params)
        mutation=(prefix+'void LegoBaseline('+signature+')'+body+
            '\nvoid main('+signature+') { LegoBaseline('+','.join(p[2] for p in params)+
            '); oPos.z += 0.001 * oPos.w; }')
        mutated=compile_(mutation,'mutated-pair-vs','vs')
        negative=subprocess.run([str(exe),str(vertex_a),str(pixel_a),str(mutated),
                                  str(pixel_a),'pair'],capture_output=True,text=True,timeout=60)
        if negative.returncode!=9:
            raise RuntimeError('Wrong depth transform did not fail pair validation: '+negative.stdout)
    report = {'game_launched':False,'frame_generation':False,'native_integration_proven':False,
              'positive_exit':positive.returncode,'positive_stdout':positive.stdout,
              'previous_equals_current_negative_exit':negative_motion.returncode,
              'alpha_discard_removed_negative_exit':negative_alpha.returncode,
              'real_shader_compilation':real,'real_vertex_gpu':real_gpu,
              'depth_pairs':depth_pairs,'depth_pair_negative_exit':negative.returncode if args.depth_pair else None,
              'compilation':logs}
    (args.output/'verification.json').write_text(json.dumps(report,indent=2))
    print('PASS GPU motion/depth/cutout tests and both compiled negative controls')


if __name__ == '__main__':
    main()
