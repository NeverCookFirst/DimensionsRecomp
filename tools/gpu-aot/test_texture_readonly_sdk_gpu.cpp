#include "readonly-prefix.inc"
#include "make-image.inc"
#ifdef LEGO_TEST_NATIVE_PROVIDER
#include <rex/system/memory_data_provider.h>
#include <condition_variable>
// Recorder executes on another thread. Mark that thread too: a protected
// header read there recursively enters this provider while its caller holds
// delivery and waits for Recorder. A callback-thread-only guard cannot detect
// this cycle, so an invalid implementation would merely time out.
static thread_local bool materializing_provider_data=false;
// Prototype grouping only: production owns discovery, coverage, raw readback
// and publication; this fixture supplies a stable set of pending GPU owners.
struct NativeOwnerGroup {
 rex::memory::Memory& memory;
 std::vector<std::shared_ptr<TextureResource>> owners;
 std::vector<PhysicalCopyRange> spans;
 std::atomic<bool> pending=true;
 std::atomic<unsigned> callbacks=0;
 std::mutex delivery,mutex;std::condition_variable condition;
 bool release=false;unsigned waits=0;
 static bool Call(std::unique_lock<std::recursive_mutex>& global,void* context,uint32_t physical,
                  rex::memory::PhysicalProviderOperation operation){
  auto& group=*static_cast<NativeOwnerGroup*>(context);
  if(operation==rex::memory::PhysicalProviderOperation::kQuery){
   if(!group.pending)return false;
   return std::any_of(group.spans.begin(),group.spans.end(),[&](auto span){
    return CopyRangesOverlap(span,{physical&~4095u,4096});
   });
  }
  static thread_local bool providing=false;
  if(providing||materializing_provider_data){std::fputs("REENTERED_NATIVE_PROVIDER_DURING_HEADER_VALIDATION\n",stderr);std::exit(23);}
  providing=true;
  ++group.callbacks;
  global.unlock();
  // All guest readers must reach their real SDK fault callbacks before the
  // first one is allowed to complete its GPU work.
  {std::unique_lock lock(group.mutex);group.condition.notify_all();group.condition.wait(lock,[&]{return group.release;});}
  std::unique_lock delivery(group.delivery);bool done=true;
  if(group.pending){
   Recorder([&]{
    materializing_provider_data=true;
    done=MaterializeResolvedTextures(group.owners);
    materializing_provider_data=false;
   });
   if(done)++group.waits;
  }
  global.lock();
  if(done&&group.pending.exchange(false))for(auto span:group.spans)group.memory.DisablePhysicalMemoryDataProviders(span.address,span.length);
  providing=false;return done;
 }
};
#endif

DWORD Protection(void* address){MEMORY_BASIC_INFORMATION state{};CHECK(VirtualQuery(address,&state,sizeof(state)));return state.Protect&255;}
#ifdef LEGO_TEST_REAL_HEADERS
void InstallHeader(const std::shared_ptr<TextureResource>& resource,u32 alias,std::vector<u8>& expected){
 const auto& f=resource->guest_fetch;const u32 base=f.base_address<<12,offset=32768-sizeof(D3DTexture);
 const u32 bpt=TextureReadbackTexelBytes(ResolvedTextureReadbackFormat(*resource));
 const u32 mask=f.endianness==1?1:f.endianness==2?3:f.endianness==3?2:0;
 for(u32 y=0;y<resource->height;++y)for(u32 x=0;x<resource->width;++x)for(u32 c=0;c<bpt;++c){
  const u32 at=f.tiled?rex::graphics::texture_util::GetTiledOffset2D(x,y,512/bpt,bpt==8?3:2):y*512+x*bpt;
  CHECK(((at+c)^mask)<offset||((at+c)^mask)>=offset+sizeof(D3DTexture));
 }
 resource->guest_address=alias==0?0xA0000000+base+offset:alias==1?0xC0000000+base+offset:0xE0000000+base+offset-4096;
 D3DTexture header{};header.resource.common=resource->adopted_header_type;
 std::array<u32,6> cached{};std::memcpy(cached.data(),&resource->guest_fetch,sizeof(cached));
 for(u32 i=0;i<6;++i)header.format.dword[i]=cached[i];
 std::memcpy(memory.data.data()+base+offset,&header,sizeof(header));
 std::memcpy(expected.data()+base+offset,&header,sizeof(header));
}
#endif
int main(){
 SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
 char loaded[MAX_PATH]{};CHECK(GetModuleFileNameA(GetModuleHandleA("rexruntime.dll"),loaded,MAX_PATH));
 CHECK(!_stricmp(loaded,std::getenv("LEGO_TEST_RUNTIME_EXPECTED")));
 rex::memory::Memory sdk_memory;CHECK(sdk_memory.Initialize());fixture::sdk_memory=&sdk_memory;
#ifdef LEGO_TEST_NATIVE_PROVIDER
 auto module=GetModuleHandleA("rexruntime.dll");
 auto register_provider=reinterpret_cast<rex::memory::RegisterPhysicalDataProvider>(GetProcAddress(module,"rex_memory_register_data_provider_v1"));
 auto unregister_provider=reinterpret_cast<rex::memory::UnregisterPhysicalDataProvider>(GetProcAddress(module,"rex_memory_unregister_data_provider_v1"));
 CHECK(register_provider&&unregister_provider);
#endif
 memory.data={sdk_memory.TranslatePhysical<u8*>(0),1u<<20};
 // SDK Initialize allocates the first 16 MiB through C for GPU writeback.
 auto* alias=sdk_memory.LookupHeap(0xC0000000);CHECK(alias);
 coordinator=std::this_thread::get_id();auto api=plume::CreateD3D12Interface();device=api->createDevice();CHECK(device);
 queue=device->createCommandQueue(plume::RenderCommandListType::DIRECT);commands=queue->createCommandList();fence=device->createCommandFence();
 rex::graphics::texture_util::layout={{32768,512},0};u32 cases=0;
 for(u32 format=0;format<3;++format)for(u32 scale:{2u,3u,4u})for(bool tiled:{false,true})for(u32 endian=0;endian<4;++endian)for(bool partial:{false,true}){
  g_textures.clear();std::fill(memory.data.begin(),memory.data.end(),0x55);
  auto expected=std::vector<u8>(memory.data.begin(),memory.data.end());
  commands->begin();open=true;auto a=MakeImage(0,format,scale,tiled,endian,partial,expected);
  auto b=MakeImage(1,format,scale,!tiled,endian,!partial,expected);
#ifdef LEGO_TEST_REAL_HEADERS
  const u32 aliases[]{1,0,2};
  InstallHeader(a.resource,aliases[cases%3],expected);
  InstallHeader(b.resource,aliases[(cases+1)%3],expected);
#endif
  const auto base=a.resource->guest_fetch.base_address<<12;
  CHECK(alias->Protect(0xC0000000+base,32768,rex::memory::kMemoryProtectRead));
  CHECK(Protection(sdk_memory.TranslatePhysical(base))==PAGE_READONLY);
  CHECK(!PoolCopyRangeWritable({base,32768})&&PoolCopyRangeReadable({base,32768}));
  // The second allocation remains physically writable: one transaction must
  // retain two different protection regions and restore each correctly.
  std::array owners{a.resource,b.resource};const auto before=readbacks;
#ifdef LEGO_TEST_NATIVE_PROVIDER
  const uint32_t second_base=b.resource->guest_fetch.base_address<<12;
  NativeOwnerGroup group{sdk_memory,{a.resource,b.resource},{{base,32768},{second_base,32768}}};
  auto handle=register_provider(&sdk_memory,NativeOwnerGroup::Call,&group);CHECK(handle);
  for(auto span:group.spans)sdk_memory.EnablePhysicalMemoryDataProviders(span.address,span.length);
  uint8_t observed_a=0,observed_b=0,observed_e=0;
  std::thread reader_a([&]{observed_a=*sdk_memory.TranslateVirtual<volatile uint8_t*>(0xC0000000+base);});
  std::thread reader_b([&]{observed_b=*sdk_memory.TranslateVirtual<volatile uint8_t*>(0xA0000000+second_base);});
  std::thread reader_e([&]{observed_e=*sdk_memory.TranslateVirtual<volatile uint8_t*>(0xE0000000+base-4096+7);});
  {std::unique_lock lock(group.mutex);CHECK(group.condition.wait_for(lock,std::chrono::seconds(5),[&]{return group.callbacks>=3;}));
   group.release=true;group.condition.notify_all();}
  reader_a.join();reader_b.join();reader_e.join();CHECK(group.waits==1&&!group.pending);
  CHECK(observed_a==expected[base]&&observed_b==expected[second_base]&&observed_e==expected[base+7]);
  CHECK(unregister_provider(&sdk_memory,handle));
#else
  Recorder([&]{CHECK(MaterializeResolvedTextures(owners));});
#endif
  CHECK(readbacks==before+1&&a.resource->guest_backing_current&&b.resource->guest_backing_current);BytesMatch(expected);
  CHECK(Protection(sdk_memory.TranslatePhysical(base))==PAGE_READONLY);
  CHECK(Protection(sdk_memory.TranslateVirtual(0xC0000000+base))==PAGE_READONLY);
  uint32_t access=0;CHECK(alias->QueryProtect(0xC0000000+base,&access)&&access==rex::memory::kMemoryProtectRead);
  CHECK(Protection(sdk_memory.TranslatePhysical(b.resource->guest_fetch.base_address<<12))==PAGE_READWRITE);
  CHECK(alias->Protect(0xC0000000+base,32768,rex::memory::kMemoryProtectRead|rex::memory::kMemoryProtectWrite));
  if(open)Finish();retained.clear();++cases;
 }
 // Scope construction preflights every region before making a change. A
 // later invalid span cannot leave the earlier read-only physical page open.
 constexpr u32 at=0x20000;CHECK(alias->Protect(0xC0000000+at,4096,rex::memory::kMemoryProtectRead));
 const PhysicalCopyRange invalid[]{{at,4096},{0x30000000,4096}};
 CHECK(!OpenPhysicalWriteback(&sdk_memory,invalid));CHECK(Protection(sdk_memory.TranslatePhysical(at))==PAGE_READONLY);
 const PhysicalCopyRange good{at+1,4094};
 {auto outer=OpenPhysicalWriteback(&sdk_memory,{&good,1});CHECK(outer);
  CHECK(Protection(sdk_memory.TranslatePhysical(at))==PAGE_READWRITE);
  {auto inner=OpenPhysicalWriteback(&sdk_memory,{&good,1});CHECK(inner);}
  CHECK(Protection(sdk_memory.TranslatePhysical(at))==PAGE_READWRITE);
 }
 CHECK(Protection(sdk_memory.TranslatePhysical(at))==PAGE_READONLY);
 CHECK(alias->Protect(0xC0000000+at,4096,rex::memory::kMemoryProtectRead|rex::memory::kMemoryProtectWrite));
#ifdef LEGO_TEST_NATIVE_PROVIDER
 printf("PASS prototype native fault publication: %u mixed RW/RO GPU transactions, three real A/C/E guest fault readers, one GPU wait, exact bytes and restored permissions\n",cases);
#else
 printf("PASS read-only physical GPU publication: %u mixed RW/RO transactions, exact bytes, guest/host protection restoration, nested windows and failed preflight\n",cases);
#endif
}
