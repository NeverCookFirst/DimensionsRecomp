"""Run the production copy/writeback fixture with shipped SDK layout/header ABI."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

def body(text,signature):
    start=text.index(signature);end=text.index('{',start)+1;depth=1
    while depth:
        depth+=(text[end]=='{')-(text[end]=='}');end+=1
    return text[start:end]

def replace(text,old,new):
    if text.count(old)!=1:raise RuntimeError('Fixture anchor changed: '+old[:90])
    return text.replace(old,new,1)

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('output',type=Path);p.add_argument('--plume-lib',required=True,type=Path)
p.add_argument('--compiler',default='clang-cl');p.add_argument('--runtime-dir',type=Path)
p.add_argument('--float-formats',action='store_true',help='Exercise the six native floating storage formats')
p.add_argument('--run-timeout',type=int,default=90);a=p.parse_args()
out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
root=Path(__file__).resolve().parents[2];package=root.parent
sdk=package/'dev-assets/sdk-install'
native=(a.runtime_dir or package/'build/native').resolve()
textures=(root/'rexlego/src/gpu_native/textures.cpp').read_text()
hooks=(root/'rexlego/src/gpu_native/hooks_buffer.cpp').read_text()
production='\n'.join(body(textures,s) for s in [
    'bool TexturePlacementHeaderMatches(','TextureReadbackFormat ResolvedTextureReadbackFormat(', 'void RecordResolvedTexture(',
    'std::array<PhysicalCopyRange, 2> TextureDataRanges(',
    'bool TextureDataOverlapsPoolCopy(','bool TextureDataCoveredByPoolCopy(',
    'bool PoolCopyGpuSourceIndexed(','bool PoolCopyGpuDestinationIndexed(',
    'bool PrepareResolvedTextureWriteback(', 'bool EncodeResolvedTextureWriteback(',
    'void PublishResolvedTextureWriteback(', 'bool MaterializeResolvedTextures(',
    'bool MaterializeResolvedTexture(const std::shared_ptr<TextureResource>& resource) {',
    'bool MaterializePoolCopyTextureDependencies(',
    'bool MaterializePoolCopyTextureSource(','bool MaterializePoolCopyTextureDestination(',
    'template<class Iterator>\nbool InvalidatePoolCopyTexture(',
    'void RetirePoolCopyTextures(','u32 InvalidatePoolCopyTexturesIndexed('])
production+='\n'+body(hooks,'void MergeRanges(')
production+='\n'+body(hooks,'void FlushPoolCopyBatch(').replace(
    'void FlushPoolCopyBatch(','void legodimensions::gpu_native::FlushPoolCopyBatch(',1)
production+='\n'+body(hooks,'void BeginPoolCopyBatch(')+'\n'+body(hooks,'void PoolCopyRecorded(')
production=body(textures,'struct ResolvedTextureWriteback {')+';\n'+production
(out/'partial-destination-production.inc').write_text(production)
cpp=Path(__file__).with_name('test_partial_destination_gpu.cpp').read_text()
cpp=replace(cpp,'#include "gpu_native/resolve_region.h"',
    '#include "gpu_native/resolve_region.h"\n#include "gpu_native/d3d.h"\n#include <rex/graphics/pipeline/texture/util.h>')
start=cpp.index('namespace rex {constexpr u32 align(')
end=cpp.index('struct TextureResource {',start)
cpp=cpp[:start]+'using Fetch=rex::graphics::xenos::xe_gpu_texture_fetch_t;\n'+cpp[end:]
cpp=replace(cpp,'u32 guest_address=0x80001000,','u32 adopted_header_type=3;\n u32 guest_address=0x8000,')
cpp=replace(cpp,'struct D3DTexture {u8 data[52];};','')
cpp=replace(cpp,'bool TexturePlacementHeaderMatches(const TextureResource&){return true;}',
    'bool ReadableUploadSpan(u32 at,u32 n){return u64(at)+n<=memory.data.size();}\n'
    'template<class T>const T* GuestAt(u32 at){return reinterpret_cast<const T*>(memory.data.data()+at);}')
cpp=replace(cpp,'bool cpu_write_during_wait=false;', 'bool header_race=false;bool cpu_write_during_wait=false;')
cpp=replace(cpp,'if(cpu_write_during_wait)memory.data[cpu_write_address]^=0x80;',
    'if(cpu_write_during_wait)memory.data[cpu_write_address]^=0x80;'
    'if(header_race){auto* h=reinterpret_cast<D3DTexture*>(memory.data.data()+0x8000);'
    'h->format.dword[2]=u32(h->format.dword[2])^1;}')
cpp=replace(cpp,'int main(){', 'int main(int argc,char**){\n header_race=argc>1;')
cpp=replace(cpp,' for(u32 raw_format=0;',
    ' for(auto shape:{std::array<u32,3>{64,32,3},std::array<u32,3>{17,13,2},'
    'std::array<u32,3>{2,2,2},std::array<u32,3>{67,33,3}})\n for(u32 raw_format=0;')
cpp=replace(cpp,'  if(!raw_format && packing)continue;',
    '  if(!raw_format && packing)continue;\n'
    '  if(scale==3 && ((shape[0]&1)||(shape[1]&1)))continue;')
cpp=replace(cpp,'  resource->scale_twice=scale;',
    '  resource->width=shape[0];resource->height=shape[1];resource->scale_twice=scale;\n'
    '  f.dimension=rex::graphics::xenos::DataDimension::k2DOrStacked;'
    'f.base_address=32;f.pitch=shape[2];f.packed_mips=shape[0]<=17;'
    'f.format=rex::graphics::xenos::TextureFormat::k_8_8_8_8;'
    'f.type=rex::graphics::xenos::FetchConstantType::kTexture;'
    'f.size_2d.width=shape[0]-1;f.size_2d.height=shape[1]-1;')
cpp=replace(cpp,'f.endianness=endian;', 'f.endianness=rex::graphics::xenos::Endian(endian);')
cpp=replace(cpp,'f.exp_adjust=packing?3:0;', 'f.exp_adjust=packing?-13:5;')
start=cpp.index('  const u32 w=resource->width,h=resource->height,pitch=96*texel_bytes,')
end=cpp.index('  memory.data.assign(',start)
cpp=cpp[:start]+'''  const u32 w=resource->width,h=resource->height,base=f.base_address<<12;
  const auto layout=rex::graphics::texture_util::GetGuestTextureLayout(f.dimension,f.pitch,w,h,1,f.tiled,f.format,f.packed_mips,true,0);
  const u32 pitch=layout.base.row_pitch_bytes,extent=layout.base.level_data_extent_bytes;
  const u32 rounded_extent=(extent+63)&~63u;
  if(!whole && extent<=64)continue;
  u32 ox=0,oy=0,oz=0;
  if(f.packed_mips)rex::graphics::texture_util::GetPackedMipOffset(w,h,1,f.format,0,ox,oy,oz);
  auto offset=[&](u32 x,u32 y)->u32{return tiled?
    rex::graphics::texture_util::GetTiledOffset2D(x+ox,y+oy,pitch/texel_bytes,texel_bytes==8?3:2):
    (y+oy)*pitch+(x+ox)*texel_bytes;};
''' +cpp[end:]
cpp=replace(cpp,'for(u32 i=0;i<extent;++i)memory.data[4096+i]',
    'for(u32 i=0;i<rounded_extent;++i)memory.data[4096+i]')
cpp=replace(cpp,'  auto expected=memory.data;',
    '  auto* guest_header=reinterpret_cast<D3DTexture*>(memory.data.data()+resource->guest_address);\n'
    '  guest_header->resource.common=resource->adopted_header_type;\n'
    '  std::array<u32,6> words{};std::memcpy(words.data(),&f,sizeof(f));\n'
    '  for(u32 i=0;i<6;++i)guest_header->format.dword[i]=words[i];\n'
    '  CHECK(TexturePlacementHeaderMatches(*resource));\n'
    '  guest_header->format.dword[2]=words[2]^1;CHECK(!TexturePlacementHeaderMatches(*resource));\n'
    '  guest_header->format.dword[2]=words[2];\n'
    '  auto expected=memory.data;')
cpp=replace(cpp,'top.height=resource->HostHeight()/2;', 'top.height=resource->HostHeight()/2;')
cpp=replace(cpp,'auto bottom=top;bottom.y=top.height;',
    'auto bottom=top;bottom.y=top.height;bottom.height=resource->HostHeight()-top.height;')
cpp=replace(cpp,'const u32 destination=whole?base:base+4096,bytes=whole?extent:4096;',
    'const u32 destination=base,bytes=whole?rounded_extent:64;')
cpp=replace(cpp,'cpu_write_address=base;', 'cpu_write_address=base+offset(w-1,h-1);')
cpp=replace(cpp,'expected[base]=0x55^0x80;', 'expected[cpu_write_address]=0x55^0x80;')
cpp=replace(cpp,'  PoolCopyRecorded(destination,4096,bytes,',
    '  const auto before_copy=memory.data;\n  PoolCopyRecorded(destination,4096,bytes,')
cpp=replace(cpp,'  FlushPoolCopyBatch();',
    '  FlushPoolCopyBatch();\n'
    '  if(header_race){CHECK(!whole && readbacks==before+1);'
    'CHECK(resource->resolved_on_host && !resource->guest_backing_current);'
    'CHECK(!TexturePlacementHeaderMatches(*resource));'
    'CHECK(std::equal(memory.data.begin()+base,memory.data.begin()+base+extent,before_copy.begin()+base));'
    'std::puts("PASS real BE placement-header change during GPU wait rejects stale publication");return 0;}')
cpp=replace(cpp,'CHECK(!resource->resolved_on_host&&!resource->sampled_valid);',
    'if(resource->resolved_on_host||resource->sampled_valid)std::fprintf(stderr,'
    '"case%u %ux%u format%u tiled%u whole%u partial%u cpu%u extent%u range%u/%u resolved%u sampled%u current%u\\n",'
    'cases,w,h,raw_format,tiled,whole,partial_resolve,later_cpu_write,extent,'
    'TextureDataRanges(*resource)[0].address,TextureDataRanges(*resource)[0].length,'
    'resource->resolved_on_host,resource->sampled_valid,resource->guest_backing_current);'
    'CHECK(!resource->resolved_on_host&&!resource->sampled_valid);')
cpp=replace(cpp,'PASS production GPU destination preservation:',
    'PASS shipped SDK layout/header GPU destination preservation:')
if a.float_formats:
    cpp=replace(cpp,'for(u32 raw_format=0;raw_format<3;++raw_format)',
        'for(u32 raw_format=3;raw_format<9;++raw_format)')
    cpp=replace(cpp,'if(!raw_format && packing)continue;', 'if(packing)continue;')
    anchor='  const u32 texel_bytes=raw_format==2?8:4;'
    cpp=replace(cpp,anchor,'''  const plume::RenderFormat hosts[]={plume::RenderFormat::R16_FLOAT,
    plume::RenderFormat::R16G16_FLOAT,plume::RenderFormat::R16G16B16A16_FLOAT,
    plume::RenderFormat::R32_FLOAT,plume::RenderFormat::R32G32_FLOAT,plume::RenderFormat::R32G32B32A32_FLOAT};
  const rex::graphics::xenos::TextureFormat guests[]={rex::graphics::xenos::TextureFormat::k_16_FLOAT,
    rex::graphics::xenos::TextureFormat::k_16_16_FLOAT,rex::graphics::xenos::TextureFormat::k_16_16_16_16_FLOAT,
    rex::graphics::xenos::TextureFormat::k_32_FLOAT,rex::graphics::xenos::TextureFormat::k_32_32_FLOAT,
    rex::graphics::xenos::TextureFormat::k_32_32_32_32_FLOAT};
  resource->format=hosts[raw_format-3];f.format=guests[raw_format-3];f.num_format=1;
  f.sign_x=f.sign_y=f.sign_z=f.sign_w=rex::graphics::xenos::TextureSign::kSigned;
  constexpr u32 sizes[]={2,4,8,4,8,16};const u32 texel_bytes=sizes[raw_format-3];''')
    cpp=replace(cpp,'pitch/texel_bytes,texel_bytes==8?3:2)',
        'pitch/texel_bytes,texel_bytes==16?4:texel_bytes==8?3:texel_bytes==4?2:1)')
    cpp=replace(cpp,'const u8 value=u8(sx*17+sy*29+c*61);',
        'const u8 value=FloatByte(raw_format,sx,sy,c);')
    cpp=replace(cpp,'=u8(x*17+y*29+c*61);', '=FloatByte(raw_format,x,y,c);')
    cpp=replace(cpp,'int main(int argc,char**){', '''u8 FloatByte(u32 format,u32 x,u32 y,u32 c){
 constexpr u32 singles[]={0,0x80000000,1,0x007fffff,0x00800000,0x3f800000,0xbf800000,
  0x7f7fffff,0xff7fffff,0x7f800000,0xff800000,0x7fc12345,0x7f812345,0xffffffff};
 constexpr u32 halves[]={0,0x8000,1,0x03ff,0x0400,0x3c00,0xbc00,0x7bff,0xfbff,0x7c00,0xfc00,0x7e12,0x7c12,0xffff};
 const bool single=format>=6;const u32 word=single?singles[(x+y*67+c/4)%14]:halves[(x+y*67+c/2)%14];
 return u8(word>>(8*(c&(single?3:1))));
}
int main(int argc,char**){''')
    cpp=replace(cpp,'RGBA8/RG16/RGBA16 raw bits,', 'six floating formats with special raw bits,')
(out/'sdk-layout.cpp').write_text(cpp)
exe=out/'sdk-layout.exe'
cmd=[a.compiler,str(out/'sdk-layout.cpp'),'/std:c++latest','/O2','/MD','/EHsc',
    '/I'+str(out),'/I'+str(root/'rexlego/src'),'/I'+str(sdk/'include'),
    '/I'+str(root/'thirdparty/plume'),
    '/I'+str(root/'thirdparty/plume/contrib/D3D12MemoryAllocator/include'),
    '/Fo'+str(out/'sdk-layout.obj'),'/Fe'+str(exe),'/link',str(a.plume_lib.resolve()),
    str(sdk/'lib/rexruntime.lib'),'d3d12.lib','dxgi.lib','dxguid.lib','user32.lib']
r=subprocess.run(cmd,capture_output=True,text=True,timeout=120)
(out/'compile.log').write_text(r.stdout+r.stderr);r.check_returncode()
records=[]
for cache in ['0','1']:
    env=dict(os.environ,PATH=str(native)+os.pathsep+os.environ['PATH'],PLUME_D3D12_STATE_CACHE=cache)
    r=subprocess.run([str(exe)],env=env,capture_output=True,text=True,timeout=a.run_timeout)
    (out/('positive-cache-'+cache+'.log')).write_text(r.stdout+r.stderr)
    r.check_returncode();records.append(dict(cache=cache,exit=r.returncode,stdout=r.stdout.strip()))
env=dict(os.environ,PATH=str(native)+os.pathsep+os.environ['PATH'],PLUME_D3D12_STATE_CACHE='1')
race=subprocess.run([str(exe),'header-race'],env=env,capture_output=True,text=True,timeout=30)
(out/'header-race.log').write_text(race.stdout+race.stderr);race.check_returncode()
guard='if (!TexturePlacementHeaderMatches(*resource) || !PoolCopyRangeReadable(backing)) return false;'
mutant=replace(production,guard,'if (!PoolCopyRangeReadable(backing)) return false;')
(out/'partial-destination-production.inc').write_text(mutant)
mutant_exe=out/'missing-postwait-header-check.exe'
mutant_cmd=[('/Fe'+str(mutant_exe)) if item=='/Fe'+str(exe) else item for item in cmd]
r=subprocess.run(mutant_cmd,capture_output=True,text=True,timeout=120)
(out/'mutant-compile.log').write_text(r.stdout+r.stderr);r.check_returncode()
negative=subprocess.run([str(mutant_exe),'header-race'],env=env,capture_output=True,text=True,timeout=30)
(out/'header-race-negative.log').write_text(negative.stdout+negative.stderr)
if negative.returncode!=23:raise RuntimeError('Omitting post-wait header validation escaped the GPU race check')
float_tiling_negative=None
if a.float_formats:
    mutant=replace(production,'TextureReadbackTexelLog2(job.format)','2')
    (out/'partial-destination-production.inc').write_text(mutant)
    tiling_exe=out/'wrong-float-texel-tiling.exe'
    tiling_cmd=[('/Fe'+str(tiling_exe)) if item=='/Fe'+str(exe) else item for item in cmd]
    r=subprocess.run(tiling_cmd,capture_output=True,text=True,timeout=120)
    (out/'tiling-mutant-compile.log').write_text(r.stdout+r.stderr);r.check_returncode()
    r=subprocess.run([str(tiling_exe)],env=env,capture_output=True,text=True,timeout=a.run_timeout)
    (out/'tiling-negative.log').write_text(r.stdout+r.stderr)
    if r.returncode!=23:raise RuntimeError('Wrong float tiled texel width escaped the GPU comparison')
    float_tiling_negative=r.returncode
(out/'partial-destination-production.inc').write_text(production)
(out/'verification.json').write_text(json.dumps(dict(game_launched=False,window_opened=False,
    shipped_layout_functions=True,actual_guest_header=True,actual_fetch_constant=True,
    actual_production_copy_and_materialization=True,actual_gpu=True,float_formats=a.float_formats,
    float_tiling_negative_exit=float_tiling_negative,
    runtime=str(native/'rexruntime.dll'),runtime_sha256=hashlib.sha256((native/'rexruntime.dll').read_bytes()).hexdigest(),
    cases=records,header_race_exit=race.returncode,header_race_negative_exit=negative.returncode,
    header_race_stdout=race.stdout.strip(),
    scope='controlled guest memory/access/registry, real shipped layout ABI and BE header matching'),indent=2)+'\n')
for record in records:print(record['stdout'])
print(race.stdout.strip())
print('PASS negative control: omitted post-wait placement check permits stale publication')
