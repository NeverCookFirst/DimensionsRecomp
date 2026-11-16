#include "settings_revision.h"
#include "settings_service.h"
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <latch>
#include <thread>
using namespace legodimensions::settings;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"line %d failed: %s\n",__LINE__,#x); std::exit(23); } } while (0)

struct Registry {
 Values initial{{"fullscreen","false"},{"scale","150"},{"fps","240"}}, live=initial,saved;
 unsigned writes=0,saves=0,fail_at=0,throw_at=0;
 bool read_failed=false;
 std::function<void()> on_write;
 Backend backend(){return {
  [this]{if(read_failed)throw std::runtime_error("Read failed");return live;},
  [](auto,auto value){return NormalizedValue{true,std::string(value),{}};},
  [this](auto key,auto value){++writes;live[std::string(key)]=value;if(on_write)on_write();
   if(writes==throw_at)throw std::runtime_error("Setter failed after changing value");return writes!=fail_at;},
  [this](const Values& changes,std::string&){++saves;saved=changes;return true;}
 };}
};
struct Fixture : Registry {
 bool window_ready=false,window_failed=false,window_throws=false,wrong_window_revision=false;
 unsigned window_checks=0,commits=0;
 SettingsRevisionCoordinator coordinator;
 Backend wrapped;
 Fixture():coordinator(backend(),[this](const SettingsRevision&r,const PresentationObservation&p){
  ++window_checks;CHECK(p.width==1280&&p.height==720);
  if(window_throws)throw std::runtime_error("Window query failed");
  return RevisionResult{wrong_window_revision?r.id+100:r.id,window_failed?RevisionState::kFailed:window_ready?RevisionState::kComplete:RevisionState::kPending,"Window failed"};
 }),wrapped(coordinator.MakeBackend()){}
 RevisionTicket submit(Values changes={{"fullscreen","true"},{"scale","300"}}){
  auto result=wrapped.submit(changes);CHECK(result.state==RevisionState::kPending&&result.revision);
  CHECK(coordinator.AdvanceFromUI());auto request=coordinator.PendingForRenderer();CHECK(request&&request->id==result.revision);return request;
 }
 bool commit(const RevisionTicket& request,RevisionState result=RevisionState::kComplete){
  return coordinator.TryCommit(request,[&](const SettingsRevision&r){++commits;CHECK(r.id==request->id);return RevisionResult{r.id,result,"Render preparation failed"};});
 }
 void finish(const RevisionTicket& request){CHECK(commit(request));CHECK(coordinator.AcknowledgePresentation(request,{1280,720,false}));window_ready=true;
  CHECK(wrapped.observe(request->id).state==RevisionState::kComplete);}
};

int main(){unsigned scenarios=0;
 {
  Fixture f;auto base=f.coordinator.ActiveForRenderer();CHECK(base&&base->id==0&&base->values==f.initial);++scenarios;
  CHECK(!f.wrapped.write("fps","0")&&f.writes==0);++scenarios;
  Values changes{{"fullscreen","true"},{"scale","300"}};auto result=f.wrapped.submit(changes);changes["scale"]="20";
  CHECK(f.writes==0&&!f.coordinator.PendingForRenderer()&&f.wrapped.observe(result.revision).state==RevisionState::kPending);++scenarios;
  f.on_write=[&]{CHECK(!f.coordinator.PendingForRenderer()&&f.coordinator.ActiveForRenderer()==base);};
  CHECK(f.coordinator.AdvanceFromUI());auto r=f.coordinator.PendingForRenderer();
  CHECK(r&&r->values.at("scale")=="300"&&r->values.at("fps")=="240"&&r->changes.size()==2&&f.writes==2);++scenarios;
  CHECK(!f.coordinator.AcknowledgePresentation(r,{1280,720,false})&&f.window_checks==0);
  CHECK(f.wrapped.observe(r->id).state==RevisionState::kPending);++scenarios;
  CHECK(f.commit(r)&&f.coordinator.ActiveForRenderer()==r&&!f.coordinator.PendingForRenderer());
  CHECK(f.wrapped.observe(r->id).state==RevisionState::kPending&&f.window_checks==0);++scenarios;
  CHECK(!f.coordinator.AcknowledgePresentation(r,{0,720,false}));CHECK(f.coordinator.AcknowledgePresentation(r,{1280,720,false}));
  CHECK(f.wrapped.observe(r->id).state==RevisionState::kPending&&f.window_checks==1);++scenarios;
  std::string error;CHECK(!f.wrapped.persist(r->changes,error)&&f.saves==0);
  f.wrong_window_revision=true;f.window_ready=true;CHECK(f.wrapped.observe(r->id).state==RevisionState::kPending);++scenarios;
  f.wrong_window_revision=false;CHECK(f.wrapped.observe(r->id).state==RevisionState::kComplete);
  CHECK(f.wrapped.persist(r->changes,error)&&f.saves==1);CHECK(!f.commit(r)&&f.commits==1);++scenarios;
 }
 {
  Fixture f;auto first=f.wrapped.submit({{"fullscreen","true"}});auto second=f.wrapped.submit({{"fps","0"}});
  CHECK(second.revision>first.revision&&f.writes==0);CHECK(f.coordinator.AdvanceFromUI());auto r=f.coordinator.PendingForRenderer();
  CHECK(r->values.at("fullscreen")=="false"&&r->values.at("fps")=="0"&&f.writes==1);
  CHECK(f.wrapped.observe(first.revision).state==RevisionState::kFailed);++scenarios;
 }
 {
  Fixture f;auto old=f.submit();auto newer=f.submit({{"fullscreen","false"},{"fps","0"}});
  CHECK(!f.commit(old)&&f.commits==0&&!f.coordinator.AcknowledgePresentation(old,{1280,720,false}));
  auto forged=std::make_shared<SettingsRevision>(*newer);CHECK(!f.commit(forged)&&f.commits==0);++scenarios;
  f.finish(newer);CHECK(!f.coordinator.AcknowledgePresentation(old,{1280,720,false}));++scenarios;
 }
 {
  Fixture f;auto request=f.submit();auto old=f.coordinator.ActiveForRenderer();CHECK(!f.commit(request,RevisionState::kPending));
  CHECK(f.coordinator.ActiveForRenderer()==old&&f.wrapped.observe(request->id).state==RevisionState::kPending);
  f.finish(request);CHECK(f.commits==2);++scenarios;
 }
 {
  Fixture f;auto request=f.submit();CHECK(!f.commit(request,RevisionState::kFailed));
  CHECK(f.wrapped.observe(request->id).state==RevisionState::kFailed&&!f.coordinator.PendingForRenderer());
  auto restore=f.submit(f.initial);f.finish(restore);CHECK(f.live==f.initial);++scenarios;
 }
 for(bool throws:{false,true}){
  Fixture f;auto request=f.submit();bool result=f.coordinator.TryCommit(request,[&](const auto&r)->RevisionResult{
   if(throws)throw std::runtime_error("Renderer preparation threw");return {r.id+1,RevisionState::kComplete,{}};});
  CHECK(!result&&f.wrapped.observe(request->id).state==RevisionState::kFailed&&f.coordinator.ActiveForRenderer()->id==0);++scenarios;
 }
 for(bool throws:{false,true}){
  Fixture f;auto request=f.submit();CHECK(f.commit(request)&&f.coordinator.AcknowledgePresentation(request,{1280,720,false}));
  f.window_failed=!throws;f.window_throws=throws;CHECK(f.wrapped.observe(request->id).state==RevisionState::kFailed);++scenarios;
 }
 for(bool throws:{false,true}){
  Fixture f;if(throws)f.throw_at=1;else f.fail_at=1;auto result=f.wrapped.submit({{"fullscreen","true"},{"scale","300"}});
  CHECK(!f.coordinator.AdvanceFromUI());CHECK(f.wrapped.observe(result.revision).state==RevisionState::kFailed);
  CHECK(!f.coordinator.PendingForRenderer()&&f.coordinator.ActiveForRenderer()->values==f.initial);
  f.fail_at=f.throw_at=0;auto restore=f.submit(f.initial);f.finish(restore);CHECK(f.live==f.initial);++scenarios;
 }
 {
  Fixture f;auto result=f.wrapped.submit({{"fullscreen","true"}});f.live["fps"]="90";
  CHECK(!f.coordinator.AdvanceFromUI()&&f.wrapped.observe(result.revision).state==RevisionState::kFailed&&!f.coordinator.PendingForRenderer());++scenarios;
 }
 {
  Fixture f;auto request=f.submit();f.live["fps"]="90";
  CHECK(f.wrapped.observe(request->id).state==RevisionState::kFailed&&!f.coordinator.PendingForRenderer());++scenarios;
 }
 {
  Fixture f;auto request=f.submit();f.finish(request);f.live["fps"]="90";std::string error;
  CHECK(!f.wrapped.persist(request->changes,error)&&f.saves==0&&f.wrapped.observe(request->id).state==RevisionState::kFailed);++scenarios;
 }
 {
  Fixture f;auto request=f.submit();f.finish(request);f.window_ready=false;std::string error;
  CHECK(!f.wrapped.persist(request->changes,error)&&f.saves==0&&f.wrapped.observe(request->id).state==RevisionState::kPending);
  f.window_ready=true;CHECK(f.wrapped.persist(request->changes,error)&&f.saves==1);++scenarios;
 }
 {
  Fixture f;auto result=f.wrapped.submit({{"unknown","true"}});CHECK(result.state==RevisionState::kFailed&&f.writes==0);
  f.read_failed=true;bool threw=false;try{f.wrapped.submit({{"fullscreen","true"}});}catch(const std::exception&){threw=true;}
  CHECK(threw&&f.writes==0);++scenarios;
 }
 {
  Fixture f;bool rejected=false;f.on_write=[&]{try{f.wrapped.submit({{"fps","0"}});}catch(const std::exception&){rejected=true;}};
  auto request=f.submit();CHECK(rejected&&f.writes==2&&request->id==1);++scenarios;
 }
 {
  Fixture f;auto request=f.submit();std::latch entered(1),release(1);bool committed=false;
  std::thread render([&]{committed=f.coordinator.TryCommit(request,[&](const auto&r){entered.count_down();release.wait();return RevisionResult{r.id,RevisionState::kComplete,{}};});});
  entered.wait();auto next=f.wrapped.submit({{"fps","0"}});auto before=f.writes;
  CHECK(!f.coordinator.AdvanceFromUI()&&f.writes==before); // Must return while renderer is suspended.
  release.count_down();render.join();CHECK(committed&&f.coordinator.ActiveForRenderer()==request);
  CHECK(!f.coordinator.AcknowledgePresentation(request,{1280,720,false}));
  CHECK(f.coordinator.AdvanceFromUI());auto pending=f.coordinator.PendingForRenderer();CHECK(pending->id==next.revision&&pending->values.at("fps")=="0");f.finish(pending);++scenarios;
 }
 {
  Fixture f;auto request=f.submit();std::atomic<unsigned> rejected=0;std::thread wrong([&]{
   try{f.wrapped.submit({{"fps","0"}});}catch(const std::exception&){++rejected;}
   try{f.coordinator.AdvanceFromUI();}catch(const std::exception&){++rejected;}
   try{f.wrapped.observe(request->id);}catch(const std::exception&){++rejected;}
   try{f.wrapped.read();}catch(const std::exception&){++rejected;}
   try{f.coordinator.StopFromUI();}catch(const std::exception&){++rejected;}
  });wrong.join();CHECK(rejected==5);++scenarios;
 }
 {
  Fixture f;auto request=f.submit();f.coordinator.StopFromUI();CHECK(!f.coordinator.PendingForRenderer()&&!f.commit(request));
  CHECK(!f.coordinator.AcknowledgePresentation(request,{1280,720,false}));bool denied=false;try{f.wrapped.read();}catch(const std::exception&){denied=true;}CHECK(denied);++scenarios;
 }
 {
  Registry registry;auto token=std::make_shared<int>(1);std::weak_ptr<int> observer=token;
  SettingsRevisionCoordinator coordinator(registry.backend(),[token](const auto&r,const auto&){return RevisionResult{r.id,RevisionState::kComplete,{}};});
  token.reset();auto wrapped=coordinator.MakeBackend();CHECK(!observer.expired());coordinator.StopFromUI();CHECK(observer.expired());++scenarios;
 }
 {
  Fixture f;f.on_write=[&]{f.coordinator.StopFromUI();};auto r=f.wrapped.submit({{"fullscreen","true"},{"scale","300"}});
  CHECK(!f.coordinator.AdvanceFromUI()&&f.writes==1&&!f.coordinator.PendingForRenderer());++scenarios;
 }
 {
  Fixture f;auto request=f.submit();CHECK(f.commit(request)&&f.coordinator.AcknowledgePresentation(request,{1280,720,false}));
  // Window checking is a UI callback and may stop the host reentrantly.
  f.window_ready=true;f.coordinator.StopFromUI();CHECK(!f.coordinator.PendingForRenderer()&&f.coordinator.ActiveForRenderer()==request);++scenarios;
 }
 {
  Fixture f;std::deque<std::function<void()>> jobs;auto now=Clock::now();
  auto service=std::make_shared<QueuedSettings>(f.wrapped,[&](auto job){jobs.push_back(std::move(job));return true;},f.initial,
    std::set<std::string,std::less<>>{"fullscreen","scale"},[&]{return now;});
  auto invoke=[&](Command c){auto future=service->Submit(c);CHECK(!jobs.empty());auto task=std::move(jobs.front());jobs.pop_front();task();return future.get();};
  CHECK(invoke({CommandKind::kOpen}).ok&&invoke({CommandKind::kEdit,"fullscreen","true"}).ok);
  CHECK(invoke({CommandKind::kApply}).phase==Phase::kApplying&&f.writes==0&&f.saves==0);
  CHECK(f.coordinator.AdvanceFromUI());auto request=f.coordinator.PendingForRenderer();f.finish(request);
  CHECK(invoke({CommandKind::kSnapshot}).phase==Phase::kPreview);CHECK(invoke({CommandKind::kKeep}).ok&&f.saves==1);++scenarios;
  CHECK(invoke({CommandKind::kEdit,"scale","300"}).ok&&invoke({CommandKind::kApply}).phase==Phase::kApplying);
  CHECK(service->FocusLostFromUI().phase==Phase::kRestoring);CHECK(f.coordinator.AdvanceFromUI());request=f.coordinator.PendingForRenderer();
  CHECK(request->values.at("scale")=="150");f.finish(request);CHECK(invoke({CommandKind::kSnapshot}).phase==Phase::kEditing&&f.saves==1);
  CHECK(service->ShutdownFromUI().ok);f.coordinator.StopFromUI();++scenarios;
 }
 std::printf("PASS %u revision coordinator scenarios: copied full proposals, deferred UI writers, renderer/presentation/window gates, supersession, leases without UI waits, failures/recovery, stop/callback lifetime and queued transaction integration\n",scenarios);
}
