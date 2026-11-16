"""Exercise production variant lookup, type isolation and device ownership."""
import argparse
from pathlib import Path
import subprocess

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('output',type=Path);p.add_argument('--compiler',default='clang++')
    a=p.parse_args();out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
    root=Path(__file__).resolve().parents[2]
    text=(root/'rexlego/src/gpu_native/float_blend_indices.cpp').read_text()
    body=text[text.index('namespace legodimensions::gpu_native {'):]
    metadata=out/'variants.inc'
    metadata.write_text('const FloatBlendIndicesEntry g_float_blend_indices_entries[] = {\n'
        '{10,0,0,1,0},{10,0,1,1,1},{10,1,2,1,0},{10,1,3,1,1},{20,0,4,1,1}};\n')
    code=r'''
#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <tuple>
#include <cstdio>
#include <cstdlib>
int live=0,created=0;bool available=true;
#define CHECK(c) do{if(!(c)){std::fprintf(stderr,"Failed %s\n",#c);return 23;}}while(0)
namespace plume {
enum class RenderShaderFormat{DXIL};
struct RenderShader {int byte;explicit RenderShader(int b):byte(b){++live;}~RenderShader(){--live;}};
struct Device {
 std::unique_ptr<RenderShader> createShader(const void* p,size_t n,const char*,RenderShaderFormat){
  ++created;return available&&n?std::make_unique<RenderShader>(*static_cast<const unsigned char*>(p)):nullptr;
 }
};
}
namespace legodimensions::gpu_native {
struct ShaderBytecode {const unsigned char* data=nullptr;size_t size=0;explicit operator bool()const{return data&&size;}};
struct HostDevice {static plume::Device* Device(){static plume::Device d;return &d;}};
}
''' + body + r'''
extern "C" const unsigned char g_floatBlendIndicesBlob[]={11,12,13,14,15};
int main(){using namespace legodimensions::gpu_native;
 CHECK(FindFloatBlendIndicesDxil(10,0).data[0]==11);
 CHECK(FindPositionPrecisionDxil(10,0).data[0]==12);
 CHECK(FindFloatBlendIndicesDxil(10,3).data[0]==13);
 CHECK(FindPositionPrecisionDxil(10,3).data[0]==14);
 CHECK(!FindFloatBlendIndicesDxil(20,0));CHECK(FindPositionPrecisionDxil(20,0).data[0]==15);
 CHECK(!FindFloatBlendIndicesDxil(30,0));CHECK(!FindPositionPrecisionDxil(30,0));
 auto* f=ResolveFloatBlendIndicesShader(10,0);auto* p=ResolvePositionPrecisionShader(10,0);
 CHECK(f&&p&&f!=p&&f->byte==11&&p->byte==12&&live==2);
 CHECK(ResolvePositionPrecisionShader(10,2)==p&&created==2);
 available=false;CHECK(!ResolvePositionPrecisionShader(10,1));CHECK(live==2);
 available=true;CHECK(ResolvePositionPrecisionShader(10,1)->byte==14);CHECK(live==3);
 ResetFloatBlendIndicesShaders();CHECK(live==0);
 CHECK(ResolvePositionPrecisionShader(20,0)->byte==15);CHECK(live==1);
 ResetFloatBlendIndicesShaders();CHECK(live==0);
 std::puts("PASS native variant kinds, specialization, failure isolation, cache and reset");
}
'''
    source=out/'variants.cpp';source.write_text(code);exe=out/'variants.exe'
    subprocess.run([a.compiler,'-std=c++20','-O2','-DLEGO_FLOAT_BLEND_INDICES_METADATA="'+metadata.as_posix()+'"',str(source),'-o',str(exe)],check=True,timeout=45)
    subprocess.run([str(exe)],check=True,timeout=10)

if __name__=='__main__':main()
