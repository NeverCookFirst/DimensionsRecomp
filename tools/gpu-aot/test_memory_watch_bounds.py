"""Execute actual PhysicalSpan and SDK range-query bodies against counted pages."""
import argparse
import hashlib
import json
from pathlib import Path
import resource
import subprocess


def extract(source, signature):
    start = source.index(signature)
    begin = source.index('{', start)
    depth = 1
    end = begin + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


OLD_VALIDATION = r'''  for (uint64_t at = first, end = uint64_t(first) + input.length; at < end;) {
    rex::memory::HeapAllocationInfo info{};
    if (!heap->QueryRegionInfo(uint32_t(at), &info) ||
        !(info.state & rex::memory::kMemoryAllocationCommit) ||
        !(info.protect & rex::memory::kMemoryProtectRead)) return false;
    const uint64_t page = heap->heap_base() + ((at - heap->heap_base()) / heap->page_size()) * heap->page_size();
    const uint64_t next = page + info.region_size;
    if (next <= at) return false;
    at = std::min(next, end);
  }'''


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('output', type=Path)
    ap.add_argument('--compiler', default='clang++')
    ap.add_argument('--verify-regression', action='store_true')
    args = ap.parse_args()
    root = Path(__file__).resolve().parents[2]
    watch_path = root/'rexlego/src/gpu_native/memory_watch.cpp'
    sdk_path = root/'rexglue-sdk/src/system/xmemory.cpp'
    watch, sdk = watch_path.read_text(), sdk_path.read_text()
    span = extract(watch, 'bool PhysicalSpan(')
    query = extract(sdk, 'bool BaseHeap::QueryRegionInfo(')
    bounded = extract(sdk, 'bool BaseHeap::IsRangeCommittedReadable(')
    replacement = '  if (!heap->IsRangeCommittedReadable(first, input.length)) return false;'
    assert span.count(replacement) == 1, 'Expected the bounded production validation'
    legacy = span.replace(replacement, OLD_VALIDATION).replace('bool PhysicalSpan(', 'bool LegacyPhysicalSpan(', 1)
    # Instrument only query call counts; page reads are counted by the model's
    # operator[], so the production loop and all its decisions remain intact.
    query = query.replace('{', '{\n  ++queries;', 1)
    bounded = bounded.replace('{', '{\n  ++bounded_calls;', 1)
    model = r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <limits>
#include <mutex>
#include <random>
#include <vector>
#define REXSYS_ERROR(...) ((void)0)
namespace rex::memory {
constexpr uint32_t kMemoryAllocationCommit=2, kMemoryProtectRead=1;
struct HeapAllocationInfo {
 uint32_t base_address,allocation_base,allocation_protect,allocation_size,
          region_size,state,protect;
};
struct Entry { uint32_t base_address,allocation_protect,region_page_count,state,current_protect; };
struct Table {
 std::vector<Entry> entries=std::vector<Entry>(131072);
 uint64_t reads=0;
 size_t size() const { return entries.size(); }
 Entry operator[](size_t index) { ++reads; return entries.at(index); }
};
struct BaseHeap {
 uint32_t heap_base_=0,heap_size_=0x20000000,page_size_=4096,page_size_shift_=12;
 std::recursive_mutex heap_mutex_;
 Table page_table_;
 uint64_t queries=0,bounded_calls=0;
 uint32_t heap_base() const { return heap_base_; }
 uint32_t page_size() const { return page_size_; }
 bool QueryRegionInfo(uint32_t,HeapAllocationInfo*);
 bool IsRangeCommittedReadable(uint32_t,uint32_t);
 void Fill(uint32_t state=2,uint32_t protection=1) {
   for(auto& entry:page_table_.entries) entry={0,protection,131072,state,protection};
 }
 void ResetCount() { page_table_.reads=queries=bounded_calls=0; }
};
struct Memory {
 BaseHeap heap;
 bool discontinuous=false;
 uint32_t bad_endpoint=UINT32_MAX;
 uint32_t GetPhysicalAddress(uint32_t address) {
   if(bad_endpoint!=UINT32_MAX && address==bad_endpoint) return UINT32_MAX;
   if(address>=0xA0000000) {
     auto result=address&0x1fffffff;
     return discontinuous && (result&0xfff) ? result+4096 : result;
   }
   return UINT32_MAX;
 }
 BaseHeap* GetPhysicalHeap() { return &heap; }
};
}
namespace legodimensions::gpu_native {
constexpr uint32_t kPhysicalSize=0x20000000;
struct CpuMemorySpan { uint32_t address=0,length=0; bool operator==(const CpuMemorySpan&) const=default; };
'''
    checks = r'''
using namespace legodimensions::gpu_native;
void Same(rex::memory::Memory& memory,uint32_t address,uint32_t length,bool expected) {
 CpuMemorySpan input{address,length},old_output{0xdddddddd,0xeeeeeeee},new_output=old_output;
 bool old=LegacyPhysicalSpan(&memory,input,old_output);
 bool next=PhysicalSpan(&memory,input,new_output);
 if(old!=next || next!=expected || !(old_output==new_output))
   std::cerr << "span " << std::hex << address << " length " << length
             << " legacy " << old << " bounded " << next << " expected " << expected << "\n";
 assert(old==next && next==expected && old_output==new_output);
 if(next) assert(new_output.length==length);
}
int main() {
 rex::memory::Memory memory;
 memory.heap.Fill();
 for(uint32_t alias:{0u,0xA0000000u,0xC0000000u,0xE0000000u}) {
   Same(memory,alias+4097,8192,true); // Unaligned, three pages.
   Same(memory,alias+0x1ffffff0,16,true); // Last physical byte, no overflow.
 }
 Same(memory,0,0,false);
 Same(memory,0x70001000,4,false); // Not physically mapped.
 Same(memory,0xfffffff0,32,false); // Guest address wraps.
 Same(memory,0x1ffffff0,32,false); // Physical span wraps out of backing.
 memory.discontinuous=true;
 Same(memory,0xA0001000,4096,false); // Endpoint mapping must be contiguous.
 memory.discontinuous=false;
 memory.bad_endpoint=0xA0002fff;
 Same(memory,0xA0001000,8192,false);
 memory.bad_endpoint=UINT32_MAX;
 for(uint32_t state:{0u,1u,2u,3u}) {
   for(uint32_t protect:{0u,1u,2u,3u,4u,5u,8u,9u}) {
     // Noaccess/write-only/guard-bit variants follow the actual SDK's bit
     // predicate; guard plus read remains readable, rather than guessing.
     memory.heap.Fill();
     auto& hole=memory.heap.page_table_.entries[2];
     hole.state=state;hole.current_protect=protect;
     Same(memory,0xA0001000,8192,(state&2)&&(protect&1));
     Same(memory,0xA0001000,4096,true); // Hole outside requested span.
   }
 }
 memory.heap.Fill();
 // Adjacent allocations/protection regions remain valid if all used pages read.
 for(uint32_t i=8;i<16;++i) memory.heap.page_table_.entries[i]={8,3,8,3,3};
 Same(memory,0xC0007001,8*4096,true);
 std::mt19937 random(0x624);
 for(unsigned round=0;round<50;++round) {
   memory.heap.Fill();
   for(unsigned j=0;j<20;++j) {
     auto& e=memory.heap.page_table_.entries[random()%128];
     e.state=random()%4;e.current_protect=random()%16;
   }
   for(unsigned j=0;j<40;++j) {
     uint32_t offset=random()%(128*4096),length=1+random()%32768;
     bool expected=true;
     for(uint32_t page=offset/4096;page<=(offset+length-1)/4096;++page) {
       auto e=memory.heap.page_table_.entries.at(page);
       if(!(e.state&2)||!(e.current_protect&1)) expected=false;
     }
     Same(memory,0xA0000000+offset,length,expected);
   }
 }
 memory.heap.Fill();
 CpuMemorySpan output;
 memory.heap.ResetCount();
 assert(LegacyPhysicalSpan(&memory,{0xA0001000,16},output));
 auto legacy_reads=memory.heap.page_table_.reads;
 assert(memory.heap.queries==1 && legacy_reads==131072);
 memory.heap.ResetCount();
 assert(PhysicalSpan(&memory,{0xA0001000,16},output));
 assert(memory.heap.queries==0 && memory.heap.bounded_calls==1 && memory.heap.page_table_.reads==1);
 std::cout << "PASS equivalence: holes/protections/aliases/overflow/random; page_reads "
           << legacy_reads << " -> " << memory.heap.page_table_.reads << "\n";
}
'''
    source = model + span + '\n' + legacy + '\n}\nnamespace rex::memory {\n' + query + '\n' + bounded + '\n}\n' + checks
    args.output.mkdir(parents=True, exist_ok=True)
    cpp, exe = args.output/'memory-watch-bounds.cpp', args.output/'memory-watch-bounds.exe'
    cpp.write_text(source)
    subprocess.run([args.compiler,'-std=c++20','-O0','-pthread',str(cpp),'-o',str(exe)],check=True,timeout=45)
    def no_core(): resource.setrlimit(resource.RLIMIT_CORE,(0,0))
    result=subprocess.run([str(exe.resolve())],capture_output=True,text=True,check=True,timeout=20,preexec_fn=no_core)
    print(result.stdout.strip())
    negative=False
    if args.verify_regression:
        control = source.replace(replacement,OLD_VALIDATION,1)
        negative_cpp=args.output/'memory-watch-bounds-original.cpp'
        negative_exe=args.output/'memory-watch-bounds-original.exe'
        negative_cpp.write_text(control)
        subprocess.run([args.compiler,'-std=c++20','-O0','-pthread',str(negative_cpp),'-o',str(negative_exe)],check=True,timeout=45)
        bad=subprocess.run([str(negative_exe.resolve())],capture_output=True,text=True,timeout=20,preexec_fn=no_core)
        assert bad.returncode != 0 and 'memory.heap.queries==0' in bad.stderr,bad.stderr
        negative=True
    (args.output/'verification.json').write_text(json.dumps(dict(status='passed',
        actual_production_physical_span=True,actual_sdk_query_and_bounded_bodies=True,
        modeled_page_table_and_mapping=True,negative_original_validation_rejected=negative,
        legacy_page_reads=131072,bounded_page_reads=1,
        source_sha256={str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in (watch_path,sdk_path)},
        scope='Asset-free predicate/scaling proof; actual callback/write invalidation covered separately. No runtime speed or driver claim.'),indent=2)+'\n')


if __name__ == '__main__':
    main()
