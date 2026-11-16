"""Verify TU23 distance layouts, sentinels, baseline identity and re-loads."""
import argparse
from pathlib import Path
import re,subprocess

p=argparse.ArgumentParser(description=__doc__);p.add_argument('output',type=Path)
p.add_argument('--compiler',default='clang++');a=p.parse_args()
root=Path(__file__).resolve().parents[2];out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
production=(root/'rexlego/src/scene_distance.cpp').read_text()
helpers=production[production.index('namespace {'):production.index('\nREX_HOOK_RAW(sub_82C6F428)')]
code=r'''
#include "scene_distance.h"
#include <array>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <random>
using u8=uint8_t;using u32=uint32_t;
#define CHECK(c) do{if(!(c)){std::fprintf(stderr,"Failed %s\n",#c);return 23;}}while(0)
''' + helpers + r'''
int main(){using namespace legodimensions;
 std::array<u8,96> initial{},data{};std::mt19937 random(0x82C6F428);
 for(unsigned i=0;i<100000;++i){
  for(auto& b:initial)b=uint8_t(random());data=initial;
  CHECK(!ScaleSceneInstanceDistances(data,1));CHECK(data==initial);
  CHECK(!ScaleLevelFarClip(data,1));CHECK(data==initial);
 }
 initial.fill(0xAB);data=initial;
 for(size_t at=12;at<32;at+=4)WriteSceneWord(data,at,std::bit_cast<u32>(float(100+at)));
 initial=data;CHECK(ScaleSceneInstanceDistances(data,1.5));
 for(size_t at=0;at<96;++at)if(at<12||at>=32)CHECK(data[at]==initial[at]);
 for(size_t at=12;at<32;at+=4)CHECK(std::bit_cast<float>(ReadSceneWord(data,at))==float(100+at)*1.5f);
 for(u32 bits:{0u,0x80000000u,0xBF800000u,0x7F800000u,0x7FC01234u,0x7F7FFFFFu}){
  for(size_t at=12;at<32;at+=4)WriteSceneWord(data,at,bits);
  initial=data;CHECK(!ScaleSceneInstanceDistances(data,2));CHECK(data==initial);
 }
 CHECK(SceneDistanceScale(-5)==0.5);CHECK(SceneDistanceScale(8)==2);
 CHECK(SceneDistanceScale(std::numeric_limits<double>::quiet_NaN())==1);
 CHECK(SceneDistanceScale(std::numeric_limits<double>::infinity())==1);
 CHECK(!ScaleSceneInstanceDistances({data.data(),31},2));CHECK(!ScaleLevelFarClip({data.data(),9},2));
 for(u32 value:{0u,10u,100u,1500u,5000u,9000u,65535u}){
  data.fill(0xAB);data[8]=u8(value>>8);data[9]=u8(value);initial=data;
  ScaleLevelFarClip(data,1.5);
  const u32 result=u32(data[8])<<8|data[9];
  CHECK(result==(value?u32(std::min(9000.0,std::round(value*1.5))):0));
  for(size_t i=0;i<data.size();++i)if(i!=8&&i!=9)CHECK(data[i]==initial[i]);
 }
 // Actual production transaction preserves originals through repeated loads,
 // changing source values and callers that externally rewrite a record.
 std::vector<u8> guest(512);constexpr u32 address=128;
 for(size_t at=12;at<32;at+=4)WriteSceneWord({guest.data()+address,32},at,std::bit_cast<u32>(100.f));
 for(int n=0;n<8;++n){
  Restore(instance_distances,address,guest.data());
  CHECK(std::bit_cast<float>(ReadSceneWord({guest.data()+address,32},12))==100.f);
  AppliedDistance r;r.size=32;std::memcpy(r.baseline.data(),guest.data()+address,32);
  CHECK(ScaleSceneInstanceDistances({guest.data()+address,32},1.5));
  std::memcpy(r.scaled.data(),guest.data()+address,32);Remember(instance_distances,address,r);
  CHECK(std::bit_cast<float>(ReadSceneWord({guest.data()+address,32},12))==150.f);
 }
 WriteSceneWord({guest.data()+address,32},12,std::bit_cast<u32>(200.f));
 Restore(instance_distances,address,guest.data());
 CHECK(std::bit_cast<float>(ReadSceneWord({guest.data()+address,32},12))==200.f);
 AppliedDistance r;r.size=10;
 for(u32 i=1;i<2000;++i)Remember(level_distances,i,r);
 CHECK(level_distances.size()==1024);
 std::puts("PASS TU23 distance fields: 100000 baseline cases; endian/limits/sentinels/reloads/bounded bookkeeping");
}
'''
source=out/'scene-distance.cpp';source.write_text(code);exe=out/'scene-distance.exe'
subprocess.run([a.compiler,'-std=c++20','-O2','-I'+str(root/'rexlego/src'),str(source),'-o',str(exe)],check=True,timeout=45)
subprocess.run([str(exe)],check=True,timeout=10)
# Check the recorded property layouts against the generated serialization code.
directory=root/'rexlego/generated/default'
if directory.exists():
    def find(name):
        for path in directory.glob('*.cpp'):
            text=path.read_text();start=text.find('DEFINE_REX_FUNC('+name+') {')
            if start<0:continue
            end=text.find('\nDEFINE_REX_FUNC(',start+1)
            return text[start:end if end>=0 else len(text)]
        raise ValueError('Missing original '+name)
    instance=find('sub_82C6F428');far=find('sub_839EFD70')
    assert 'ctx.r5.s64 = ctx.r9.s64 + 1444;' in instance
    assert 'ctx.r4.s64 = ctx.r24.s64 + 12;' in instance
    assert 'ctx.r4.s64 = ctx.r24.s64 + 16;' in instance
    assert 'ctx.r6.s64 = 4;' in instance
    assert 'ctx.r3.s32, 10' in far and 'ctx.r3.s32, 9000' in far
    assert 'REX_STORE_U16(ctx.r11.u32 + 8,' in far
    print('PASS actual generated NuSceneInstance field and level far-clip setter layout checks')
