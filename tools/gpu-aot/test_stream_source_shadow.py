"""Differential native stream publication against actual TU23 setter/getter."""
import argparse
from pathlib import Path
import subprocess

p=argparse.ArgumentParser(description=__doc__);p.add_argument('output',type=Path)
p.add_argument('--compiler',default='clang++');a=p.parse_args()
root=Path(__file__).resolve().parents[2];out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
def extract(name):
 matches=[]
 for path in (root/'rexlego/generated/default').glob('*.cpp'):
  text=path.read_text();start=text.find('DEFINE_REX_FUNC('+name+') {')
  if start>=0:
   end=text.find('\nDEFINE_REX_FUNC(',start+1);matches.append(text[start:end if end>=0 else len(text)])
 if len(matches)!=1:raise ValueError('Expected one original '+name)
 return matches[0]
prefix=(root/'tools/gpu-aot/test_sampler_state.cpp').read_text().split('#include "tu23_set_texture_oracle.inc"')[0]
prefix=prefix.replace('#include "gpu_native/sampler_state.h"','#include "gpu_native/stream_source_shadow.h"')
prefix=prefix.replace('int32_t s32;','int32_t s32; uint8_t u8;')
prefix=prefix.replace('  int xer = 0;','  struct { bool ca=false; operator int() const {return 0;} } xer;')
prefix=prefix.replace('#define REX_LOAD_U8(x) base[uint32_t(x)]','#define REX_LOAD_U8(x) base[uint32_t(x)]\n#define REX_STORE_U8(x,v) (base[uint32_t(x)]=uint8_t(v))')
prefix+='''
void __savegprlr_26(OracleContext&,uint8_t*){}
void __restgprlr_26(OracleContext&,uint8_t*){}
void sub_83FC4CE0(OracleContext&ctx,uint8_t*base){
 uint32_t n=Read32(base,ctx.r3.u32+4)+1;Write32(base,ctx.r3.u32+4,n);ctx.r3.u32=n;
}
'''
code=prefix+extract('sub_83FBA160')+extract('sub_83FBA280')+r'''
int main(){
 std::vector<uint8_t> memory(0x40000);std::mt19937 random(0x83FBA160);
 constexpr uint32_t device=0x10000,buffer=0x20000,outOffset=0x28000,outStride=0x28004;
 for(uint32_t n=0;n<100000;++n){
  for(uint32_t i=0;i<0x6080;++i)memory[device+i]=uint8_t(random());
  const uint32_t stream=n%18,ptr=n%3?buffer:0,offset=(random()%1024)*4,stride=(random()%255)*4;
  const uint32_t lo=0xF0000000u|(random()&0x07FFF000u),hi=65536;
  Write32(memory.data(),buffer+24,lo);Write32(memory.data(),buffer+28,hi);
  Write32(memory.data(),device+12836+4*stream,0);
  const uint64_t dirty=uint64_t(random())<<32|random();
  auto native=memory;
  OracleContext ctx;ctx.r1.u32=0x3F000;ctx.r3.u32=device;ctx.r4.u32=stream;
  ctx.r5.u32=ptr;ctx.r6.u32=offset;ctx.r7.u32=stride;ctx.r8.u64=dirty;
  sub_83FBA160(ctx,memory.data());
  if(!WriteStreamSourceShadow({native.data()+device,0x6080},stream,ptr,lo,hi,offset,stride,dirty))return 2;
  if(std::memcmp(native.data()+device,memory.data()+device,0x6080)){std::printf("publication mismatch case %u\n",n);return 3;}
  ctx={};ctx.r1.u32=0x3F000;ctx.r3.u32=device;ctx.r4.u32=stream;ctx.r5.u32=outOffset;ctx.r6.u32=outStride;
  sub_83FBA280(ctx,native.data());
  if(ctx.r3.u32!=ptr||Read32(native.data(),outOffset)!=(ptr?offset:0)||Read32(native.data(),outStride)!=stride)return 4;
 }
 // Reproduce the actual 83EB55A0 save/unbind/restore sequence. The previous
 // native setter left the getter's fields stale and corrupts the restored draw.
 const uint32_t lo=0xF0120000,offset=396,stride=24;
 Write32(memory.data(),buffer+24,lo);Write32(memory.data(),buffer+28,65536);
 std::fill(memory.begin()+device,memory.begin()+device+0x6080,0);
 for(bool old:{false,true}){
  auto state=memory;
  if(old)Write32(state.data(),device+12836,buffer);
  else WriteStreamSourceShadow({state.data()+device,0x6080},0,buffer,lo,65536,offset,stride,1);
  OracleContext ctx;ctx.r1.u32=0x3F000;ctx.r3.u32=device;ctx.r4.u32=0;ctx.r5.u32=outOffset;ctx.r6.u32=outStride;
  sub_83FBA280(ctx,state.data());
  const bool preserved=ctx.r3.u32==buffer&&Read32(state.data(),outOffset)==offset&&Read32(state.data(),outStride)==stride;
  if(preserved==old)return 5;
 }
 std::puts("PASS TU23 stream setter/getter: 100000 cases; save/restore regression and old-setter negative control");
}
'''
source=out/'stream-source-shadow.cpp';source.write_text(code);exe=out/'stream-source-shadow.exe'
subprocess.run([a.compiler,'-std=c++20','-O2','-I'+str(root/'rexlego/src'),source,'-o',exe],check=True,timeout=60)
subprocess.run([exe],check=True,timeout=30)
