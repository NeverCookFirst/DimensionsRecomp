"""Compare actual Plume binding bodies through an ordered fake D3D12 recorder."""
import argparse
import hashlib
import json
import os
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
    parser.add_argument('--negative-control', choices=('graphics-key', 'reset', 'external', 'metadata'))
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    root = Path(__file__).resolve().parents[2]
    source = (root/'thirdparty/plume/plume_d3d12.cpp').read_text()
    header = (root/'thirdparty/plume/plume_d3d12.h').read_text()
    signatures = ('static bool D3D12StateCacheEnabled(',
                  'static void D3D12ReportStateCacheConfiguration(',
                  'void D3D12CommandList::begin(', 'void D3D12CommandList::end(',
                  'void D3D12CommandList::setPipeline(',
                  'void D3D12CommandList::setGraphicsPipelineLayout(',
                  'void D3D12CommandList::setComputePipelineLayout(',
                  'void D3D12CommandList::setRaytracingPipelineLayout(',
                  'void D3D12CommandList::notifyCommandStateWasChangedExternally(',
                  'void D3D12CommandList::notifyDescriptorHeapWasChangedExternally(',
                  'void D3D12CommandList::checkTopology(',
                  'void D3D12CommandList::checkStencilRef(',
                  'void D3D12CommandList::setRootDescriptor(',
                  'void D3D12CommandList::setGraphicsRootDescriptor(')
    bodies = '\n'.join(function(source, signature) for signature in signatures)
    if args.negative_control == 'graphics-key':
        old = ('!cacheEnabled || !cachedPipelineValid || cachedPipelineType != pipelineType ||\n'
               '                cachedPipelineObject != graphicsPipeline->d3d')
        assert bodies.count(old) == 1
        bodies = bodies.replace(old, '!cacheEnabled || activeGraphicsPipeline != graphicsPipeline')
    elif args.negative_control == 'reset':
        old = '        notifyCommandStateWasChangedExternally();'
        assert bodies.count(old) == 2
        bodies = bodies.replace(old, '')
    elif args.negative_control == 'external':
        old = function(source, 'void D3D12CommandList::notifyCommandStateWasChangedExternally(')
        bodies = bodies.replace(old, 'void D3D12CommandList::notifyCommandStateWasChangedExternally() {}')
    elif args.negative_control == 'metadata':
        old = 'activeGraphicsPipelineLayout = interfacePipelineLayout;'
        assert bodies.count(old) == 1
        bodies = bodies.replace(old, 'if (!activeGraphicsPipelineLayout || activeGraphicsPipelineLayout->rootSignature != interfacePipelineLayout->rootSignature) '+old)
    begin = header.index('        ID3D12GraphicsCommandList *d3d =', header.index('struct D3D12CommandList :'))
    end = header.index('        D3D12CommandList(D3D12CommandQueue *queue)', begin)
    fields = header[begin:end]
    code = r'''
#include <array>
#include <cassert>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>
void Check(bool value,const char* name){if(!value){std::cerr<<name<<'\n';std::exit(17);}}
using D3D12_PRIMITIVE_TOPOLOGY=uint32_t;
constexpr uint32_t D3D_PRIMITIVE_TOPOLOGY_UNDEFINED=0;
using D3D12_GPU_VIRTUAL_ADDRESS=uint64_t;
struct ID3D12RootSignature {uint32_t id;};
struct NativePso {uint32_t id,kind;};
struct ID3D12StateObject {uint32_t id;};
struct FakeD3D {
 uint32_t pipeline_id=0,pipeline_kind=0,topology=0,stencil=0;
 ID3D12RootSignature* graphics_root=nullptr;ID3D12RootSignature* compute_root=nullptr;
 std::array<uint64_t,4> graphics_args{},compute_args{};
 uint32_t pso_calls=0,graphics_root_calls=0,compute_root_calls=0,cbv_calls=0;
 void Reset(void*,void*){pipeline_id=pipeline_kind=0;graphics_root=compute_root=nullptr;
  graphics_args={};compute_args={};topology=stencil=0;}
 void Close(){}
 void SetPipelineState(NativePso* p){++pso_calls;pipeline_id=p?p->id:0;pipeline_kind=p?p->kind:0;}
 void SetPipelineState1(ID3D12StateObject* p){++pso_calls;pipeline_id=p?p->id:0;pipeline_kind=3;}
 void SetGraphicsRootSignature(ID3D12RootSignature* root){++graphics_root_calls;
  if(graphics_root!=root)graphics_args={};graphics_root=root;}
 void SetComputeRootSignature(ID3D12RootSignature* root){++compute_root_calls;
  if(compute_root!=root)compute_args={};compute_root=root;}
 void SetGraphicsRootConstantBufferView(uint32_t index,uint64_t value){++cbv_calls;graphics_args.at(index)=value;}
 void SetGraphicsRootShaderResourceView(uint32_t index,uint64_t value){graphics_args.at(index)=value;}
 void SetGraphicsRootUnorderedAccessView(uint32_t index,uint64_t value){graphics_args.at(index)=value;}
 void SetComputeRootConstantBufferView(uint32_t index,uint64_t value){compute_args.at(index)=value;}
 void SetComputeRootShaderResourceView(uint32_t index,uint64_t value){compute_args.at(index)=value;}
 void SetComputeRootUnorderedAccessView(uint32_t index,uint64_t value){compute_args.at(index)=value;}
 void IASetPrimitiveTopology(uint32_t value){topology=value;}
 void OMSetStencilRef(uint32_t value){stencil=value;}
};
using ID3D12GraphicsCommandList=FakeD3D;using ID3D12GraphicsCommandList1=FakeD3D;
using ID3D12GraphicsCommandList4=FakeD3D;
struct ID3D12CommandAllocator {void Reset(){}};
struct NativeBuffer {uint64_t GetGPUVirtualAddress()const{return 0x10000;}};
namespace plume {
struct RenderPipeline{};struct RenderPipelineLayout{};struct RenderCommandList{};
struct RenderBuffer{};struct RenderBufferReference{RenderBuffer* ref;uint64_t offset;};
enum class RenderRootDescriptorType {CONSTANT_BUFFER,SHADER_RESOURCE,UNORDERED_ACCESS};
struct D3D12Pipeline : RenderPipeline {enum class Type:uint32_t{Compute=1,Graphics=2,Raytracing=3};Type type;};
struct D3D12ComputePipeline:D3D12Pipeline{NativePso* d3d;D3D12ComputePipeline(NativePso* p){type=Type::Compute;d3d=p;}};
struct D3D12GraphicsPipeline:D3D12Pipeline{NativePso* d3d;uint32_t topology,stencilRef;
 D3D12GraphicsPipeline(NativePso* p,uint32_t t,uint32_t s){type=Type::Graphics;d3d=p;topology=t;stencilRef=s;}};
struct D3D12RaytracingPipeline:D3D12Pipeline{ID3D12StateObject* stateObject;
 D3D12RaytracingPipeline(ID3D12StateObject* p){type=Type::Raytracing;stateObject=p;}};
struct D3D12PipelineLayout:RenderPipelineLayout{
 ID3D12RootSignature* rootSignature;std::vector<std::pair<uint32_t,RenderRootDescriptorType>> rootDescriptorRootIndicesAndTypes;
 D3D12PipelineLayout(ID3D12RootSignature* r,uint32_t index=0):rootSignature(r){rootDescriptorRootIndicesAndTypes.push_back({index,RenderRootDescriptorType::CONSTANT_BUFFER});}
};
struct D3D12Buffer:RenderBuffer{NativeBuffer* d3d;};
struct D3D12CommandQueue{};struct D3D12Framebuffer{};
struct D3D12CommandList:RenderCommandList {
''' + fields + r'''
 void begin();void end();void setPipeline(const RenderPipeline*);
 void setGraphicsPipelineLayout(const RenderPipelineLayout*);
 void setComputePipelineLayout(const RenderPipelineLayout*);
 void setRaytracingPipelineLayout(const RenderPipelineLayout*);
 void notifyCommandStateWasChangedExternally();void notifyDescriptorHeapWasChangedExternally();
 void checkTopology();void checkStencilRef();void resetSamplePositions(){activeSamplePositions=false;}
 void setRootDescriptor(const D3D12PipelineLayout*,RenderBufferReference,uint32_t,bool);
 void setGraphicsRootDescriptor(RenderBufferReference,uint32_t);
};
''' + bodies + r'''
}
int main(int argc,char** argv){
 Check(argc==2,"expected enabled/disabled mode");const bool enabled=std::string(argv[1])=="enabled";
 using namespace plume;
 FakeD3D d3d;ID3D12CommandAllocator allocator;D3D12CommandList list;
 list.d3d=list.d3dV1=list.d3dV4=&d3d;list.commandAllocator=&allocator;
 NativePso gn{101,2},cn{201,1},external_native{999,2};ID3D12StateObject rn{301};
 D3D12GraphicsPipeline graphics(&gn,7,2),graphics_alias(&gn,9,3);
 D3D12ComputePipeline compute(&cn);D3D12RaytracingPipeline ray(&rn);
 ID3D12RootSignature gr{11},cr{21},other{31};
 D3D12PipelineLayout gl(&gr),cl(&cr),other_layout(&other),gl_alias(&gr,2);
 NativeBuffer native_buffer;D3D12Buffer buffer;buffer.d3d=&native_buffer;
 std::vector<std::string> states;
 auto snapshot=[&](const char* label){std::ostringstream out;
  out<<label<<':'<<d3d.pipeline_id<<':'<<d3d.pipeline_kind<<':'
     <<(d3d.graphics_root?d3d.graphics_root->id:0)<<':'<<(d3d.compute_root?d3d.compute_root->id:0)
     <<':'<<d3d.topology<<':'<<d3d.stencil;
  for(auto value:d3d.graphics_args)out<<':'<<value;for(auto value:d3d.compute_args)out<<':'<<value;
  states.push_back(out.str());};
 list.begin();list.setGraphicsPipelineLayout(&gl);list.setComputePipelineLayout(&cl);
 d3d.SetGraphicsRootConstantBufferView(0,1001);d3d.SetComputeRootConstantBufferView(0,2001);
 const auto gr_before=d3d.graphics_root_calls,cr_before=d3d.compute_root_calls;
 list.setGraphicsPipelineLayout(&gl);list.setRaytracingPipelineLayout(&cl);
 Check(d3d.graphics_args[0]==1001&&d3d.compute_args[0]==2001,"same_signature_preserves_bindings");
 Check(d3d.graphics_root_calls==gr_before+uint32_t(!enabled)&&
       d3d.compute_root_calls==cr_before+uint32_t(!enabled),"optin_root_emission_count");
 list.setGraphicsPipelineLayout(&gl_alias);
 Check(list.activeGraphicsPipelineLayout==&gl_alias,"layout_alias_metadata");
 list.setGraphicsRootDescriptor({&buffer,256},0);
 Check(d3d.graphics_args[2]==0x10100,"root_descriptor_uses_latest_layout_metadata");
 list.setGraphicsRootDescriptor({&buffer,512},0);
 Check(d3d.graphics_args[2]==0x10200&&d3d.cbv_calls==3,"dynamic_cbv_not_cached");
 list.setGraphicsPipelineLayout(&other_layout);
 Check(d3d.graphics_args[0]==0&&d3d.compute_args[0]==2001,"graphics_change_compute_independent");
 list.setComputePipelineLayout(&other_layout);
 Check(d3d.compute_args[0]==0,"compute_signature_change_invalidates_compute_bindings");
 list.setGraphicsPipelineLayout(&gl);list.setComputePipelineLayout(&cl);
 snapshot("roots");
 // All nine type transitions, including returning to the original graphics object.
 const std::array<RenderPipeline*,3> pipelines{&graphics,&compute,&ray};
 const std::array<uint32_t,3> ids{101,201,301},kinds{2,1,3};
 for(uint32_t i=0;i<3;++i)for(uint32_t j=0;j<3;++j){
  list.setPipeline(pipelines[i]);list.setPipeline(pipelines[j]);
  Check(d3d.pipeline_id==ids[j]&&d3d.pipeline_kind==kinds[j],"pipeline_transition_state");
  list.setPipeline(&graphics);
  Check(d3d.pipeline_id==101&&d3d.pipeline_kind==2,"pipeline_transition_state");
  snapshot("transition");
 }
 const auto before_alias=d3d.pso_calls;
 list.setPipeline(&graphics_alias);list.checkTopology();list.checkStencilRef();
 Check(list.activeGraphicsPipeline==&graphics_alias&&d3d.topology==9&&d3d.stencil==3,"graphics_alias_metadata");
 Check(d3d.pso_calls==before_alias+uint32_t(!enabled),"underlying_handle_alias_redundant");
 snapshot("alias");
 // External heap notification has no effect on root signature/PSO identity.
 const auto heap_pso=d3d.pso_calls,heap_root=d3d.graphics_root_calls;
 list.descriptorHeapsSet=true;list.notifyDescriptorHeapWasChangedExternally();
 Check(!list.descriptorHeapsSet,"heap_notification_preserved");
 list.setPipeline(&graphics_alias);list.setGraphicsPipelineLayout(&gl);
 Check(d3d.pso_calls==heap_pso+uint32_t(!enabled)&&
       d3d.graphics_root_calls==heap_root+uint32_t(!enabled),"heap_notification_orthogonal");
 // Raw native changes are allowed only with explicit invalidation before rebinding.
 d3d.SetPipelineState(&external_native);d3d.SetGraphicsRootSignature(&other);
 d3d.SetComputeRootSignature(&other);list.notifyCommandStateWasChangedExternally();
 list.setPipeline(&graphics_alias);list.setGraphicsPipelineLayout(&gl);list.setComputePipelineLayout(&cl);
 Check(d3d.pipeline_id==101&&d3d.graphics_root==&gr&&d3d.compute_root==&cr,"external_mutation_state");
 snapshot("external");
 // End/begin Reset reuses the same object and all native handles.
 list.end();list.begin();list.setPipeline(&graphics_alias);
 list.setGraphicsPipelineLayout(&gl);list.setComputePipelineLayout(&cl);
 Check(d3d.pipeline_id==101&&d3d.graphics_root==&gr&&d3d.compute_root==&cr,"reset_rebind_state");
 list.checkTopology();list.checkStencilRef();snapshot("reset");list.end();
 // Null native signatures remain distinguishable from unknown initial state.
 D3D12PipelineLayout null_layout(nullptr);
 list.begin();const auto null_before=d3d.graphics_root_calls;
 list.setGraphicsPipelineLayout(&null_layout);list.setGraphicsPipelineLayout(&null_layout);
 Check(d3d.graphics_root_calls==null_before+(enabled?1u:2u),"unknown_null_signature_first_emit");list.end();
 std::cout<<"{\"pso_calls\":"<<d3d.pso_calls<<",\"graphics_root_calls\":"<<d3d.graphics_root_calls
          <<",\"compute_root_calls\":"<<d3d.compute_root_calls<<",\"states\":[";
 for(size_t i=0;i<states.size();++i){if(i)std::cout<<',';std::cout<<'"'<<states[i]<<'"';}
 std::cout<<"]}\n";
}
'''
    cpp = args.output/'plume-state-bindings.cpp'
    cpp.write_text(code)
    exe = args.output/'plume-state-bindings.exe'
    subprocess.run([args.compiler, '-std=c++20', '-O2', str(cpp), '-o', str(exe)],
                   check=True, timeout=45)
    def run(value, enabled):
        environment = dict(os.environ)
        environment.pop('PLUME_D3D12_STATE_CACHE', None)
        if value is not None:
            environment['PLUME_D3D12_STATE_CACHE'] = value
        return subprocess.run([str(exe.resolve()), 'enabled' if enabled else 'disabled'],
                              env=environment, capture_output=True, text=True, timeout=10)
    if args.negative_control:
        result = run('1', True)
        oracle = {'graphics-key':'pipeline_transition_state', 'reset':'reset_rebind_state',
                  'external':'external_mutation_state', 'metadata':'layout_alias_metadata'}[args.negative_control]
        if result.returncode != 17 or oracle not in result.stderr:
            raise RuntimeError('Negative control missed its exact oracle: '+result.stderr)
        print('PASS: '+args.negative_control+' fails '+oracle)
        return
    reference = None
    records = {}
    for value in (None, '', '0', 'true', '10', '1'):
        result = run(value, value == '1')
        if result.returncode:
            raise RuntimeError(result.stderr)
        expected_state = 'enabled' if value == '1' else 'disabled'
        if result.stderr != 'Plume D3D12 command state cache: '+expected_state+'\n':
            raise AssertionError('Actual activation evidence must appear exactly once per process')
        record = json.loads(result.stdout)
        records['absent' if value is None else value] = record
        if reference is None:
            reference = record
        if value != '1' and record != reference:
            raise AssertionError('Default/invalid flag differs from original API behavior')
        if record['states'] != reference['states']:
            raise AssertionError('Enabled/disabled actual draw-time state differs')
    assert records['1']['pso_calls'] < reference['pso_calls']
    assert records['1']['graphics_root_calls'] < reference['graphics_root_calls']
    assert records['1']['compute_root_calls'] < reference['compute_root_calls']
    report = {'passed':True, 'flag':'PLUME_D3D12_STATE_CACHE=1; exact1 only',
              'source_sha256':hashlib.sha256(source.encode()).hexdigest(),
              'header_sha256':hashlib.sha256(header.encode()).hexdigest(),
              'actual_functions':list(signatures), 'records':records,
              'boundary':'fake D3D recorder implements documented root/pipeline semantics; no GPU/fence/FPS claim'}
    (args.output/'verification.json').write_text(json.dumps(report,indent=2)+'\n')
    print('PASS: actual Plume bodies, nine type transitions, native-handle aliases, root independence/preservation, Reset and notified external mutations')


if __name__ == '__main__':
    main()
