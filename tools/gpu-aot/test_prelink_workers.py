"""Run real prelink worker selection/pool bodies without DXC or actual threads."""
import argparse
from pathlib import Path
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('output', type=Path)
p.add_argument('--compiler', default='clang++')
a = p.parse_args()
root = Path(__file__).resolve().parents[2]
source = (root / 'tools/gpu-aot/prelink_shader_cache.cpp').read_text()
selection_start = source.index('  const auto worker_count = ')
selection_end = source.index('  std::map<uint32_t, std::vector<uint8_t>> specLibs;', selection_start)
selection = source[selection_start:selection_end]
# Invalid budgets must fail before any DXC compiler/library work.
assert selection_end < source.index('CompileSpecConstantLib(', selection_end)
pool_start = source.index('    std::vector<std::thread> pool;')
pool_end = source.index('\n  }', pool_start)
pool = source[pool_start:pool_end]
# Substitute only the platform observations and thread recorder. The production
# budget selection, error return, loop bound and join loop remain unchanged.
selection = selection.replace('std::thread::hardware_concurrency()', 'hardware')
selection = selection.replace('std::getenv("LEGO_GPU_PRELINK_JOBS")', 'requested')
pool = pool.replace('std::thread', 'RecordingThread')
header = (root / 'tools/gpu-aot/prelink_jobs.h').as_posix()
code = r'''
#include <cassert>
#include <climits>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>
''' + '#include "' + header + '"\n' + r'''
unsigned created=0,joined=0,calls=0;
struct RecordingThread {
  template<class F>explicit RecordingThread(F worker){++created;worker();}
  void join(){++joined;}
};
int Run(size_t job_count,unsigned hardware,const char* requested){
  std::vector<unsigned char> jobs(job_count);
''' + selection + r'''
  auto worker=[]{++calls;};
''' + pool + r'''
  return 0;
}
void Check(size_t jobs,unsigned hardware,const char* requested,unsigned expected){
  created=joined=calls=0;
  assert(Run(jobs,hardware,requested)==0);
  assert(created==expected&&joined==expected&&calls==expected);
}
int main(){
  Check(0,20,nullptr,0);Check(0,0,"1",0);
  Check(1,20,nullptr,1);Check(3,20,nullptr,3);Check(30,20,nullptr,20);
  Check(30,0,nullptr,4);Check(2,0,nullptr,2);
  Check(30,20,"1",1);Check(3,20,"2",2);Check(3,20,"100",3);
  Check(30,20,"100",20);Check(30,0,"100",4);Check(30,0,"2",2);
  Check(30,20,"0002",2);
  const auto maximum=std::to_string(UINT_MAX);
  Check(30,20,maximum.c_str(),20);
  for(const char* bad:{"","0","0000","-1","+1"," 1","1 ","1\n","1x","0x2",
                       "1.5","99999999999999999999999999999999999999"}){
    for(size_t jobs:{size_t{0},size_t{3}}){
      created=joined=calls=0;
      assert(Run(jobs,20,bad)==1);
      assert(created==0&&joined==0&&calls==0);
    }
  }
  const auto overflow=std::to_string(static_cast<unsigned long long>(UINT_MAX)+1);
  assert(!lego_gpu_aot::PrelinkWorkerCount(3,20,overflow.c_str()));
  // Budget arithmetic must not narrow the size_t job count before capping.
  assert(lego_gpu_aot::PrelinkWorkerCount(std::numeric_limits<size_t>::max(),20,"1")==1);
  // Every positive hint/request combination obeys all three ceilings.
  for(unsigned hardware=0;hardware<32;++hardware){
    for(unsigned request=1;request<48;++request){
      const auto requested=std::to_string(request);
      for(unsigned jobs=0;jobs<64;++jobs){
        const auto count=lego_gpu_aot::PrelinkWorkerCount(jobs,hardware,requested.c_str());
        assert(count&&*count<=jobs&&*count<=request&&*count<=(hardware?hardware:4));
        assert((*count==0)==(jobs==0));
      }
    }
  }
  std::puts("PASS: actual prelink pool respects strict requested/hardware/job ceilings; invalid budgets create no workers");
}
'''
a.output.mkdir(parents=True, exist_ok=True)
cpp = a.output.resolve() / 'prelink-workers.cpp'
exe = a.output.resolve() / 'prelink-workers.exe'
cpp.write_text(code)
subprocess.run([a.compiler, '-std=c++20', '-UNDEBUG', str(cpp), '-o', str(exe)],
               check=True, timeout=45)
subprocess.run([str(exe)], check=True, timeout=10)
