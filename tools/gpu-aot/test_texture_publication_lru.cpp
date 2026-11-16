// Production publication cache and QueuePreparedTextureUpload; no GPU or game.
#include "gpu_native/texture_upload_publication.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <future>
#include <thread>
using namespace legodimensions::gpu_native;
using u64=uint64_t;
#define CHECK(c) do{if(!(c)){std::fprintf(stderr,"line %u: %s\n",__LINE__,#c);std::exit(23);}}while(0)
struct Copy:NativeDrawCommands{void Replay(plume::RenderCommandList*)override{}};
struct Lease{
 TextureUploadPublicationCache* cache=nullptr;
 ~Lease(){if(!cache)return;
  auto future=std::async(std::launch::async,[this]{return cache->Bytes();});
  if(future.wait_for(std::chrono::milliseconds(500))!=std::future_status::ready){
   std::fprintf(stderr,"staging owner destroyed under cache mutex\n");std::fflush(stderr);std::_Exit(23);
  }
  future.get();
 }
};
auto Make(u64 bytes,TextureUploadPublicationCache* cache=nullptr){
 auto lease=std::make_shared<Lease>();lease->cache=cache;
 return std::make_shared<TextureUploadPublication>(std::make_shared<TextureUploadReplayState>(),std::make_unique<Copy>(),lease,bytes);
}
TextureUploadPublicationCache g_texture_upload_publications(2);
struct TextureResource{std::weak_ptr<TextureUploadPublication> prepared_upload;};
struct HostDevice{static inline std::vector<std::shared_ptr<void>> retired;static void RetireResource(std::shared_ptr<void> p){retired.push_back(std::move(p));}};
#include "publication-queue.inc"
int main(){
 {TextureUploadPublicationCache cache(2);auto a=Make(1),b=Make(1),c=Make(1);
  cache.Keep(a);cache.Keep(b);CHECK(cache.Touch(a));cache.Keep(c);
  CHECK(cache.Bytes()==2&&!cache.Touch(b)&&cache.Touch(a)&&cache.Touch(c));
  for(unsigned i=0;i<64;++i)cache.Keep(c);CHECK(cache.Bytes()==2);
  cache.Keep({});cache.Keep(Make(3));CHECK(cache.Bytes()==2&&!cache.Touch({}));cache.Reset();CHECK(!cache.Bytes()&&!cache.Touch(a));
 }
 {auto a=Make(1),b=Make(1),c=Make(1);g_texture_upload_publications.Keep(a);g_texture_upload_publications.Keep(b);
  TextureResource r;r.prepared_upload=a;plume::RenderCommandList commands;
  CHECK(QueuePreparedTextureUpload(r,&commands));g_texture_upload_publications.Keep(c);
  CHECK(!g_texture_upload_publications.Touch(b)&&g_texture_upload_publications.Touch(a));
  g_texture_upload_publications.Reset();HostDevice::retired.clear();
 }
 // Last GPU/staging owner destruction may re-enter cache/renderer code.
 // Both eviction and Reset must release it after dropping the cache mutex.
 {TextureUploadPublicationCache cache(1);cache.Keep(Make(1,&cache));cache.Keep(Make(1,&cache));cache.Reset();CHECK(!cache.Bytes());}
 {TextureUploadPublicationCache cache(32);std::array<std::shared_ptr<TextureUploadPublication>,64> values;
  for(unsigned i=0;i<64;++i)values[i]=Make(1+i%8);
  std::vector<unsigned> order;u64 bytes=0;
  uint32_t random=0xCAFE2026;
  auto next=[&]{random^=random<<13;random^=random>>17;random^=random<<5;return random;};
  for(unsigned trial=0;trial<10000;++trial){const unsigned id=next()%64,op=next()%16;auto it=std::find(order.begin(),order.end(),id);
   if(op==0){cache.Reset();order.clear();bytes=0;}
   else if(op<9){cache.Keep(values[id]);
    if(it!=order.end())order.erase(it);
    else{while(bytes+values[id]->Bytes()>32){bytes-=values[order.front()]->Bytes();order.erase(order.begin());}bytes+=values[id]->Bytes();}
    order.push_back(id);
   }else{const bool expected=it!=order.end();CHECK(cache.Touch(values[id])==expected);
    if(expected){order.erase(it);order.push_back(id);}}
   CHECK(cache.Bytes()==bytes&&bytes<=32);
  }
 }
 unsigned misses=0,fifo_misses=0;
 {TextureUploadPublicationCache cache(8);std::vector<std::shared_ptr<TextureUploadPublication>> values;
  for(unsigned i=0;i<1002;++i)values.push_back(Make(1));cache.Keep(values[0]);cache.Keep(values[1]);
  std::vector<unsigned> fifo{0,1};
  for(unsigned frame=0;frame<100;++frame){for(unsigned hot=0;hot<2;++hot){
    if(!cache.Touch(values[hot])){++misses;cache.Keep(values[hot]);}
    if(std::find(fifo.begin(),fifo.end(),hot)==fifo.end()){++fifo_misses;if(fifo.size()==8)fifo.erase(fifo.begin());fifo.push_back(hot);}}
   for(unsigned cold=0;cold<6;++cold){const unsigned id=2+frame*6+cold;cache.Keep(values[id]);if(fifo.size()==8)fifo.erase(fifo.begin());fifo.push_back(id);}
   CHECK(cache.Bytes()==8);
  }
  CHECK(misses==0&&fifo_misses==98);
 }
 {TextureUploadPublicationCache cache(32);std::array<std::shared_ptr<TextureUploadPublication>,64> values;
  for(unsigned i=0;i<64;++i)values[i]=Make(1);
  std::vector<std::thread> threads;
  for(unsigned worker=0;worker<4;++worker)threads.emplace_back([&,worker]{for(unsigned i=0;i<10000;++i){auto& p=values[(i+worker*13)%64];cache.Keep(p);cache.Touch(p);if(i%101==0)cache.Reset();}});
  for(auto& t:threads)t.join();CHECK(cache.Bytes()<=32);cache.Reset();
 }
 std::printf("PASS publication LRU: 10000 differential operations, 40000 concurrent operations, production queue promotion, duplicate/budget/reset and destruction outside mutex; hot/cold trace misses LRU=%u FIFO=%u (structural cache count, not FPS)\n",misses,fifo_misses);
}
