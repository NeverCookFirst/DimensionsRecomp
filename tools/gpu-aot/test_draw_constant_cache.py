"""Differential constant upload reuse, immutable storage and recording lifetime."""
import argparse
from pathlib import Path
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('output', type=Path)
p.add_argument('--compiler', default='clang++')
p.add_argument('--captured-packets', type=Path)
p.add_argument('--negative-control', action='store_true')
a = p.parse_args()
root = Path(__file__).resolve().parents[2]
out = a.output.resolve(); out.mkdir(parents=True, exist_ok=True)
header = (root/'rexlego/src/gpu_native/draw_constant_cache.h').read_text()
if a.negative_control:
    header = header.replace('!serial || serial != serial_', '!serial')
(out/'draw_constant_cache.h').write_text(header)
source = (root/'rexlego/src/gpu_native/draw.cpp').read_text()
bind = source[source.index('bool BindConstants('):source.index('\nplume::RenderPipeline* GetPipeline(')]
assert 'cache.Resolve(recording_serial, data,' in bind
assert bind.index('PrepareTextureResource(bindings.textures[i]') < bind.index('cache.Resolve(')
assert 'if (temporal_snapshot)' in bind and 'SaveLongProbeConstants(' in bind
assert 'LEGO_NATIVE_NO_CONSTANT_CACHE' in bind
assert bind.count('BoundShaderTextureMask(ShaderStage::kVertex)') == 1
assert bind.count('BoundShaderTextureMask(ShaderStage::kPixel)') == 1
assert 'draw_device.recording_serial,' in source
cpp = out/'constants.cpp'
cpp.write_text(r'''
#include "draw_constant_cache.h"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <random>
#include <vector>
#include <deque>
#include <chrono>
#ifdef _WIN32
#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <immintrin.h>
#pragma comment(lib,"d3d12.lib")
#endif
using namespace legodimensions::gpu_native;
#define CHECK(v) do {if(!(v)){std::fprintf(stderr,"failed line %d: %s\n",__LINE__,#v);std::exit(23);}}while(0)
struct Slice {void* buffer=nullptr;void* mapped=nullptr;uint64_t offset=0;
 explicit operator bool()const{return buffer&&mapped;}};
using Cache=DrawConstantCache<Slice>;
using Packet=std::array<uint8_t,Cache::packet_size>;
struct Arena {
 std::vector<uint8_t> memory=std::vector<uint8_t>(64*1024*1024,0xCA);
 uint64_t used=0,calls=0;bool fail=false;
 Slice Allocate(uint32_t bytes){++calls;if(fail)return {};bytes=Cache::Align(bytes);CHECK(used+bytes<=memory.size());
  Slice result{this,memory.data()+used,used};used+=bytes;return result;}
};
void Verify(const Packet& packet,const std::array<Slice,3>& slices){
 unsigned at=0;for(unsigned i=0;i<3;++i){CHECK(slices[i]);CHECK(slices[i].offset%256==0);
  CHECK(std::memcmp(slices[i].mapped,packet.data()+at,Cache::sizes[i])==0);at+=Cache::sizes[i];}
}
int main(int argc,char** argv){
 Arena arena;Cache cache;Packet packet{};std::array<Slice,3> slices;
 auto allocate=[&](uint32_t n){return arena.Allocate(n);};
 CHECK(cache.Resolve(1,packet,allocate,slices));Verify(packet,slices);
 CHECK(arena.used==8960&&arena.calls==1);auto original=slices;
 CHECK(cache.Resolve(1,packet,allocate,slices));CHECK(arena.calls==1);
 for(unsigned i=0;i<3;++i)CHECK(slices[i].mapped==original[i].mapped);
 packet[8192]=4;CHECK(cache.Resolve(1,packet,allocate,slices));Verify(packet,slices);
 CHECK(arena.used==9728&&arena.calls==2);CHECK(*static_cast<uint8_t*>(original[2].mapped)==0);
 packet[4096]=7;arena.fail=true;
 CHECK(!cache.Resolve(1,packet,allocate,slices));arena.fail=false;
 CHECK(cache.Resolve(1,packet,allocate,slices));Verify(packet,slices);CHECK(arena.used==13824);
 const auto calls=arena.calls;CHECK(!cache.Resolve(1,std::span(packet).first(1),allocate,slices));CHECK(arena.calls==calls);
 // Physical arena and command-list pointer can recur after GPU completion.
 // A new serial must upload all bytes even if CPU constants did not change.
 arena.used=0;std::fill(arena.memory.begin(),arena.memory.end(),0xED);
 CHECK(cache.Resolve(2,packet,allocate,slices));Verify(packet,slices);CHECK(arena.used==8960);
 const auto before=arena.used;
 CHECK(cache.Resolve(0,packet,allocate,slices));CHECK(cache.Resolve(0,packet,allocate,slices));
 CHECK(arena.used==before+2*8960);Verify(packet,slices);
 std::mt19937 random(0x83FB9F10);std::deque<std::pair<Packet,std::array<Slice,3>>> retained;
 uint64_t serial=3;
 for(unsigned draw=0;draw<100000;++draw){
  if(draw%1000==0){++serial;arena.used=0;retained.clear();}
  if(draw%9){unsigned block=random()%3;unsigned begin=block==0?0:block==1?4096:8192;
   for(unsigned j=0;j<4;++j)packet[begin+random()%Cache::sizes[block]]=uint8_t(random());}
  CHECK(cache.Resolve(serial,packet,allocate,slices));Verify(packet,slices);
  for(const auto& saved:retained)Verify(saved.first,saved.second);
  retained.emplace_back(packet,slices);if(retained.size()>8)retained.pop_front();
 }
 if(argc>1){
  std::ifstream input(argv[1],std::ios::binary);CHECK(input.good());
  std::vector<Packet> packets;Packet data;
  while(input.read(reinterpret_cast<char*>(data.data()),data.size()))packets.push_back(data);
  CHECK(!packets.empty());
  uint64_t needed=0,allocations=0;
  // Recording boundaries deliberately restrict reuse; never bridge a submit.
  for(unsigned span:{1u,17u,100u,100000u}){
   Cache captured;arena.used=arena.calls=0;
   for(unsigned i=0;i<packets.size();++i){CHECK(captured.Resolve(100+i/span,packets[i],allocate,slices));Verify(packets[i],slices);}
   std::printf("captured draws=%zu recording_span=%u reserved=%llu baseline=%llu allocations=%llu\n",
      packets.size(),span,(unsigned long long)arena.used,(unsigned long long)(packets.size()*8960),(unsigned long long)arena.calls);
   if(span==1)CHECK(arena.used==packets.size()*8960);
  }
  // Isolate CPU cost on the same retained packet sequence. This is not FPS.
  volatile uint64_t sum=0;
  for(bool reused:{false,true}){
   auto start=std::chrono::steady_clock::now();uint64_t total_bytes=0;
   for(unsigned repeat=0;repeat<100;++repeat){
    Cache captured;arena.used=0;
    for(const auto& data:packets){
     if(reused)CHECK(captured.Resolve(repeat+1,data,allocate,slices));
     else{auto s=allocate(8816);std::memcpy(s.mapped,data.data(),data.size());}
    }
    total_bytes+=arena.used;sum=sum+arena.memory[arena.used-256];
   }
   double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
   std::printf("cpu packet replay reuse=%d elapsed_ms=%.3f reserved_bytes=%llu checksum=%llu\n",reused,ms,
      (unsigned long long)total_bytes,(unsigned long long)sum);
  }
 }

#ifdef _WIN32
 if(argc>1){
  using Microsoft::WRL::ComPtr;
  ComPtr<ID3D12Device> device;CHECK(SUCCEEDED(D3D12CreateDevice(nullptr,D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device))));
  D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_UPLOAD;
  D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;desc.Width=64*1024*1024;
  desc.Height=1;desc.DepthOrArraySize=1;desc.MipLevels=1;desc.SampleDesc.Count=1;desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  ComPtr<ID3D12Resource> buffer;CHECK(SUCCEEDED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&buffer))));
  void* mapped=nullptr;D3D12_RANGE read{0,0};CHECK(SUCCEEDED(buffer->Map(0,&read,&mapped)));
  std::memset(mapped,0,desc.Width);_mm_sfence();
  std::ifstream input(argv[1],std::ios::binary);std::vector<Packet> packets;Packet data;
  while(input.read(reinterpret_cast<char*>(data.data()),data.size()))packets.push_back(data);
  for(unsigned trial=0;trial<6;++trial)for(bool reused:{bool(trial%2),!bool(trial%2)}){
   auto start=std::chrono::steady_clock::now();uint64_t total=0;
   for(unsigned repeat=0;repeat<100;++repeat){
    Cache captured;uint64_t used=0;
    auto wc_allocate=[&](uint32_t n){auto offset=used;used+=Cache::Align(n);CHECK(used<=desc.Width);
       return Slice{buffer.Get(),static_cast<uint8_t*>(mapped)+offset,offset};};
    for(const auto& packet:packets){
     if(reused)CHECK(captured.Resolve(repeat+1,packet,wc_allocate,slices));
     else{auto slice=wc_allocate(packet.size());std::memcpy(slice.mapped,packet.data(),packet.size());}
    }
    total+=used;_mm_sfence();
   }
   double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
   std::printf("D3D12 UPLOAD cpu replay trial=%u reuse=%d elapsed_ms=%.3f reserved_bytes=%llu\n",trial,reused,ms,(unsigned long long)total);
  }
  buffer->Unmap(0,nullptr);
 }
#endif
 std::puts("PASS exact packet bytes, bounded shadow, immutable earlier CBVs, failure retry, arena reset and serial-zero bypass");
}
''')
exe = out/'constants.exe'
subprocess.run([a.compiler, '-std=c++20', '-O2', str(cpp), '-o', str(exe)], check=True, timeout=45)
subprocess.run([str(exe), *([str(a.captured_packets)] if a.captured_packets else [])], check=True, timeout=20)
