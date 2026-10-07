"""Verify actual frame CSV publication without per-draw detailed timing."""
import argparse
import csv
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("--compiler", default="clang++")
    parser.add_argument("--negative-control", action="store_true")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    source = (root / "rexlego/src/gpu_native/device.cpp").read_text()
    start = source.index("  if (timing_enabled || metrics_enabled) {")
    end = source.index("  state.snapshot_number = 0;", start)
    block = source[start:end]
    if args.negative_control:
        block = block.replace("if (timing_enabled || metrics_enabled)", "if (timing_enabled)")
    cpp = r'''
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>
using u32=uint32_t;using u64=uint64_t;
int detailed_logs=0,draw_consumptions=0,upload_consumptions=0,buffer_consumptions=0;
#define REXLOG_INFO(...) (++detailed_logs)
bool LongProbeEnabled(){return false;}
template<class... T>void LongProbeEvent(T...){throw std::runtime_error("unexpected trace");}
struct Upload {double cpu_ms=0,source_ms=0,hash_ms=0;u64 calls=0,source_hits=0,converted_bytes=0,hashed_bytes=0;};
struct Draw {u64 calls=542;std::array<double,7> stages_ms{};std::array<double,5> constants_ms{};};
struct Buffer {double hash_ms=0;u64 calls=0,hashed_bytes=0,converted_bytes=0,watch_hits=0,watch_audits=0,watch_mismatches=0;};
Upload ConsumeTextureUploadTiming(){++upload_consumptions;return {};}
Draw ConsumeDrawTiming(){++draw_consumptions;return {};}
Buffer ConsumeBufferUploadTiming(){++buffer_consumptions;return {};}
struct State {
 u32 present_number=120,timing_log_count=0;
 std::chrono::steady_clock::time_point last_present_time=std::chrono::steady_clock::now()-std::chrono::milliseconds(70);
 u64 sync_calls=3,cpu_resource_wait_skips=4,frame_slot_wait_calls=8,callbacks_enqueued=42,callbacks_executed=42;
 double sync_ms=0,frame_slot_wait_ms=0,acquire_cpu_ms=0,present_submit_cpu_ms=0,swap_present_cpu_ms=0;
 std::array<u64,7> sync_reason_calls{};std::array<double,7> sync_reason_ms{};
 std::vector<int> completion_callbacks;
};
void Publish(State& state,bool timing_enabled,bool metrics_enabled){
 const auto present_start=std::chrono::steady_clock::time_point{};
 const bool presented=true;
'''+block+r'''
}
int main(int argc,char** argv){try{
#ifdef _WIN32
 _putenv_s("LEGO_NATIVE_FRAME_METRICS",argv[1]);
#else
 setenv("LEGO_NATIVE_FRAME_METRICS",argv[1],1);
#endif
 State disabled;Publish(disabled,false,false);
 if(draw_consumptions)throw std::runtime_error("disabled metrics consumed counters");
 State cadence;Publish(cadence,false,true);
 if(draw_consumptions!=1||upload_consumptions!=1||buffer_consumptions!=1)
  throw std::runtime_error("cadence-only CSV not published");
 if(cadence.sync_calls||cadence.frame_slot_wait_calls||cadence.callbacks_enqueued||cadence.callbacks_executed)
  throw std::runtime_error("cadence-only counters did not reset");
 if(cadence.last_present_time.time_since_epoch().count()==0||detailed_logs)
  throw std::runtime_error("cadence-only detailed logs emitted");
 State detailed;detailed.present_number=240;Publish(detailed,true,false);
 if(draw_consumptions!=2||detailed_logs==0)throw std::runtime_error("detailed timing disabled");
 return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 17;}}
'''
    args.output.mkdir(parents=True, exist_ok=True)
    cpp_path = args.output.resolve() / "frame-cadence.cpp"
    exe = args.output.resolve() / "frame-cadence.exe"
    metrics = args.output.resolve() / "frames.csv"
    metrics.unlink(missing_ok=True)
    cpp_path.write_text(cpp)
    subprocess.run([args.compiler, "-std=c++20", str(cpp_path), "-o", str(exe)], check=True, timeout=45)
    result = subprocess.run([str(exe), str(metrics)], capture_output=True, text=True, timeout=10)
    if args.negative_control:
        if result.returncode != 17 or "cadence-only CSV not published" not in result.stderr:
            raise RuntimeError("Negative control missed cadence publication oracle")
        print("Legacy detailed-only publication negative control rejected as expected")
        return
    result.check_returncode()
    with metrics.open(newline="") as stream:
        rows = list(csv.DictReader(stream))
    assert len(rows) == 2 and [row['detailed_timing_enabled'] for row in rows] == ['0', '1']
    row = rows[0]
    assert float(row['interval_ms']) >= 60 and int(row['draw_calls']) == 542
    assert int(row['frame_slot_wait_calls']) == 8 and int(row['callbacks_enqueued']) == 42
    for key in ('constants_ms', 'vertices_ms', 'buffer_hash_ms', 'present_cpu_ms', 'frame_slot_wait_ms'):
        assert float(row[key]) == 0, key
    print("PASS: actual frame CSV cadence without detailed timing; counters/reset and explicit timing field.")


if __name__ == '__main__':
    main()
