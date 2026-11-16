"""Exercise the real Plume shader-query body with failed/rejected adapters."""
import argparse
import json
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('output', type=Path)
parser.add_argument('--source', type=Path, default=Path(__file__).resolve().parents[2] / 'thirdparty/plume/plume_d3d12.cpp')
parser.add_argument('--compiler', default='clang-cl')
parser.add_argument('--expect-failure', action='store_true')
args = parser.parse_args()
args.output.mkdir(parents=True, exist_ok=True)
text = args.source.read_text()
body = text[text.index('            // Determine the shader model supported by the device.'):text.index('            // Determine if the device supports sample locations.')]
prefix = r'''
#include <cstdint>
#include <cstdio>
#include <cstdlib>
using HRESULT = int32_t;
constexpr HRESULT S_OK=0, E_FAIL=int32_t(0x80004005), E_INVALIDARG=int32_t(0x80070057);
#define FAILED(x) ((x)<0)
#define SUCCEEDED(x) ((x)>=0)
#define _countof(x) (sizeof(x)/sizeof((x)[0]))
enum D3D_SHADER_MODEL { D3D_SHADER_MODEL_5_1=0x51, D3D_SHADER_MODEL_6_0=0x60 };
constexpr int D3D12_FEATURE_SHADER_MODEL=0;
struct D3D12_FEATURE_DATA_SHADER_MODEL { D3D_SHADER_MODEL HighestShaderModel; };
static void Check(bool value) { if(!value) std::exit(23); }
struct Adapter { unsigned releases=0; void Release() { Check(++releases==1); } };
struct Device : Adapter {
  HRESULT status=S_OK; D3D_SHADER_MODEL reported=D3D_SHADER_MODEL_6_0; unsigned calls=0;
  HRESULT CheckFeatureSupport(int,void* data,unsigned) {
    Check(!releases); ++calls;
    auto& q=*static_cast<D3D12_FEATURE_DATA_SHADER_MODEL*>(data);
    if(status==E_INVALIDARG && q.HighestShaderModel==D3D_SHADER_MODEL_5_1) {
      q.HighestShaderModel=D3D_SHADER_MODEL_5_1; return S_OK;
    }
    q.HighestShaderModel=reported; return status;
  }
};
static void Run(HRESULT status,D3D_SHADER_MODEL reported,bool accepted) {
  Device devices[2]; Adapter adapters[2]; devices[0].status=status;devices[0].reported=reported;
  unsigned picked=99;
  for(unsigned option=0;option<2;++option) {
    auto* deviceOption=&devices[option]; auto* adapterOption=&adapters[option]; HRESULT res;
'''
suffix = r'''
    Check(!deviceOption->releases && !adapterOption->releases);
    picked=option; break;
  }
  Check(picked==(accepted?0u:1u));
  for(unsigned i=0;i<2;++i) {
    if(!devices[i].releases)devices[i].Release();
    if(!adapters[i].releases)adapters[i].Release();
    Check(devices[i].releases==1 && adapters[i].releases==1);
  }
}
int main() {
  Run(S_OK,D3D_SHADER_MODEL_6_0,true);
  Run(E_FAIL,D3D_SHADER_MODEL_6_0,false);
  Run(S_OK,D3D_SHADER_MODEL_5_1,SM_5_1_SUPPORTED!=0);
  Run(E_INVALIDARG,D3D_SHADER_MODEL_6_0,SM_5_1_SUPPORTED!=0);
  std::puts("PASS: actual Plume query skips failed adapter, selects next adapter, and releases once");
}
'''
source = args.output / 'adapter-query.cpp'
source.write_text(prefix + body + suffix)
results = []
for mode in (0, 1):
    executable = args.output / f'adapter-query-{mode}.exe'
    subprocess.run([args.compiler, str(source), '/std:c++20', '/O2', '/EHsc', f'/DSM_5_1_SUPPORTED={mode}',
                    f'/Fo{args.output}/', f'/Fe{executable}'], check=True, timeout=60, capture_output=True, text=True)
    result = subprocess.run([str(executable)], capture_output=True, text=True, timeout=10)
    if result.returncode != (23 if args.expect_failure else 0):
        raise AssertionError(f'Unexpected exit {result.returncode}: {result.stdout} {result.stderr}')
    results.append(dict(sm51=bool(mode), exit_code=result.returncode, stdout=result.stdout))
(args.output/'verification.json').write_text(json.dumps(dict(game_launched=False, actual_query_body=True,
    mocked_adapter_boundary=True, negative_control=args.expect_failure, results=results), indent=2))
print('PASS: adapter query negative control' if args.expect_failure else 'PASS: both shader-model configurations')
