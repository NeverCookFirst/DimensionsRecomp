"""Check bounded placement capture identity using the production capture body."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess


def function(source, signature):
    start = source.index(signature)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    parser.add_argument('--compiler', default='clang++')
    parser.add_argument('--verify-regression', action='store_true')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    source = (root / 'rexlego/src/gpu_native/shaders.cpp').read_text()
    body = function(source, 'void CaptureMissingPlacementShader(')
    adopt = function(source, 'std::shared_ptr<ShaderResource> AdoptShader(')
    assert 'CaptureMissingPlacementShader(guest_address, container_address, microcode,' in adopt
    assert adopt.index('if (!cache_entry)') < adopt.index('CaptureMissingPlacementShader(')
    d3d = (root / 'rexlego/src/gpu_native/d3d.h').read_text()
    container = function(d3d, 'struct ShaderContainer') + ';'
    fixture = r'''
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>
#define XXH_INLINE_ALL
#include <xxhash.h>
using u32=uint32_t;using u64=uint64_t;
struct be_u32 {
 uint8_t bytes[4]{};
 operator u32()const{return (u32(bytes[0])<<24)|(u32(bytes[1])<<16)|(u32(bytes[2])<<8)|bytes[3];}
 be_u32& operator=(u32 value){for(int i=0;i<4;++i)bytes[i]=uint8_t(value>>(24-i*8));return *this;}
};
''' + container + r'''
static_assert(sizeof(ShaderContainer)==36);
enum class ShaderStage{ kVertex=6,kPixel=7 };
constexpr u32 kVertexShaderGuestSize=872,kPixelShaderGuestSize=40;
''' + function(source, 'size_t StageIndex(') + r'''
int _dupenv_s(char** value,size_t* length,const char* name){
 const char* env=std::getenv(name);*value=nullptr;*length=0;
 if(env){*length=std::strlen(env)+1;*value=static_cast<char*>(std::malloc(*length));std::memcpy(*value,env,*length);}
 return 0;
}
constexpr u32 guest=0x1000,physical=0x10000;
std::vector<uint8_t> memory_bytes(0x20000);
std::atomic<u32> range_calls=0,translations=0,hash_calls=0;
u32 header_begin=guest+kPixelShaderGuestSize,header_readable=64,physical_readable=32;
bool force_hash=false;
bool mutate_after_container_hash=false;
struct Memory {
 template<class T>T TranslateVirtual(u32 at){++translations;assert(at<memory_bytes.size());return reinterpret_cast<T>(memory_bytes.data()+at);}
} fixture_memory;
#define REX_KERNEL_MEMORY() (&fixture_memory)
bool ReadableGuestRange(u32 at,u32 size){
 ++range_calls;
 const auto contained=[&](u32 start,u32 length){return at>=start&&u64(at)+size<=u64(start)+length;};
 return size&&(contained(header_begin,header_readable)||contained(physical,physical_readable));
}
u64 RealHash(const void* data,size_t length){return XXH3_64bits(data,length);}
u64 CountedHash(const void* data,size_t length){
 ++hash_calls;assert(length<=65536*2+4);const u64 result=force_hash?7:RealHash(data,length);
 if(mutate_after_container_hash&&length==100){memory_bytes[physical+20]^=0x55;mutate_after_container_hash=false;}
 return result;
}
struct Captured {ShaderStage stage;std::string category;u64 hash;std::vector<uint8_t> bytes;};
struct Identity {u32 stage,guest,physical,virtual_bytes,physical_bytes;u64 container_hash,physical_hash;};
std::mutex output_mutex;
std::vector<Captured> captures;
std::vector<Identity> identities;
bool existing_files=false;
u32 dump_calls=0;
void LongProbeEvent(const char* event,bool anomaly,const char*,u32 stage,
 const char*,u32 guest,const char*,u32 physical,const char*,u32 virtual_bytes,
 const char*,u32 physical_bytes,const char*,u64 container_hash,const char*,u64 physical_hash){
 assert(std::string(event)=="missing_placement_shader_identity"&&anomaly);
 std::lock_guard lock(output_mutex);
 identities.push_back({stage,guest,physical,virtual_bytes,physical_bytes,container_hash,physical_hash});
}
void DumpMissingShader(const ShaderContainer* bytes,size_t length,ShaderStage stage,u64 hash,const char* category){
 std::lock_guard lock(output_mutex);
 ++dump_calls;if(existing_files)return;
 const auto* begin=reinterpret_cast<const uint8_t*>(bytes);
 captures.push_back({stage,category,hash,{begin,begin+length}});
}
#undef XXH3_64bits
#define XXH3_64bits CountedHash
''' + body + r'''
#undef XXH3_64bits
ShaderContainer& header(){return *reinterpret_cast<ShaderContainer*>(memory_bytes.data()+header_begin);}
void valid(ShaderStage stage=ShaderStage::kPixel,u32 address=guest){
 header_begin=address+(stage==ShaderStage::kVertex?kVertexShaderGuestSize:kPixelShaderGuestSize);
 header_readable=64;physical_readable=32;
 std::fill(memory_bytes.begin()+header_begin,memory_bytes.begin()+header_begin+64,0xAB);
 header().flags=stage==ShaderStage::kVertex?0x102A1101:0x102A1100;
 header().virtual_size=64;header().physical_size=32;
 for(u32 i=0;i<32;++i)memory_bytes[physical+i]=uint8_t(i+1);
}
void capture(ShaderStage stage=ShaderStage::kPixel,u32 address=guest,size_t readable=32){
 CaptureMissingPlacementShader(address,physical,memory_bytes.data()+physical,readable,stage);
}
size_t complete_count(){size_t count=0;for(auto& entry:captures)count+=entry.category=="placement-containers";return count;}
void exact_identity(){
 const auto& out=identities.back();
 assert(out.stage==u32(ShaderStage::kPixel)&&out.guest==guest&&out.physical==physical);
 assert(out.virtual_bytes==64&&out.physical_bytes==32);
 std::vector<uint8_t> expected(memory_bytes.begin()+header_begin,memory_bytes.begin()+header_begin+64);
 expected.insert(expected.end(),memory_bytes.begin()+header_begin+8,memory_bytes.begin()+header_begin+12);
 expected.insert(expected.end(),memory_bytes.begin()+physical,memory_bytes.begin()+physical+32);
 assert(out.container_hash==RealHash(expected.data(),expected.size()));
 assert(out.physical_hash==RealHash(memory_bytes.data()+physical,32));
}
void exact_last(){
 assert(identities.size()==complete_count());exact_identity();
 const auto& out=captures.back();assert(out.category=="placement-containers");
 assert(out.bytes.size()==100);
 assert(std::memcmp(out.bytes.data(),memory_bytes.data()+header_begin,64)==0);
 assert(std::memcmp(out.bytes.data()+64,memory_bytes.data()+header_begin+8,4)==0);
 assert(std::memcmp(out.bytes.data()+68,memory_bytes.data()+physical,32)==0);
 assert(out.hash==RealHash(out.bytes.data(),out.bytes.size()));
 assert(captures[captures.size()-2].category=="placement-raw");
 assert(captures[captures.size()-2].bytes.size()==32);
}
int main(int argc,char** argv){
 assert(argc==2);const std::string mode=argv[1];valid();
 if(mode=="identity"){
  capture();assert(complete_count()==1);exact_last();capture();assert(complete_count()==1);
  memory_bytes[physical+20]^=0x55;capture();
  assert((complete_count()==2)&&"same guest address with changed valid bytes must capture again");exact_last();
  memory_bytes[header_begin+48]^=0x33;capture();assert(complete_count()==3);exact_last();
  const auto copy=std::vector<uint8_t>(memory_bytes.begin()+header_begin,memory_bytes.begin()+header_begin+64);
  header_begin=0x2000+kPixelShaderGuestSize;
  std::memcpy(memory_bytes.data()+header_begin,copy.data(),copy.size());
  capture(ShaderStage::kPixel,0x2000);assert(complete_count()==3);
 }else if(mode=="malformed"){
  header_readable=35;capture();assert(hash_calls==0);valid();
  header().flags=0x102A1110;capture();valid();
  header().flags=0x102A1101;capture();valid();
  header().virtual_size=35;capture();valid();
  header().virtual_size=65537;capture();valid();
  header().virtual_size=65;capture();valid();
  header().physical_size=7;capture();valid();
  header().physical_size=65537;capture();valid();
  capture(ShaderStage::kPixel,guest,31);
  physical_readable=31;capture();valid();
  const auto reads=range_calls.load();capture(ShaderStage::kVertex,0xFFFFFF00u);
  assert(range_calls==reads);assert(hash_calls==0&&captures.empty()&&identities.empty());
  capture();assert(complete_count()==1);exact_last();
 }else if(mode=="budget"){
  for(u32 i=0;i<256;++i){
   const auto stage=i%2?ShaderStage::kVertex:ShaderStage::kPixel;valid(stage);
   std::memcpy(memory_bytes.data()+physical,&i,sizeof(i));capture(stage);
   assert(complete_count()==i+1);
  }
  assert(captures.size()==512&&identities.size()==256);const auto reads=range_calls.load(),hashes=hash_calls.load();
  valid();memory_bytes[physical+20]^=0xCC;capture();
  assert(complete_count()==256&&identities.size()==256&&range_calls==reads&&hash_calls==hashes);
 }else if(mode=="stage"){
  force_hash=true;capture();valid(ShaderStage::kVertex);capture(ShaderStage::kVertex);
  assert(complete_count()==2);capture(ShaderStage::kVertex);assert(complete_count()==2);
  assert(captures[1].stage==ShaderStage::kPixel&&captures[3].stage==ShaderStage::kVertex);
  assert(identities.size()==2&&identities[0].stage==u32(ShaderStage::kPixel)&&identities[1].stage==u32(ShaderStage::kVertex));
 }else if(mode=="concurrent"){
  std::vector<std::thread> workers;for(int i=0;i<8;++i)workers.emplace_back([]{for(int j=0;j<20;++j)capture();});
  for(auto& worker:workers)worker.join();assert(complete_count()==1&&captures.size()==2);exact_last();
 }else if(mode=="disabled"){
  capture();assert(captures.empty()&&identities.empty()&&range_calls==0&&translations==0&&hash_calls==0);
 }else if(mode=="existing-files"){
  existing_files=true;capture();assert(captures.empty()&&dump_calls==2&&identities.size()==1);exact_identity();
  capture();assert(dump_calls==2&&identities.size()==1);
 }else if(mode=="mutating-source"){
  mutate_after_container_hash=true;capture();assert(complete_count()==1&&identities.size()==1);
  const auto& full=captures.back();const auto& identity=identities.back();
  assert(identity.container_hash==RealHash(full.bytes.data(),full.bytes.size()));
  assert(identity.physical_hash==RealHash(full.bytes.data()+68,32));
  assert(identity.physical_hash!=RealHash(memory_bytes.data()+physical,32));
 }else assert(false);
}
'''
    args.output.mkdir(parents=True, exist_ok=True)
    executable_suffix = '.exe' if os.name == 'nt' else ''

    def build(name, text):
        cpp = args.output / (name + '.cpp')
        executable = args.output / (name + executable_suffix)
        cpp.write_text(text)
        subprocess.run([args.compiler, '-std=c++20', '-pthread', '-UNDEBUG', '-I',
                        str(root / 'rexglue-sdk/thirdparty/xxHash'), str(cpp), '-o',
                        str(executable)], check=True, timeout=45)
        return executable.resolve()

    executable = build('capture', fixture)
    modes = ['identity', 'malformed', 'budget', 'stage', 'concurrent', 'disabled',
             'existing-files', 'mutating-source']
    for mode in modes:
        env = dict(os.environ)
        env.pop('LEGO_DUMP_MISSING_SHADERS', None)
        if mode != 'disabled':
            env['LEGO_DUMP_MISSING_SHADERS'] = '1'
        subprocess.run([str(executable), mode], env=env, check=True, timeout=10)
    regression = 'not requested'
    if args.verify_regression:
        old = fixture.replace('captured_containers[StageIndex(stage)].insert(hash)',
                              'captured_containers[StageIndex(stage)].insert(guest_address)')
        assert old != fixture
        executable = build('address-dedup', old)
        env = dict(os.environ, LEGO_DUMP_MISSING_SHADERS='1')
        result = subprocess.run([str(executable), 'identity'], env=env, capture_output=True,
                                text=True, timeout=10)
        assert result.returncode != 0 and 'changed valid bytes must capture again' in result.stderr
        regression = 'address-based dedup rejects the second valid container at the same address'
    report = {
        'result': 'PASS', 'regression_proof': regression,
        'production_body_sha256': hashlib.sha256(body.encode()).hexdigest(),
        'cases': modes,
        'boundaries': 'capture and stage selection bodies plus container layout are production; '
                      'xxHash is the pinned implementation; guest memory/range queries and dump I/O '
                      'are simulated. Tests verify exact identity events before existing-file '
                      'dump suppression and emitted bytes, not filesystem persistence.',
    }
    (args.output / 'verification.json').write_text(json.dumps(report, indent=2) + '\n')
    print('PASS: placement capture content/stage identity events, exact bytes, validation and 256-container cap')


if __name__ == '__main__':
    main()
