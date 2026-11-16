"""Execute the emitted pixel-position expression against the SDK scale contract.

Consumes a compiled gradient fixture's HLSL; does not need private game data.
The removed unscaled expression must fail the same coordinate checks.
"""
import argparse,json,re,subprocess
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('hlsl',type=Path);p.add_argument('output',type=Path)
p.add_argument('--compiler',default='clang++');a=p.parse_args();a.output.mkdir(parents=True,exist_ok=True)
text=a.hlsl.read_text()
assert 'g_PixelPositionScale : packoffset(c36.w)' in text
match=re.search(r'float4 r7 = (.*?);',text,re.S);assert match
expression=match[1].replace('iPos.xy','iPos')
assert 'g_PixelPositionScale' in expression
harness=r'''
#include <cmath>
struct float2 {float x,y;float2(float a,float b):x(a),y(b){};
 float2 operator-(float a)const{return{x-a,y-a};}
 float2 operator*(float2 a)const{return{x*a.x,y*a.y};}
 float2 operator*(float a)const{return{x*a,y*a};}};
struct float4 {float x,y,z,w;float4(float2 a,float b,float c):x(a.x),y(a.y),z(b),w(c){}};
float4 Emit(float2 iPos,float g_PixelPositionScale,bool iFace){return EXPRESSION;}
int main(){
 for(bool face : {false,true})for(float scale : {0.0f,1.0f,2.0f/3.0f})
 for(int y=0;y<1080;y+=19)for(int x=0;x<1920;x+=17){
  const auto r=Emit({float(x)+0.5f,float(y)+0.5f},scale,face);
  const float inverse=scale>0?scale:1;
  // SDK rounds host center to the host pixel address, then reverts scale.
  const float ex=std::floor(float(x)+0.5f)*inverse*(face?1:-1);
  const float ey=std::floor(float(y)+0.5f)*inverse;
  if(std::abs(r.x-ex)>0.0002f||std::abs(r.y-ey)>0.0002f||r.z!=0||r.w!=0)return 17;
 }
}
'''.replace('#include <cmath>','#include <cmath>\n#include <initializer_list>').replace('EXPRESSION',expression)
for name,program,expected in [('scaled',harness,0),('unscaled',harness.replace('g_PixelPositionScale > 0.0 ? g_PixelPositionScale : 1.0','1.0'),17)]:
 cpp=a.output/(name+'.cpp');exe=a.output/(name+'.exe');cpp.write_text(program)
 subprocess.run([a.compiler,'-std=c++20','-DLEGO_NATIVE_GPU',str(cpp),'-o',str(exe)],check=True,timeout=45)
 r=subprocess.run([str(exe)],timeout=10);assert r.returncode==expected,(name,r.returncode)
(a.output/'verification.json').write_text(json.dumps({'result':'passed','hlsl':str(a.hlsl),'offset':588,'shared_bytes':624,'scales':[0,1,2/3],'front_back_faces':True,'unscaled_negative_exit':17},indent=2))
print('PASS: emitted guest pixel coordinates; unscaled negative control fails')
