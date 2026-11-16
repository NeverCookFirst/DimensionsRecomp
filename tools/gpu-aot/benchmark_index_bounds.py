"""Compare index scanners with identical inputs, compiler flags, and checksums."""
import argparse
import hashlib
import json
from pathlib import Path
import statistics
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('output', type=Path)
p.add_argument('--baseline-header', type=Path, required=True)
p.add_argument('--compiler', default='clang++')
a = p.parse_args()
root = Path(__file__).resolve().parents[2]
out = a.output.resolve()
out.mkdir(parents=True, exist_ok=True)
candidate = root/'rexlego/src/gpu_native/index_bounds.h'
baseline = a.baseline_header.read_text()
(out/'baseline.h').write_text(baseline.replace('namespace legodimensions::gpu_native', 'namespace baseline'))
(out/'candidate.h').write_bytes(candidate.read_bytes())
cpp = out/'benchmark.cpp'
cpp.write_text(r'''
#include "baseline.h"
#include "candidate.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <random>
#include <vector>
__declspec(noinline) uint64_t Old(std::span<const uint8_t> bytes, uint32_t width) {
  const auto r = baseline::ScanGuestIndexBounds(bytes, width);
  return (uint64_t(r.min) << 32) | r.max;
}
__declspec(noinline) uint64_t New(std::span<const uint8_t> bytes, uint32_t width) {
  const auto r = legodimensions::gpu_native::ScanGuestIndexBounds(bytes, width);
  return (uint64_t(r.min) << 32) | r.max;
}
int main() {
  std::mt19937 random(0x83FC6A58);
  for (unsigned width : {2u, 4u}) for (unsigned count : {3u, 24u, 96u, 384u, 1536u, 6144u, 24576u}) {
    std::vector<uint8_t> data(count * width + 32);
    for (auto& byte : data) byte = uint8_t(random());
    const unsigned repetitions = std::max(500u, 8u * 1024 * 1024 / (count * width));
    for (unsigned trial = 0; trial < 8; ++trial) for (unsigned order = 0; order < 2; ++order) {
      const bool newer = bool(order ^ (trial & 1));
      const auto scan = newer ? New : Old;
      uint64_t checksum = 0;
      const auto begin = std::chrono::steady_clock::now();
      for (unsigned i = 0; i < repetitions; ++i) {
        std::atomic_signal_fence(std::memory_order_seq_cst);
        checksum += scan({data.data() + (i & 31), count * width}, width);
      }
      const auto ns = std::chrono::duration<double, std::nano>(
          std::chrono::steady_clock::now() - begin).count() / repetitions;
      std::printf("%u,%u,%u,%u,%.3f,%llu\n", width, count, trial, unsigned(newer), ns,
          static_cast<unsigned long long>(checksum));
    }
  }
}
''')
flags = ['-std=c++20', '-O3', '-march=x86-64-v3']
exe = out/'benchmark.exe'
subprocess.run([a.compiler, *flags, str(cpp), '-o', str(exe)], check=True, timeout=60)
r = subprocess.run([str(exe)], capture_output=True, text=True, check=True, timeout=120)
(out/'samples.csv').write_text('width,count,trial,candidate,ns,checksum\n' + r.stdout)
rows = [line.split(',') for line in r.stdout.splitlines()]
summary = []
for width in (2, 4):
    for count in (3, 24, 96, 384, 1536, 6144, 24576):
        selected = [row for row in rows if int(row[0]) == width and int(row[1]) == count]
        assert len(selected) == 16
        assert len({row[5] for row in selected}) == 1
        medians = [statistics.median(float(row[4]) for row in selected if int(row[3]) == mode)
                   for mode in (0, 1)]
        item = dict(width=width, count=count, baseline_ns=medians[0], candidate_ns=medians[1],
                    speedup=medians[0]/medians[1])
        summary.append(item)
        print(json.dumps(item))
report = dict(scope='CPU index scan microbenchmark; no gameplay FPS claim',
              baseline_sha256=hashlib.sha256(a.baseline_header.read_bytes()).hexdigest(),
              candidate_sha256=hashlib.sha256(candidate.read_bytes()).hexdigest(),
              compiler=subprocess.check_output([a.compiler, '--version'], text=True),
              flags=flags, checksums_equal=True, summary=summary)
(out/'verification.json').write_text(json.dumps(report, indent=2) + '\n')
