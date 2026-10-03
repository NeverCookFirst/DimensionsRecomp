"""Execute the production bounded heap check with holes and boundary cases."""
import argparse
from pathlib import Path
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('output', type=Path)
a = p.parse_args()
a.output.mkdir(parents=True, exist_ok=True)
root = Path(__file__).resolve().parents[2]
source = (root / 'rexglue-sdk/src/system/xmemory.cpp').read_text()
start = source.index('bool BaseHeap::IsRangeCommittedReadable(')
end = source.index('\nbool BaseHeap::QuerySize(', start)
method = source[start:end]
code = r'''
#include <cstdint>
#include <mutex>
#include <vector>
#include <cassert>
#include <random>
namespace memory { constexpr uint32_t kMemoryAllocationCommit=2, kMemoryProtectRead=1; }
struct Entry { uint32_t state=2, current_protect=1; };
struct BaseHeap {
 uint32_t heap_base_=0x100000, heap_size_=128*4096, page_size_shift_=12;
 std::recursive_mutex heap_mutex_;
 std::vector<Entry> page_table_=std::vector<Entry>(128);
 bool IsRangeCommittedReadable(uint32_t address,uint32_t length);
};
''' + method + r'''
int main() {
 BaseHeap h;
 assert(h.IsRangeCommittedReadable(h.heap_base_,h.heap_size_));
 assert(!h.IsRangeCommittedReadable(h.heap_base_-1,1));
 assert(!h.IsRangeCommittedReadable(h.heap_base_+h.heap_size_,1));
 assert(!h.IsRangeCommittedReadable(0xfffffff0u,32));
 assert(h.IsRangeCommittedReadable(0,0));
 h.page_table_[5].state=0; // A hole inside a readable allocation.
 assert(!h.IsRangeCommittedReadable(h.heap_base_+4096*4+4095,2));
 assert(h.IsRangeCommittedReadable(h.heap_base_+4096*4,4096));
 h.page_table_[5].state=1; // Reserved but not committed; read flag still set.
 assert(!h.IsRangeCommittedReadable(h.heap_base_+4096*5,1));
 h.page_table_[5]={2,0};
 assert(!h.IsRangeCommittedReadable(h.heap_base_+4096*5,1));
 std::mt19937 rng(0x1080);
 for (unsigned n=0;n<10000;++n) {
   auto& e=h.page_table_[rng()%128]; e.state=rng()%4;e.current_protect=rng()%4;
   uint32_t offset=rng()%(h.heap_size_+4096), length=rng()%8193;
   bool expected=true;
   for(uint32_t i=0;i<length;++i) {
     if(offset+i>=h.heap_size_){expected=false;break;}
     const auto& entry=h.page_table_[(offset+i)/4096];
     if(!(entry.state&2)||!(entry.current_protect&1)){expected=false;break;}
   }
   assert(h.IsRangeCommittedReadable(h.heap_base_+offset,length)==expected);
 }
 // An address range ending at 4 GiB is valid in the last virtual heap.
 h.heap_base_=0xfff80000;
 for(auto& e:h.page_table_)e={2,1};
 assert(h.IsRangeCommittedReadable(0xfffffff0u,16));
 assert(!h.IsRangeCommittedReadable(0xfffffff0u,17));
}
'''
cpp = a.output / 'readable-range.cpp'
cpp.write_text(code)
exe = a.output / 'readable-range.exe'
subprocess.run(['clang++', '-std=c++20', str(cpp), '-o', str(exe)], check=True)
subprocess.run([str(exe.resolve())], check=True)
print('PASS: production range check, holes/reservation/protection, unaligned boundaries, 4 GiB edge, 10000 differential cases')
