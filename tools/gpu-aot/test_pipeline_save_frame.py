"""Exercise the production per-present cache check against a busy serializer."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('output', type=Path)
p.add_argument('--before', type=Path)
p.add_argument('--compiler', default='clang-cl')
a = p.parse_args()
root = Path(__file__).resolve().parents[2]
source = a.before or root/'rexlego/src/gpu_native/pipeline_library.cpp'
text = source.read_text()
start = text.index('  void Frame() {')
end = start+text[start:].index('{')+1
depth = 1
while depth:
    depth += (text[end] == '{')-(text[end] == '}')
    end += 1
body = text[start:end]
out = a.output.resolve()
out.mkdir(parents=True, exist_ok=True)
cpp = out/'pipeline-save-frame.cpp'
cpp.write_text(r'''
#include <atomic>
#include <chrono>
#include <cstdio>
#include <future>
#include <mutex>
#include <thread>
using Clock=std::chrono::steady_clock;
constexpr auto kQuietBeforeSave=std::chrono::seconds(5);
struct Cache {
 std::mutex mutex_; std::atomic<bool> saving_{false};
 unsigned unsaved_=1; Clock::time_point last_store_{};
 std::atomic<unsigned> saves{0};
 void Save(){std::lock_guard lock(mutex_);++saves;unsaved_=0;}
 ~Cache(){while(saving_.load())std::this_thread::yield();}
''' + body + r'''
};
int main(){
 Cache cache; std::promise<void> held;
 std::thread serializer([&]{std::lock_guard lock(cache.mutex_);held.set_value();
   std::this_thread::sleep_for(std::chrono::milliseconds(200));});
 held.get_future().wait();auto begin=Clock::now();
 for(int i=0;i<1000;i++)cache.Frame();
 const auto ms=std::chrono::duration<double,std::milli>(Clock::now()-begin).count();
 serializer.join();bool good=ms<100;
 cache.Frame();while(cache.saving_.load())std::this_thread::yield();
 good=good&&cache.saves==1;
 cache.Frame();good=good&&cache.saves==1;
 {std::lock_guard lock(cache.mutex_);cache.unsaved_=1;cache.last_store_=Clock::now();}
 cache.Frame();good=good&&cache.saves==1;
 std::printf("1000 busy present checks: %.3f ms; saved=%u; quiet/clean checks preserved=%d\n",
  ms,cache.saves.load(),int(good));
 return good?0:23;
}
''')
exe = out/'pipeline-save-frame.exe'
r = subprocess.run([a.compiler,str(cpp),'/std:c++20','/O2','/MT','/EHsc',
                    '/Fo'+out.as_posix()+'/', '/Fe'+str(exe)],capture_output=True,text=True,timeout=60)
(out/'compile.log').write_text(r.stdout+r.stderr)
r.check_returncode()
r = subprocess.run([str(exe)],capture_output=True,text=True,timeout=10)
(out/'results.log').write_text(r.stdout+r.stderr)
expected = 23 if a.before else 0
assert r.returncode == expected,(r.returncode,r.stdout,r.stderr)
(out/'verification.json').write_text(json.dumps({'game_launched':False,
 'actual_production_frame_body':True,'synthetic_serializer':True,
 'negative_control':bool(a.before),'exit':r.returncode,'output':r.stdout,
 'source_sha256':hashlib.sha256(source.read_bytes()).hexdigest()},indent=2))
print(r.stdout.strip())
