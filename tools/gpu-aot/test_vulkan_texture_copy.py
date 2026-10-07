"""Exercise the actual Vulkan texture-copy body and public footprint types without a GPU.

The Vulkan dispatch is recorded, not emulated. Real pixel/coherence coverage is
provided separately by the bounded headless Vulkan ABI prototype.
"""
import argparse
import hashlib
import json
from pathlib import Path
import resource
import signal
import subprocess


def function(source, signature):
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
    parser.add_argument('--source', type=Path)
    parser.add_argument('--vulkan-include', type=Path,
                        help='Vulkan header include root (defaults to public Plume contrib/Vulkan-Headers/include)')
    parser.add_argument('--verify-regression', action='store_true')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    plume = root / 'thirdparty/plume'
    vulkan_include = args.vulkan_include or plume / 'contrib/Vulkan-Headers/include'
    if not (vulkan_include / 'vulkan/vulkan_core.h').is_file():
        parser.error('Vulkan headers unavailable; initialize Plume contrib headers or set --vulkan-include')
    path = args.source or plume / 'plume_vulkan.cpp'
    production = path.read_text()
    helpers = '\n'.join(function(production, sig) for sig in (
        'static VkImageLayout toImageLayout(', 'static VkImageAspectFlags toViewAspectFlags(',
        'static VkImageAspectFlags toAspectFlags('))
    signature = 'void VulkanCommandList::copyTextureRegion('
    actual = function(production, signature)
    harness = r'''
#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <vector>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>
#include "plume_render_interface_types.h"
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr,"copy_oracle line %d: %s\n",__LINE__,#c); std::exit(17); } } while (0)
namespace plume {
struct RenderBuffer { virtual ~RenderBuffer() = default; };
struct RenderTexture { virtual ~RenderTexture() = default; };
struct VulkanBuffer: RenderBuffer { VkBuffer vk = (VkBuffer)uintptr_t(0x111); RenderBufferDesc desc; };
struct VulkanTexture: RenderTexture { VkImage vk = (VkImage)uintptr_t(0x222); RenderTextureDesc desc; RenderTextureLayout textureLayout=RenderTextureLayout::COPY_SOURCE; };
struct VulkanCommandQueue { RenderCommandListType type=RenderCommandListType::DIRECT; };
struct Record { int kind; VkBufferImageCopy buffer{}; VkImageCopy image{}; VkImage src{},dst{}; VkBuffer srcBuffer{},dstBuffer{}; VkImageLayout srcLayout{},dstLayout{}; };
std::vector<Record> records;
void vkCmdCopyBufferToImage(VkCommandBuffer, VkBuffer b, VkImage i, VkImageLayout layout, uint32_t n, const VkBufferImageCopy* c) { CHECK(n==1); Record r{};r.kind=1;r.buffer=*c;r.dst=i;r.srcBuffer=b;r.dstLayout=layout;records.push_back(r); }
void vkCmdCopyImageToBuffer(VkCommandBuffer, VkImage i, VkImageLayout layout, VkBuffer b, uint32_t n, const VkBufferImageCopy* c) { CHECK(n==1); Record r{};r.kind=2;r.buffer=*c;r.src=i;r.dstBuffer=b;r.srcLayout=layout;records.push_back(r); }
void vkCmdCopyImage(VkCommandBuffer,VkImage s,VkImageLayout sl,VkImage d,VkImageLayout dl,uint32_t n,const VkImageCopy* c) { CHECK(n==1);Record r{};r.kind=3;r.image=*c;r.src=s;r.dst=d;r.srcLayout=sl;r.dstLayout=dl;records.push_back(r); }
struct VulkanCommandList { VkCommandBuffer vk{};VulkanCommandQueue ownedQueue;VulkanCommandQueue* queue=&ownedQueue;int closures=0;void endActiveRenderPass(){++closures;}void copyTextureRegion(const RenderTextureCopyLocation&,const RenderTextureCopyLocation&,uint32_t=0,uint32_t=0,uint32_t=0,const RenderBox*=nullptr); };
@HELPERS@
@BODY@
}
using namespace plume;
int main() {
 VulkanCommandList list;VulkanTexture image,destination;VulkanBuffer buffer;
 image.desc=RenderTextureDesc::Texture(RenderTextureDimension::TEXTURE_2D,32,16,1,3,4,RenderFormat::R8G8B8A8_UNORM);
 destination.desc=image.desc;destination.textureLayout=RenderTextureLayout::COPY_DEST;destination.vk=(VkImage)uintptr_t(0x333);
 buffer.desc=RenderBufferDesc::ReadbackBuffer(65536);
 auto src=RenderTextureCopyLocation::Subresource(&image,1,2);
 auto dst=RenderTextureCopyLocation::PlacedFootprint(&buffer,RenderFormat::R8G8B8A8_UNORM,24,16,3,32,512);
 // Existing upload and image-copy routes retain their original semantics.
 auto upload=RenderTextureCopyLocation::PlacedFootprint(&buffer,RenderFormat::R8G8B8A8_UNORM,7,5,1,10,256);
 list.copyTextureRegion(RenderTextureCopyLocation::Subresource(&destination,1,3),upload,3,2,0);
 CHECK(records.size()==1&&records.back().kind==1);auto u=records.back();
 CHECK(u.buffer.bufferOffset==256&&u.buffer.bufferRowLength==10&&u.buffer.bufferImageHeight==5);
 CHECK(u.buffer.imageExtent.width==7&&u.buffer.imageExtent.height==5&&u.buffer.imageExtent.depth==1);
 CHECK(u.buffer.imageOffset.x==3&&u.buffer.imageOffset.y==2&&u.buffer.imageSubresource.mipLevel==1&&u.buffer.imageSubresource.baseArrayLayer==3);
 CHECK(u.srcBuffer==buffer.vk&&u.dst==destination.vk&&u.dstLayout==VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
 RenderBox box(2,1,10,5,0,1);
 list.copyTextureRegion(RenderTextureCopyLocation::Subresource(&destination,0,1),src,7,8,0,&box);
 CHECK(records.size()==2&&records.back().kind==3);auto t=records.back();
 CHECK(t.image.srcOffset.x==2&&t.image.srcOffset.y==1&&t.image.extent.width==8&&t.image.extent.height==4);
 CHECK(t.image.srcSubresource.mipLevel==1&&t.image.srcSubresource.baseArrayLayer==2&&t.image.dstSubresource.mipLevel==0&&t.image.dstSubresource.baseArrayLayer==1);
 CHECK(t.image.dstOffset.x==7&&t.image.dstOffset.y==8&&t.srcLayout==VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL&&t.dstLayout==VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
 // Missing stock route: full selected mip and array layer, padded texel pitch.
 std::puts("BEGIN stock readback null-image regression");std::fflush(stdout);
 list.copyTextureRegion(dst,src);
 CHECK(records.size()==3&&records.back().kind==2);auto r=records.back();
 CHECK(r.buffer.bufferOffset==512&&r.buffer.bufferRowLength==32&&r.buffer.bufferImageHeight==16);
 CHECK(r.buffer.imageExtent.width==16&&r.buffer.imageExtent.height==8&&r.buffer.imageExtent.depth==1);
 CHECK(r.buffer.imageSubresource.mipLevel==1&&r.buffer.imageSubresource.baseArrayLayer==2&&r.buffer.imageSubresource.layerCount==1);
 CHECK(r.src==image.vk&&r.dstBuffer==buffer.vk&&r.srcLayout==VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
 // Source box, destination XYZ and independently calculated row/slice offset.
 image.desc=RenderTextureDesc::Texture3D(32,16,8,3,RenderFormat::R8G8B8A8_UNORM);src=RenderTextureCopyLocation::Subresource(&image,1);
 box=RenderBox(2,1,10,5,0,2);list.copyTextureRegion(dst,src,4,2,1,&box);r=records.back();
 CHECK(r.kind==2&&r.buffer.bufferOffset==2832&&r.buffer.imageExtent.depth==2);
 CHECK(r.buffer.imageOffset.x==2&&r.buffer.imageOffset.y==1&&r.buffer.imageOffset.z==0);
 // BC1: pitches are texels rounded to4; byte offset counts8-byte blocks.
 image.desc=RenderTextureDesc::Texture2D(10,7,1,RenderFormat::BC1_UNORM);src=RenderTextureCopyLocation::Subresource(&image);
 dst=RenderTextureCopyLocation::PlacedFootprint(&buffer,RenderFormat::BC1_UNORM,18,15,1,21,1024);
 list.copyTextureRegion(dst,src,4,4);r=records.back();
 CHECK(r.buffer.bufferOffset==1080&&r.buffer.bufferRowLength==24&&r.buffer.bufferImageHeight==16);
 CHECK(r.buffer.imageExtent.width==10&&r.buffer.imageExtent.height==7);
 box=RenderBox(4,4,10,7);list.copyTextureRegion(dst,src,8,8,0,&box);r=records.back();
 CHECK(r.buffer.bufferOffset==1136&&r.buffer.imageExtent.width==6&&r.buffer.imageExtent.height==3);
 image.desc.format=RenderFormat::BC3_UNORM;dst.placedFootprint.format=RenderFormat::BC3_UNORM;
 list.copyTextureRegion(dst,src,4,4);r=records.back();
 CHECK(r.buffer.bufferOffset==1136&&r.buffer.bufferRowLength==24&&r.buffer.bufferImageHeight==16);
 destination.desc=image.desc;
 auto bcUpload=dst;bcUpload.placedFootprint.width=10;bcUpload.placedFootprint.height=7;
 list.copyTextureRegion(RenderTextureCopyLocation::Subresource(&destination),bcUpload);r=records.back();
 CHECK(r.kind==1&&r.buffer.bufferRowLength==24&&r.buffer.bufferImageHeight==8);
 // Single depth aspect is representable; combined depth/stencil is refused.
 image.desc=RenderTextureDesc::Texture2D(4,3,1,RenderFormat::D32_FLOAT,RenderTextureFlag::DEPTH_TARGET);src=RenderTextureCopyLocation::Subresource(&image);
 dst=RenderTextureCopyLocation::PlacedFootprint(&buffer,RenderFormat::D32_FLOAT,4,3,1,8,256);
 list.copyTextureRegion(dst,src);r=records.back();CHECK(r.buffer.imageSubresource.aspectMask==VK_IMAGE_ASPECT_DEPTH_BIT&&r.buffer.bufferRowLength==8);
 auto reject=[&](const RenderTextureCopyLocation& d,const RenderTextureCopyLocation& s,uint32_t x=0,uint32_t y=0,uint32_t z=0,const RenderBox* b=nullptr){size_t count=records.size();int closures=list.closures;list.copyTextureRegion(d,s,x,y,z,b);CHECK(records.size()==count&&list.closures==closures+1);};
 image.desc.format=RenderFormat::D24_UNORM_S8_UINT;dst.placedFootprint.format=RenderFormat::D24_UNORM_S8_UINT;reject(dst,src);
 image.desc=RenderTextureDesc::Texture2D(8,8,1,RenderFormat::R8G8B8A8_UNORM);src=RenderTextureCopyLocation::Subresource(&image);
 dst=RenderTextureCopyLocation::PlacedFootprint(&buffer,RenderFormat::R8G8B8A8_UNORM,8,8,1,8,256);
 auto small=dst;small.placedFootprint.width=7;reject(small,src);
 small=dst;small.placedFootprint.format=RenderFormat::UNKNOWN;reject(small,src);
 small=dst;small.placedFootprint.format=RenderFormat::MAX;reject(small,src);
 small=dst;small.placedFootprint.format=RenderFormat(9999);reject(small,src);
 image.desc.format=RenderFormat::UNKNOWN;reject(dst,src);image.desc.format=RenderFormat::R8G8B8A8_UNORM;
 image.desc.multisampling.sampleCount=RenderSampleCount::COUNT_4;reject(dst,src);image.desc.multisampling.sampleCount=RenderSampleCount::COUNT_1;
 list.queue=nullptr;reject(dst,src);list.queue=&list.ownedQueue;
 list.queue->type=RenderCommandListType::UNKNOWN;reject(dst,src);list.queue->type=RenderCommandListType::DIRECT;
 small=dst;small.placedFootprint.rowWidth=536870912;reject(small,src); // 2GiB row pitch VUID09108.
 small=dst;small.placedFootprint.rowWidth=7;reject(small,src);
 small=dst;small.placedFootprint.offset=UINT64_MAX-3;reject(small,src);
 small=dst;small.placedFootprint.offset=258;reject(small,src);
 small=dst;small.placedFootprint.rowWidth=UINT32_MAX;small.placedFootprint.height=UINT32_MAX;reject(small,src);
 small=dst;small.buffer=nullptr;reject(small,src);
 reject(dst,RenderTextureCopyLocation::Subresource(&image,1));
 reject(dst,RenderTextureCopyLocation::Subresource(&image,0,1));
 box=RenderBox(-1,0,4,4);reject(dst,src,0,0,0,&box);
 box=RenderBox(0,0,9,4);reject(dst,src,0,0,0,&box);
 reject(dst,src,1);reject(dst,src,0,0,1);
 buffer.desc.size=511;reject(dst,src); // needs exactly512 bytes incl prefix.
 buffer.desc.size=512;list.copyTextureRegion(dst,src);CHECK(records.back().buffer.bufferOffset==256);
 // Large unused row/slice padding does not require allocation past the last
 // copied byte (D3D12 footprints also omit the final row's trailing padding).
 image.desc=RenderTextureDesc::Texture2D(1,1,1,RenderFormat::R8G8B8A8_UNORM);
 dst=RenderTextureCopyLocation::PlacedFootprint(&buffer,RenderFormat::R8G8B8A8_UNORM,1,64,1,256,0);
 buffer.desc.size=4;list.copyTextureRegion(dst,src);r=records.back();
 CHECK(r.buffer.bufferOffset==0&&r.buffer.bufferRowLength==256&&r.buffer.bufferImageHeight==64&&r.buffer.imageExtent.width==1);
 buffer.desc.size=3;reject(dst,src);
 // D16 has two-byte texels but its final copy offset must align to4.
 buffer.desc.size=4096;image.desc=RenderTextureDesc::Texture2D(1,1,1,RenderFormat::D16_UNORM,RenderTextureFlag::DEPTH_TARGET);
 dst=RenderTextureCopyLocation::PlacedFootprint(&buffer,RenderFormat::D16_UNORM,4,1,1,4,0);
 reject(dst,src,1);list.copyTextureRegion(dst,src,2);CHECK(records.back().buffer.bufferOffset==4);
 dst.placedFootprint.offset=2;list.copyTextureRegion(dst,src,1);CHECK(records.back().buffer.bufferOffset==4);
 list.queue->type=RenderCommandListType::COMPUTE;reject(dst,src);list.queue->type=RenderCommandListType::DIRECT;
 dst.placedFootprint.format=RenderFormat::R32_FLOAT;reject(dst,src); // Depth footprint texel size mismatch.
 // COPY queue uses4-byte offsets even for byte-wide color formats. Check
 // destination X, Y and Z, and permit a final offset aligned by dstX.
 image.desc=RenderTextureDesc::Texture2D(1,1,1,RenderFormat::R8_UNORM);
 dst=RenderTextureCopyLocation::PlacedFootprint(&buffer,RenderFormat::R8_UNORM,4,2,2,5,0);
 list.queue->type=RenderCommandListType::COPY;reject(dst,src,1);reject(dst,src,0,1);reject(dst,src,0,0,1);
 list.copyTextureRegion(dst,src);CHECK(records.back().buffer.bufferOffset==0);
 dst.placedFootprint.offset=1;list.copyTextureRegion(dst,src,3);CHECK(records.back().buffer.bufferOffset==4);
 list.queue->type=RenderCommandListType::DIRECT;list.copyTextureRegion(dst,src);CHECK(records.back().buffer.bufferOffset==1);
 // No fallback may reinterpret a second buffer as an image.
 reject(dst,dst);auto unknown=dst;unknown.type=RenderTextureCopyType::UNKNOWN;reject(unknown,src);
 std::puts("PASS actual copy body: upload/image/readback, mip/layer/3D/BC1/depth/offset/bounds/release guards");
}
'''.replace('const RenderBox*=nullptr', 'const RenderBox* = nullptr')
    args.output.mkdir(parents=True, exist_ok=True)
    def limit():
        resource.setrlimit(resource.RLIMIT_AS, (3 * 1024**3, 3 * 1024**3))
        resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    def run(name, body):
        cpp = args.output / (name + '.cpp')
        exe = args.output / name
        cpp.write_text(harness.replace('@HELPERS@', helpers).replace('@BODY@', body))
        compiled = subprocess.run([args.compiler, '-std=c++17', '-O0', '-DNDEBUG',
            '-I', str(plume), '-I', str(vulkan_include),
            str(cpp), '-o', str(exe)], capture_output=True, text=True, timeout=45, preexec_fn=limit)
        if compiled.returncode:
            raise RuntimeError(compiled.stdout + compiled.stderr)
        return subprocess.run([str(exe.resolve())], capture_output=True, text=True, timeout=20, preexec_fn=limit)
    positive = run('copy', actual)
    if positive.returncode:
        raise RuntimeError(positive.stdout + positive.stderr)
    negative = None
    if args.verify_regression:
        old = subprocess.check_output(['git', '-C', str(plume), 'show', 'e0c8871b930dc1a6544bf457f2348c456b9d7e13:plume_vulkan.cpp'], text=True, timeout=10)
        failed = run('copy-stock-negative', function(old, signature))
        if failed.returncode != -signal.SIGSEGV or 'BEGIN stock readback null-image regression' not in failed.stdout:
            raise RuntimeError('Stock negative must reach named readback and fail with SIGSEGV: ' + repr(failed))
        negative = {'returncode': failed.returncode, 'stdout': failed.stdout, 'stderr': failed.stderr}
    report = {'status': 'passed', 'production_sha256': hashlib.sha256(path.read_bytes()).hexdigest(),
              'positive': positive.stdout, 'stock_negative': negative,
              'scope': 'Actual production copy/format/layout bodies with mocked Vulkan dispatch; no GPU execution'}
    (args.output / 'verification.json').write_text(json.dumps(report, indent=2) + '\n')
    print(positive.stdout, end='')
    if negative:
        print('PASS stock image-to-buffer negative, exit', negative['returncode'])


if __name__ == '__main__':
    main()
