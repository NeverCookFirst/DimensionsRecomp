"""Check actual D3D12 state conversion against the GRAPHICS stage contract."""
from pathlib import Path
import argparse,subprocess,json,hashlib
def function(text,signature):
 start=text.index(signature);end=text.index('{',start)+1;depth=1
 while depth:
  depth+=(text[end]=='{')-(text[end]=='}');end+=1
 return text[start:end]
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('output',type=Path)
 p.add_argument('--source',type=Path);p.add_argument('--expect-failure',action='store_true');a=p.parse_args()
 root=Path(__file__).resolve().parents[2];out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
 source=a.source or root/'thirdparty/plume/plume_d3d12.cpp';text=source.read_text()
 code='''#include <d3d12.h>
#include <plume_render_interface_types.h>
#include <cstdlib>
#include <cstdio>
using namespace plume;
'''+function(text,'static D3D12_RESOURCE_STATES toBufferState(')+'\n'+function(text,'static D3D12_RESOURCE_STATES toTextureState(')+r'''
void Check(bool ok){if(!ok)std::exit(23);}
int main(){
 using namespace plume;
 const auto graph=RenderBarrierStage::GRAPHICS,compute=RenderBarrierStage::COMPUTE;
 const auto read=RenderBufferAccess::READ;
 const auto all=D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
 Check(toTextureState(graph,RenderTextureLayout::SHADER_READ,0)==all);
 Check(toBufferState(graph,read,0)==all);
 Check(toTextureState(compute,RenderTextureLayout::SHADER_READ,0)==D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
 Check(toTextureState(graph|compute,RenderTextureLayout::SHADER_READ,0)==all);
 Check(toBufferState(graph|compute,read,0)==all);
 for(auto flags:{RenderBufferFlag::VERTEX,RenderBufferFlag::CONSTANT})
  Check(toBufferState(graph,read,flags)==D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
 Check(toBufferState(graph,read,RenderBufferFlag::INDEX)==D3D12_RESOURCE_STATE_INDEX_BUFFER);
 Check(toBufferState(RenderBarrierStage::COPY,read,0)==D3D12_RESOURCE_STATE_COPY_SOURCE);
 Check(toBufferState(RenderBarrierStage::COPY,RenderBufferAccess::WRITE,0)==D3D12_RESOURCE_STATE_COPY_DEST);
 Check(toBufferState(graph,RenderBufferAccess::WRITE,RenderBufferFlag::UNORDERED_ACCESS)==D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
 Check(toTextureState(graph,RenderTextureLayout::COLOR_WRITE,0)==D3D12_RESOURCE_STATE_RENDER_TARGET);
 Check(toTextureState(graph,RenderTextureLayout::DEPTH_WRITE,0)==D3D12_RESOURCE_STATE_DEPTH_WRITE);
 Check(toTextureState(graph,RenderTextureLayout::DEPTH_READ,0)==(all|D3D12_RESOURCE_STATE_DEPTH_READ));
 Check(toTextureState(RenderBarrierStage::COPY,RenderTextureLayout::COPY_DEST,0)==D3D12_RESOURCE_STATE_COPY_DEST);
 Check(toTextureState(RenderBarrierStage::COPY,RenderTextureLayout::COPY_SOURCE,0)==D3D12_RESOURCE_STATE_COPY_SOURCE);
 std::puts("PASS: GRAPHICS SRVs support vertex/pixel reads; dedicated buffer, compute, copy, depth and write states retained");
}
'''
 cpp=out/'graphics-read.cpp';cpp.write_text(code);exe=out/'graphics-read.exe'
 r=subprocess.run(['clang-cl',str(cpp),'/std:c++20','/O2','/MD','/EHsc',
  '/I'+str(root/'thirdparty/plume'),'/Fo'+out.as_posix()+'/', '/Fe'+str(exe)],capture_output=True,text=True,timeout=60)
 (out/'compile.log').write_text(r.stdout+r.stderr);r.check_returncode()
 r=subprocess.run([str(exe)],capture_output=True,text=True,timeout=15)
 (out/'result.log').write_text(r.stdout+r.stderr)
 assert r.returncode==(23 if a.expect_failure else 0),(r.returncode,r.stdout,r.stderr)
 (out/'verification.json').write_text(json.dumps(dict(passed=True,expected_failure=a.expect_failure,
  exit=r.returncode,source_sha256=hashlib.sha256(source.read_bytes()).hexdigest(),actual_gpu=False,output=r.stdout),indent=2))
 print('PASS: old GRAPHICS mapping rejected' if a.expect_failure else r.stdout.strip())
if __name__=='__main__':main()
