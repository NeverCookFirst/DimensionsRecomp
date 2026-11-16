#include "authority-prefix.inc"
#include "make-image.inc"
#include "readonly-authority.inc"
int main(){
 rex::memory::Memory sdk_memory;CHECK(sdk_memory.Initialize());fixture::sdk_memory=&sdk_memory;
 memory.data={sdk_memory.TranslatePhysical<u8*>(0),1u<<20};
 coordinator=std::this_thread::get_id();auto api=plume::CreateD3D12Interface();device=api->createDevice();CHECK(device);
 queue=device->createCommandQueue(plume::RenderCommandListType::DIRECT);commands=queue->createCommandList();fence=device->createCommandFence();
 rex::graphics::texture_util::layout={{32768,512},0};
 std::fill(memory.data.begin(),memory.data.end(),0x55);auto expected=std::vector<u8>(memory.data.begin(),memory.data.end());
 commands->begin();open=true;auto a=MakeImage(0,0,3,true,2,false,expected);
 auto b=MakeImage(1,0,3,false,2,true,expected);
 auto* heap=sdk_memory.LookupHeap(0xC0000000);const u32 base=a.resource->guest_fetch.base_address<<12;
 CHECK(heap->Protect(0xC0000000+base,32768,rex::memory::kMemoryProtectRead));
 std::array owners{a.resource,b.resource};Recorder([&]{CHECK(MaterializeResolvedTextures(owners));});
 BytesMatch(expected);CHECK(CpuMemoryUnchanged(a.resource->cpu_stamp)&&CpuMemoryUnchanged(b.resource->cpu_stamp));
 uint32_t access=0;CHECK(heap->QueryProtect(0xC0000000+base,&access)&&access==rex::memory::kMemoryProtectRead);
 {std::lock_guard lock(a.resource->mutex);RefreshPublishedTextureAuthority(*a.resource);CHECK(a.resource->resolved_on_host);}
 const bool combined=std::getenv("LEGO_TEST_WRITECOMBINE")!=nullptr;
 CHECK(heap->Protect(0xC0000000+base,32768,rex::memory::kMemoryProtectRead|
   (combined?rex::memory::kMemoryProtectWriteCombine:rex::memory::kMemoryProtectWrite)));
 // C's guest protection page is 16 MiB: its host protection update retires
 // watches for both allocations. They must be invalid before any CPU write.
 CHECK(!CpuMemoryUnchanged(a.resource->cpu_stamp)&&!CpuMemoryUnchanged(b.resource->cpu_stamp));
 const CpuMemorySpan neighbor{b.resource->guest_fetch.base_address<<12,32768};
 b.resource->cpu_stamp=WatchCpuMemory({&neighbor,1});CHECK(CpuMemoryUnchanged(b.resource->cpu_stamp));
 *sdk_memory.TranslateVirtual<volatile u8*>(0xC0000000+base+7)^=0x80;
 CHECK(!CpuMemoryUnchanged(a.resource->cpu_stamp)&&CpuMemoryUnchanged(b.resource->cpu_stamp));
 {std::lock_guard lock(a.resource->mutex);RefreshPublishedTextureAuthority(*a.resource);CHECK(!a.resource->resolved_on_host&&!a.resource->sampled_valid);}
 ShutdownCpuMemoryWatch();g_textures.clear();retained.clear();
 std::puts("PASS real RO GPU publication + armed write watch, RO-to-RW CPU write invalidation, clean neighbor and authority handoff");
}
