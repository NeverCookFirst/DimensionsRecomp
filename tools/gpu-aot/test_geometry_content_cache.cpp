#include "gpu_native/geometry_content_cache.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <thread>
using namespace legodimensions::gpu_native;
#define CHECK(x) do{if(!(x)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);std::exit(23);}}while(0)
struct Buffer {uint32_t value=0;};
auto ready() {
  auto publication=std::make_shared<GeometryPublication>();
  publication->recorded=true;publication->ready=true;return publication;
}
int main(){
  GeometryContentCache<Buffer> cache;
  GeometryContentKey key{17,29,16,6,0,16,0};
  std::vector<uint8_t> bytes(16,0x57);
  const std::array<uint32_t,2> fields{0,8},other_fields{0,12};
  auto buffer=std::make_shared<Buffer>();
  auto pending=std::make_shared<GeometryPublication>();
  cache.Remember(key,bytes,fields,buffer,pending);
  CHECK(!cache.Find(key,bytes,fields));
  {auto abandoned=std::make_shared<GeometryFenceCompletion>(pending);}
  CHECK(!pending->ready && !cache.Find(key,bytes,fields));
  auto completion=std::make_shared<GeometryFenceCompletion>(pending),another_live_fence=completion;
  pending->recorded=true;completion.reset();
  CHECK(!cache.Find(key,bytes,fields));
  buffer->value=123;another_live_fence.reset();
  CHECK(cache.Find(key,bytes,fields)==buffer && buffer->value==123);
  auto changed=bytes;changed[7]^=1;
  CHECK(!cache.Find(key,changed,fields)); // A colliding content hash is insufficient.
  CHECK(!cache.Find(key,bytes,other_fields)); // Nor is a colliding conversion hash.
  for(uint32_t field=0;field<5;++field){
    auto other=key;
    switch(field){case 0:++other.kind;break;case 1:++other.format;break;
      case 2:++other.stride;break;case 3:++other.phase;break;case 4:++other.length;break;}
    CHECK(!cache.Find(other,bytes,fields));
  }
  std::weak_ptr<Buffer> weak=buffer;buffer.reset();
  CHECK(weak.expired());CHECK(!cache.Find(key,bytes,fields));
  CHECK(cache.Stats().bytes==0 && cache.Stats().entries==0);
  std::vector<std::shared_ptr<Buffer>> owners;
  for(uint32_t i=0;i<GeometryContentCache<Buffer>::kMaxEntries+1;++i){
    key.content_hash=i;auto owner=std::make_shared<Buffer>();owners.push_back(owner);
    cache.Remember(key,bytes,fields,owner,ready());
  }
  CHECK(cache.Stats().entries==GeometryContentCache<Buffer>::kMaxEntries);
  key.content_hash=0;CHECK(!cache.Find(key,bytes,fields));
  key.content_hash=GeometryContentCache<Buffer>::kMaxEntries;CHECK(cache.Find(key,bytes,fields)==owners.back());
  cache.Reset();owners.clear();
  std::vector<uint8_t> large(GeometryContentCache<Buffer>::kMaxEntryBytes,0x57);
  key.length=uint32_t(large.size());
  for(uint32_t i=0;i<10;++i){
    key.content_hash=i;auto owner=std::make_shared<Buffer>();owners.push_back(owner);
    cache.Remember(key,large,{},owner,ready());
    CHECK(cache.Stats().bytes<=GeometryContentCache<Buffer>::kMaxBytes);
  }
  CHECK(cache.Stats().entries==8);
  cache.Reset();owners.clear();key.length=16;
  buffer=std::make_shared<Buffer>();pending=std::make_shared<GeometryPublication>();
  cache.Remember(key,bytes,fields,buffer,pending);
  std::thread producer([&]{
    buffer->value=997;pending->recorded=true;
    auto complete=std::make_shared<GeometryFenceCompletion>(pending);
  });
  producer.join();CHECK(cache.Find(key,bytes,fields)->value==997);
  cache.Reset();CHECK(!cache.Find(key,bytes,fields));
  CHECK(!GeometryContentCache<Buffer>::Eligible(0,0));
  CHECK(!GeometryContentCache<Buffer>::Eligible(GeometryContentCache<Buffer>::kMaxEntryBytes+1,0));
  CHECK(!GeometryContentCache<Buffer>::Eligible(16,257));
  std::puts("PASS geometry cache: readiness/fence/abandoned tape, exact collision checks, weak GPU ownership, bounded CPU bytes/entries, reset and cross-thread publication");
}
