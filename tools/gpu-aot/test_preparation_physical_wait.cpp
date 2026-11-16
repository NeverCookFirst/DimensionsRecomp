#include "gpu_native/preparation_dependencies.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <thread>
#include <unordered_map>
#include <vector>
using namespace legodimensions::gpu_native;
using namespace std::chrono_literals;
#define CHECK(x) do{if(!(x)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);std::exit(23);}}while(0)
struct Input{TexturePreparationFootprint footprint;};
struct Draw{std::array<Input,32>texture_inputs;};
struct FullDrawWork{bool captured=true;Draw draw;};
struct Batch{PreparationDependencyFilter bloom;std::atomic<unsigned>pending_jobs{1};std::vector<PreparationDependencyJob>jobs;};
thread_local bool t_on_coordinator=true;
PreparationDependencyFilter g_full_draw_batch_bloom;
std::deque<std::shared_ptr<Batch>>g_inflight_batches;
std::shared_ptr<Batch>open;
std::unordered_map<unsigned,TexturePreparationFootprint> footprints;
std::atomic<unsigned>flushed=0,queries=0,claimed=0;
bool per_chunk=true;
bool ChunkDependencyWaitsEnabled(){return per_chunk;}
TexturePreparationFootprint CaptureTexturePreparationFootprint(unsigned address){++queries;return footprints.at(address);}
void FlushFullDrawBatch(){++flushed;if(open){g_inflight_batches.push_back(open);open.reset();}g_full_draw_batch_bloom={};}
void RunPreparationChunkHere(const PreparationDependencyJob&job){if(job.task&&job.task->TryRun())++claimed;}
ADMIT_BODY
WAIT_BODY
TexturePreparationFootprint Footprint(unsigned at,unsigned bytes=64){TexturePreparationFootprint result;result.ranges[1]={at,bytes};return result;}
PreparationDependencyFilter Capture(unsigned header,TexturePreparationFootprint footprint,bool captured=true){
 PreparationDependencyFilter result;result.Add(header);FullDrawWork work;work.captured=captured;work.draw.texture_inputs[0].footprint=footprint;
 AddFullDrawPhysicalDependencies(result.physical,work);return result;
}
void Reset(){g_inflight_batches.clear();open.reset();g_full_draw_batch_bloom={};footprints.clear();flushed=queries=claimed=0;per_chunk=true;}
void CheckAlias(bool broad=false,bool unknown=false,bool fallback=false){
 Reset();per_chunk=!broad;constexpr unsigned source=0xA0010000,target=0xA0020000,unrelated=0xA0030000;
 const auto pixels=Footprint(0x30080),other=Footprint(0x90080);
 footprints[target]=pixels;if(unknown)footprints[target].unknown=true;
 auto batch=std::make_shared<Batch>();batch->bloom=Capture(source,pixels,!fallback);
 std::promise<void>done,other_done,started;
 batch->jobs.push_back({done.get_future().share(),Capture(source,pixels,!fallback),{}});
 batch->jobs.push_back({other_done.get_future().share(),Capture(unrelated,other),{}});
 open=batch;g_full_draw_batch_bloom=batch->bloom;
 auto wait=std::async(std::launch::async,[&]{t_on_coordinator=true;started.set_value();WaitPendingPreparationFor({target});return true;});
 started.get_future().wait();CHECK(wait.wait_for(60ms)==std::future_status::timeout);
 done.set_value();
 if(broad||unknown)CHECK(wait.wait_for(60ms)==std::future_status::timeout);
 else CHECK(wait.wait_for(3s)==std::future_status::ready);
 other_done.set_value();CHECK(wait.wait_for(3s)==std::future_status::ready&&wait.get());CHECK(flushed==1);
}
void CheckNoOverlapAndSteal(){
 Reset();constexpr unsigned target=0xA0010000;footprints[target]=Footprint(0x40000);
 g_full_draw_batch_bloom.physical.unknown=true;WaitPendingPreparationFor({0});WaitPendingPreparationFor({});CHECK(!flushed&&!queries);g_full_draw_batch_bloom={};
 auto batch=std::make_shared<Batch>();batch->bloom=Capture(0xA0020000,Footprint(0x50000));
 auto task=std::make_shared<PreparationChunkTask>();task->run=[]{};
 batch->jobs.push_back({task->done.get_future().share(),batch->bloom,task});g_inflight_batches.push_back(batch);
 WaitPendingPreparationFor({target});CHECK(!claimed&&!flushed);
 batch->bloom=Capture(0xA0020000,footprints[target]);batch->jobs[0].dependencies=batch->bloom;
 WaitPendingPreparationFor({target});CHECK(claimed==1);
 // Same-header ordering survives when there is no guest pixel span.
 footprints[target]={};batch->bloom=Capture(target,{});batch->jobs[0].dependencies=batch->bloom;WaitPendingPreparationFor({target});
 const auto before=queries.load();t_on_coordinator=false;WaitPendingPreparationFor({0xBAD});CHECK(queries==before);t_on_coordinator=true;
}
void CheckRangeUnion(){
 unsigned random=0xE0C0FFEE;auto next=[&]{random^=random<<13;random^=random>>17;random^=random<<5;return random;};
 for(unsigned trial=0;trial<256;++trial){PreparationPhysicalFilter filter;std::vector<PhysicalCopyRange>oracle;
  for(unsigned i=0;i<128;++i){PhysicalCopyRange range{next()%65536,1+next()%512};filter.Add(range);oracle.push_back(range);
   for(unsigned q=0;q<16;++q){PhysicalCopyRange query{next()%65536,1+next()%512};PreparationPhysicalFilter test;test.Add(query);
    const bool expected=std::any_of(oracle.begin(),oracle.end(),[&](auto r){return CopyRangesOverlap(r,query);});
    CHECK(filter.Matches(test)==(filter.unknown||expected));}}
 }
 PreparationPhysicalFilter filter;filter.Add(PhysicalCopyRange{0x1FFFFFFF,2});CHECK(filter.unknown);
 PreparationPhysicalFilter chain;chain.Add(PhysicalCopyRange{100,10});chain.Add(PhysicalCopyRange{120,10});chain.Add(PhysicalCopyRange{110,10});CHECK(chain.count==1&&chain.ranges[0].length==30);
 PreparationPhysicalFilter outside;outside.Add(PhysicalCopyRange{130,1});CHECK(!chain.Matches(outside));
}
int main(){CheckAlias();CheckAlias(true);CheckAlias(false,true);CheckAlias(false,false,true);CheckNoOverlapAndSteal();CheckRangeUnion();
 std::puts("PASS actual coordinator waits: different headers/shared pixels, open-batch flush, chunk isolation, conservative fallback, stealing and 524288 interval-oracle queries; no game/GPU.");}
