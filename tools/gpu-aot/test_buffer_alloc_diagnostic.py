"""Check actual bounded buffer-allocation trigger state without GPU assets."""
import argparse
from pathlib import Path
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('output', type=Path)
p.add_argument('--compiler', default='clang++')
a = p.parse_args()
root = Path(__file__).resolve().parents[2]
a.output.mkdir(parents=True, exist_ok=True)
source = a.output / 'buffer-alloc-diagnostic.cpp'
source.write_text(r'''
#include "gpu_native/buffer_alloc_diagnostic.h"
#include <cassert>
#include <fstream>
using namespace legodimensions::gpu_native;
int main(int argc,char**argv) {
 assert(argc==2);
 const auto path=std::filesystem::path(argv[1]);
 std::filesystem::remove(path);
 using D=BufferAllocationDiagnostic;
 assert(D::ParseGuest("849a4070")==0x849A4070);
 for(const auto bad:{"","849A407","849A40700","0x849A4070","849A407G"," 49A4070","-49A4070"})
  assert(!D::ParseGuest(bad));
 D disabled({},path),no_path(0x849A4070,{}),active(0x849A4070,path);
 assert(!active.Select(10,0x849A4070));
 {std::ofstream file(path);file<<"arm";}
 assert(!disabled.Select(11,0x849A4070)&&std::filesystem::exists(path));
 assert(!no_path.Select(11,0x849A4070));
 assert(!active.Select(10,0x849A4070)&&std::filesystem::exists(path)); // One check/frame.
 assert(!active.Select(11,0x849A4090)&&std::filesystem::exists(path)); // Exact guest filter.
 assert(!active.Select(11,0x849A4070)&&!std::filesystem::exists(path));
 assert(!active.Select(11,0x849A4070)); // Do not begin in a partial frame.
 for(unsigned n=0;n<128;++n) assert(active.Select(12+n/4,0x849A4070));
 {std::ofstream file(path);}
 for(unsigned n=0;n<1000;++n) assert(!active.Select(100+n,0x849A4070));
 assert(std::filesystem::exists(path)); // Whole-process cap cannot rearm.
 std::filesystem::remove(path);
}
''')
exe = a.output / 'buffer-alloc-diagnostic'
subprocess.run([a.compiler, '-std=c++20', '-O2', '-I' + str(root / 'rexlego/src'),
                str(source), '-o', str(exe)], check=True, timeout=45)
subprocess.run([str(exe.resolve()), str((a.output / 'allocation.trigger').resolve())],
               check=True, timeout=10)
print('PASS exact guest filter, disabled state, one poll/frame, deferred arming, 128-record cap')
