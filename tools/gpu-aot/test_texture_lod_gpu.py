"""Exercise translated setTexLOD and fetch bias on a windowless mip-color GPU test."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import runpy
import struct
import subprocess


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('compiler',type=Path)
    p.add_argument('common',type=Path)
    p.add_argument('output',type=Path)
    p.add_argument('--dxc',type=Path,required=True)
    args=p.parse_args();out=args.output.resolve();out.mkdir(parents=True,exist_ok=True)
    scripts=Path(__file__).resolve().parent
    fixture=runpy.run_path(str(scripts/'test_texture_lod.py'))['fixture']
    old=fixture();data=bytearray(188);data[:128]=old[:128]
    struct.pack_into('>I',data,8,60);struct.pack_into('>I',data,84,60)
    # MAX c0.z,c0.z -> r7.z, setTexLOD r7.z, tfetch2D, export r2.
    struct.pack_into('>3I',data,128,1|(4<<12)|(20<<16),2<<12,0)
    struct.pack_into('>3I',data,140,7|(4<<16),0,2<<24)
    data[152:188]=old[140:176]
    inputs=out/'input';inputs.mkdir(exist_ok=True);(inputs/'register-lod.bin').write_bytes(data)
    env=os.environ.copy();env['XENOS_RECOMP_DXIL_ONLY']='1';env['XENOS_RECOMP_LEGO_NATIVE_SCALE']='1'
    env.pop('XENOS_RECOMP_ONLY_HASH',None)
    subprocess.run([str(args.compiler.resolve()),str(inputs),str(out/'cache.cpp'),
        str(args.common.resolve()),str(out/'hlsl')],env=env,check=True,capture_output=True,timeout=120)
    text=(out/'hlsl/register-lod.hlsl').read_text()
    if 'r7.z = max(LOAD_C(0).z, LOAD_C(0).z);' not in text:
        raise RuntimeError('The translated ALU did not load the requested register LOD')
    text=text.replace('uint g_SpecConstants();','uint g_SpecConstants() { return 0; }')
    vertex=('cbuffer C : register(b0,space4) { float4 constants[256]; };\n'
        'void main(in float4 p:POSITION0,out float4 oPos:SV_Position,'+
        ','.join('out float4 t'+str(i)+':TEXCOORD'+str(i) for i in range(16))+
        ',out float4 c0:COLOR0,out float4 c1:COLOR1) { oPos=p;'+
        ''.join('t'+str(i)+'=0;' for i in range(16))+'c0=c1=0; }')
    baseline='void main(out float2 color:SV_Target0) { color=0; }'
    def compile_(source,name,stage):
        path=out/(name+'.hlsl');path.write_text(source);binary=out/(name+'.dxil')
        subprocess.run([str(args.dxc),str(path),'-T',stage+'_6_0','-E','main',
            '-DREBLUE_RECOMP','-DLEGO_NATIVE_GPU','-HV','2021','-Wno-ignored-attributes',
            '-Fo',str(binary)],check=True,capture_output=True,timeout=120)
        return binary
    vs=compile_(vertex,'vertex','vs');ps=compile_(text,'translated-lod','ps')
    normal=compile_(baseline,'baseline','ps')
    mutation=text.replace('textureLod = r7.z;','textureLod = 0.0;')
    wrong=compile_(mutation,'removed-register-state','ps')
    duplicate=text.replace('float4 g_ColorOutputScale : packoffset(c38);',
        'float4 g_ColorOutputScale : packoffset(c38); \\\n    float4 g_TestFetchBias : packoffset(c39);')
    duplicate=duplicate.replace('textureLod + 0).xyzw;',
                                'textureLod + g_TestFetchBias.w + 0).xyzw;')
    double=compile_(duplicate,'double-fetch-bias','ps')
    exe=out/'test-lod-gpu.exe'
    subprocess.run(['clang-cl',str(scripts/'test_temporal_motion_gpu.cpp'),'/std:c++20',
        '/O2','/MD','/EHsc','/Fo'+str(out/'fixture.obj'),'/Fe'+str(exe),'/link',
        'd3d12.lib','dxgi.lib','dxguid.lib'],check=True,capture_output=True,timeout=120)
    def run(pixel,instruction_bias=0):
        return subprocess.run([str(exe),str(vs),str(normal),str(vs),str(pixel),'lod'+str(instruction_bias)],
                              capture_output=True,text=True,timeout=60)
    positive=run(ps);negative=run(wrong);double_negative=run(double)
    print(positive.stdout,end='')
    if positive.returncode or negative.returncode!=10 or double_negative.returncode!=10:
        raise RuntimeError('LOD GPU fixture failed: '+positive.stdout+positive.stderr+negative.stdout)
    signed=[]
    for instruction_bias in (-1,1):
        record=bytearray(data)
        word=struct.unpack_from('>I',record,172)[0]
        struct.pack_into('>I',record,172,(word&~(127<<2))|(((instruction_bias*16)&127)<<2))
        (inputs/'register-lod.bin').write_bytes(record)
        subprocess.run([str(args.compiler.resolve()),str(inputs),str(out/'cache.cpp'),
            str(args.common.resolve()),str(out/'hlsl')],env=env,check=True,capture_output=True,timeout=120)
        shader=(out/'hlsl/register-lod.hlsl').read_text().replace('uint g_SpecConstants();','uint g_SpecConstants() { return 0; }')
        binary=compile_(shader,'instruction-bias-'+str(instruction_bias),'ps')
        result=run(binary,instruction_bias);print(result.stdout,end='')
        if result.returncode:raise RuntimeError('Signed instruction bias failed: '+result.stdout)
        signed.append({'instruction_bias':instruction_bias,'exit_code':result.returncode,'stdout':result.stdout})
    (out/'verification.json').write_text(json.dumps({'game_launched':False,
        'native_integration_proven':False,'positive_exit':positive.returncode,
        'positive_stdout':positive.stdout,'removed_register_state_negative_exit':negative.returncode,
        'negative_stdout':negative.stdout,'double_bias_negative_exit':double_negative.returncode,
        'double_bias_negative_stdout':double_negative.stdout,'signed_instruction_bias':signed,
        'translated_dxil_sha256':hashlib.sha256(ps.read_bytes()).hexdigest()},indent=2))
    print('PASS register LOD + signed sampler/instruction bias; removed-state and double-bias mutations fail')


if __name__=='__main__':main()
