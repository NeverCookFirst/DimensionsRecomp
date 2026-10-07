"""Exercise actual guest state-request counters and unchanged API emissions."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess


def function(source, signature):
    begin = source.index(signature)
    end = source.index('{', begin) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[begin:end]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    parser.add_argument('--compiler', default='clang++')
    parser.add_argument('--negative-control', action='store_true',
                        help='Deliberately ignore recording serial; oracle must fail')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    root = Path(__file__).resolve().parents[2]
    source = (root/'rexlego/src/gpu_native/draw.cpp').read_text()
    bodies = '\n'.join(function(source, name) for name in (
        'StateCallCapture* NativeStateCalls(', 'void BindNativeDrawLayout(',
        'void BindNativeDrawDescriptorSets(', 'void BindNativeDrawPipeline('))
    dispatch = function(source, 'bool DispatchDraw(')
    assert dispatch.count('HostDevice::CurrentDrawDeviceView()') == 1
    assert 'draw_device.commands != commands' in dispatch
    assert 'capture->BeginDraw(commands, draw_device.recording_serial)' in dispatch
    assert 'auto* null_buffer = draw_device.null_vertex_buffer;' in dispatch
    assert dispatch.index('BindNativeDrawLayout(') < dispatch.index('BindNativeDrawDescriptorSets(')
    assert dispatch.index('BindNativeDrawDescriptorSets(') < dispatch.index('BindConstants(')
    assert dispatch.index('BindConstants(') < dispatch.index('BindNativeDrawPipeline(')
    assert 'capture->Present(g_probe_frame.load())' in function(source, 'u32 SwapHook(')
    include = root/'rexlego/src'
    if args.negative_control:
        header = (include/'gpu_native/state_call_diagnostic.h').read_text()
        old = 'commands != commands_ || serial != serial_'
        assert header.count(old) == 1
        header = header.replace(old, 'commands != commands_')
        include = args.output/'negative-include'
        (include/'gpu_native').mkdir(parents=True, exist_ok=True)
        (include/'gpu_native/state_call_diagnostic.h').write_text(header)
    cpp = args.output/'state-call-diagnostic.cpp'
    cpp.write_text(r'''
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>
#include "gpu_native/state_call_diagnostic.h"
using namespace legodimensions::gpu_native;
using u32=uint32_t;
void Check(bool value,const char* reason){if(!value){std::cerr<<reason<<'\n';std::exit(17);}}
namespace plume {
struct RenderPipelineLayout{}; struct RenderDescriptorSet{};struct RenderPipeline{};
struct RenderCommandList {
 std::vector<std::string> calls;
 void setGraphicsPipelineLayout(RenderPipelineLayout*){calls.push_back("layout");}
 void setGraphicsDescriptorSet(RenderDescriptorSet*,u32 slot){calls.push_back("table"+std::to_string(slot));}
 void setPipeline(RenderPipeline*){calls.push_back("pipeline");}
};
}
struct DrawDeviceView {plume::RenderPipelineLayout* pipeline_layout;
 plume::RenderDescriptorSet* texture_descriptors;plume::RenderDescriptorSet* sampler_descriptors;};
''' + bodies + r'''
int main(int argc,char** argv){
 if(argc==3){
  auto* enabled=NativeStateCalls();Check(enabled!=nullptr,"explicit opt-in accessor");
  plume::RenderCommandList own;
  Check(enabled->BeginDraw(&own,1)!=nullptr,"opt-in captures valid recording");
  enabled->Present(1);
  Check(std::filesystem::exists(argv[2]),"opt-in writes designated aggregate");
  return 0;
 }
 Check(argc==2,"expected own output directory");
 // The actual cached opt-in accessor performs no capture when unset/empty.
 Check(NativeStateCalls()==nullptr,"disabled capture must be null");
 plume::RenderCommandList commands;
 plume::RenderPipelineLayout layout,other_layout;
 plume::RenderDescriptorSet texture,sampler,other_texture;
 plume::RenderPipeline graphics,compute,ray;
 DrawDeviceView view{&layout,&texture,&sampler};
 auto bind=[&](StateCallLedger* ledger){
  BindNativeDrawLayout(&commands,view,ledger);
  BindNativeDrawDescriptorSets(&commands,view,ledger);
  BindNativeDrawPipeline(&commands,&graphics,ledger);
 };
 bind(nullptr);bind(nullptr);
 Check(commands.calls.size()==12,"disabled original API count");
 const std::vector<std::string> order{"layout","table0","table1","table2","table3","pipeline"};
 Check(std::equal(order.begin(),order.end(),commands.calls.begin()),"original binding order");
 StateCallLedger ledger;
 Check(ledger.BeginDraw(&commands,1),"first recording");bind(&ledger);
 Check(ledger.BeginDraw(&commands,1),"same recording");bind(&ledger);
 Check(ledger.Counts().recordings==1&&ledger.Counts().equal_layouts==1&&
       ledger.Counts().equal_pipelines==1,"same-recording equality");
 for(u32 i=0;i<4;++i)Check(ledger.Counts().descriptors[i]==2&&
        ledger.Counts().equal_descriptors[i]==1,"independent table slot equality");
 // Same physical command pointer is reused after Reset; serial is decisive.
 Check(ledger.BeginDraw(&commands,2),"reopened same command pointer");bind(&ledger);
 Check(ledger.Counts().recordings==2&&ledger.Counts().equal_pipelines==1&&
       ledger.Counts().equal_layouts==1,"recording_reset_rebind");
 for(u32 i=0;i<4;++i)Check(ledger.Counts().equal_descriptors[i]==1,"recording resets tables");
 // Observed layout changes invalidate the comparison for every root table.
 view.pipeline_layout=&other_layout;
 Check(ledger.BeginDraw(&commands,2),"layout change draw");bind(&ledger);
 for(u32 i=0;i<4;++i)Check(ledger.Counts().equal_descriptors[i]==1,"changed layout resets tables");
 view.texture_descriptors=&other_texture;
 Check(ledger.BeginDraw(&commands,2),"descriptor change draw");bind(&ledger);
 for(u32 i=0;i<3;++i)Check(ledger.Counts().equal_descriptors[i]==1,"changed texture table");
 Check(ledger.Counts().equal_descriptors[3]==2,"independent sampler table");
 // The scope is guest requests. Unobserved helper/compute/ray binds must
 // still be emitted, even though adjacent guest requests can compare equal.
 const auto before=commands.calls.size();
 commands.setPipeline(&compute);commands.setPipeline(&ray);
 Check(ledger.BeginDraw(&commands,2),"draw after unobserved pipeline");bind(&ledger);
 Check(commands.calls.size()==before+8,"interleaved pipeline bindings preserved");
 const auto repeats=ledger.Counts().equal_pipelines;
 Check(repeats>1,"scope explicitly counts adjacent guest requests only");
 ledger.Reset();
 for(u32 i=0;i<StateCallLedger::kMaxDraws;++i){Check(ledger.BeginDraw(&commands,3),"draw budget");bind(&ledger);}
 for(u32 i=0;i<100;++i)Check(!ledger.BeginDraw(&commands,3),"draw budget closes diagnostic only");
 Check(ledger.Counts().draws==4096&&ledger.Counts().dropped_draws==100,"bounded draw counts");
 bind(nullptr); // Real recording remains available beyond diagnostic budget.
 auto directory=std::filesystem::path(argv[1]);
 auto output=directory/"bounded.csv",trigger=directory/"owned.trigger";
 auto retained=directory/"retained.csv";
 std::filesystem::remove(output);std::filesystem::remove(trigger);std::filesystem::remove(retained);
 {std::ofstream file(retained);file<<"retained evidence\n";}
 {StateCallCapture capture(retained,{});capture.Present(0);}
 {std::ifstream file(retained);std::string line;std::getline(file,line);Check(line=="retained evidence","existing evidence preserved");}
 StateCallCapture capture(output,trigger);
 Check(!capture.BeginDraw(&commands,1),"trigger initially absent");
 capture.Present(0);Check(!std::filesystem::exists(output),"unarmed creates no output");
 {std::ofstream file(trigger);file<<"arm\n";}
 capture.Present(1);Check(!std::filesystem::exists(trigger),"trigger consumed before capture");
 Check(!capture.BeginDraw(nullptr,1)&&!capture.BeginDraw(&commands,0),"invalid recording has no diagnostic");
 for(u32 frame=0;frame<120;++frame){auto* current=capture.BeginDraw(&commands,uint64_t(frame)+4);
  Check(current!=nullptr,"armed bounded capture");bind(current);capture.Present(frame+2);}
 Check(!capture.BeginDraw(&commands,200),"frame budget exhausted");capture.Present(200);
 std::ifstream file(output);std::string line;u32 lines=0;
 while(std::getline(file,line)){++lines;if(lines==1)Check(line.find("unobserved_helper_compute_ray_or_external")!=std::string::npos,"scope qualifier retained");}
 Check(lines==122&&std::filesystem::file_size(output)<1024*1024,"finite aggregate output");
 std::cout<<"PASS: actual binding bodies preserve calls/order; exact recording serial resets comparisons; bounded guest-request output\n";
}
''')
    exe = args.output/'state-call-diagnostic.exe'
    subprocess.run([args.compiler, '-std=c++20', '-O2', '-I'+str(include), str(cpp),
                    '-o', str(exe)], check=True, timeout=45)
    environment = dict(__import__('os').environ)
    environment.pop('LEGO_NATIVE_STATE_CALLS', None)
    environment.pop('LEGO_NATIVE_STATE_CALLS_TRIGGER', None)
    result = subprocess.run([str(exe.resolve()), str(args.output.resolve())],
                            env=environment, capture_output=True, text=True, timeout=10)
    if args.negative_control:
        if result.returncode != 17 or 'recording_reset_rebind' not in result.stderr:
            raise RuntimeError('Negative control did not fail the exact recording-reset oracle: '+result.stderr)
        print('PASS: omitted serial fails recording_reset_rebind')
        return
    if result.returncode:
        raise RuntimeError(result.stderr)
    opt_in = args.output/'opt-in-accessor.csv'
    opt_in.unlink(missing_ok=True)
    environment['LEGO_NATIVE_STATE_CALLS'] = str(opt_in.resolve())
    subprocess.run([str(exe.resolve()), str(args.output.resolve()), str(opt_in.resolve())],
                   env=environment, check=True, timeout=10)
    report = {'passed': True, 'draw_source_sha256': hashlib.sha256(source.encode()).hexdigest(),
              'actual_functions': ['NativeStateCalls', 'BindNativeDrawLayout',
                  'BindNativeDrawDescriptorSets', 'BindNativeDrawPipeline'],
              'scope': 'adjacent guest requests; unobserved helper/compute/ray/external calls excluded',
              'no_rendering_changes': True, 'max_frames': 120, 'max_draws_per_frame': 4096,
              'per_frame_aggregate': True, 'driver_boundary': 'fake command-list API recorder'}
    (args.output/'verification.json').write_text(json.dumps(report, indent=2)+'\n')
    print(result.stdout.strip())


if __name__ == '__main__':
    main()
