"""Check production depth format selection against the pinned format enums."""
import argparse
from pathlib import Path
import re
import subprocess


def body(source, signature):
    start = source.index(signature)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    parser.add_argument('--compiler', default='clang++')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    types = (root/'thirdparty/plume/plume_render_interface_types.h').read_text()
    xenos = (root/'rexglue-sdk/include/rex/graphics/xenos.h').read_text()
    formats = (root/'rexlego/src/gpu_native/format.cpp').read_text()
    formats = re.sub(r'^#include.*$', '', formats, flags=re.M)
    code = '''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
using u32=uint32_t;
#define REXLOG_WARN(...) ((void)0)
namespace plume {
''' + body(types, 'enum class RenderFormat ') + ';\n' + \
        body(types, 'constexpr uint32_t RenderFormatSize(') + '\n' + \
        body(types, 'constexpr bool RenderFormatIsStencil(') + '\n}\n' + \
        'namespace rex::graphics::xenos {\n' + body(xenos, 'enum class TextureFormat ') + ';\n}\n' + formats + '''
int main(){
 using namespace legodimensions::gpu_native;
 using F=plume::RenderFormat;
 // Owner rejected the UNORM24 storage candidate. Keep the working D32 alias
 // representation until the complete depth/alias chain is verified visually.
 assert(ConvertGuestTextureFormat(0x2D200196)==F::D32_FLOAT_S8_UINT);
 assert(ConvertGuestTextureFormat(0x1A220197)==F::D32_FLOAT_S8_UINT);
 assert(ConvertXenosTextureFormat(22)==F::D32_FLOAT_S8_UINT);
 assert(ConvertXenosTextureFormat(23)==F::D32_FLOAT_S8_UINT);
 for(auto f:{F::D24_UNORM_S8_UINT,F::D32_FLOAT_S8_UINT}){
  assert(IsDepthFormat(f)&&plume::RenderFormatIsStencil(f));
  assert(!IsRenderTargetFormat(f)&&!IsBlockCompressedFormat(f));
 }
 assert(plume::RenderFormatSize(F::D24_UNORM_S8_UINT)==4);
 assert(plume::RenderFormatSize(F::D32_FLOAT_S8_UINT)==8);
 assert(!IsDepthFormat(F::R32_FLOAT));
}
'''
    args.output.mkdir(parents=True, exist_ok=True)
    source = args.output.resolve()/'depth-format.cpp'
    binary = args.output.resolve()/'depth-format.exe'
    source.write_text(code)
    subprocess.run([args.compiler, '-std=c++20', '-UNDEBUG', str(source), '-o', str(binary)],
                   check=True, timeout=45)
    subprocess.run([str(binary)], check=True, timeout=10)
    print('Production compatibility depth mapping and stencil storage checks PASS')


if __name__ == '__main__':
    main()
