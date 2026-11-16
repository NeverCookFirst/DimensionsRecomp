#include "settings_frontend.h"
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <map>
#include <stdexcept>
#include <thread>
using namespace legodimensions::settings;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"line %d failed: %s\n",__LINE__,#x); std::exit(23); } } while (0)

// Receipt/resource boundary only. Registry reads, transactions, queued service
// and frontend are production code. No platform window or GPU is created.
struct Coordinator {
 Values initial=NativeSettingsDefaults(),live=initial,actual=initial,saved;
 std::map<uint64_t,Values> requests;
 uint64_t next=0,latest=0,stale=0;
 unsigned saves=0,writes=0,observes=0,submits=0;
 bool immediate=false,fail_once=false,throw_submit_once=false,throw_observe=false;
 bool save_failed=false,read_failed=false,readback_mismatch=false;
 bool zero_once=false,duplicate_once=false;
 Clock::time_point now=Clock::time_point(std::chrono::seconds(100));
 Backend backend(){return {
  [this]{if(read_failed)throw std::runtime_error("Read failed");return live;},
  [](auto key,auto value){return NormalizeNativeSetting(key,value);},
  [this](auto,auto){++writes;CHECK(false);return false;},
  [this](const Values& changes,std::string& error){if(save_failed){error="Disk unavailable";return false;}++saves;saved=changes;return true;},
  [this](const Values& changes){
   ++submits;const auto revision=++next;latest=revision;requests[revision]=changes;
   for(const auto& [key,value]:changes)live[key]=value;
   if(throw_submit_once){throw_submit_once=false;throw std::runtime_error("Partial submit failed");}
   if(fail_once){fail_once=false;return RevisionResult{revision,RevisionState::kFailed,"Resource allocation failed"};}
   if(zero_once){zero_once=false;return RevisionResult{0,RevisionState::kPending,{}};}
   if(duplicate_once){duplicate_once=false;return RevisionResult{revision-1,RevisionState::kComplete,{}};}
   if(immediate){complete(revision);return receipts.at(revision);}
   return RevisionResult{revision,RevisionState::kPending,{}};
  },
  [this](uint64_t revision){++observes;if(throw_observe)throw std::runtime_error("Observation failed");
   if(stale)return receipts.at(stale);
   if(auto it=receipts.find(revision);it!=receipts.end())return it->second;
   return RevisionResult{revision,RevisionState::kPending,{}};}
 };}
 std::map<uint64_t,RevisionResult> receipts;
 void complete(uint64_t revision){
  // A real coordinator must also supersede old resource work; transaction
  // tests separately force old receipts through observe to check its guard.
  if(revision==latest)for(const auto& [key,value]:requests.at(revision))actual[key]=value;
  receipts[revision]={revision,RevisionState::kComplete,{}};
  if(readback_mismatch)live["fullscreen"]="wrong";
 }
 void failed(uint64_t revision){receipts[revision]={revision,RevisionState::kFailed,"Consumer rejected revision"};}
 auto transaction(){return Transaction(backend(),NativeSettingsConfirmationKeys());}
 void tick(Transaction& t,int seconds=0){now+=std::chrono::seconds(seconds);t.Tick(now);}
};
void display(Transaction& t){CHECK(t.Open()&&t.Edit("fullscreen","true")&&t.Edit("native_resolution_scale","300"));}

struct UI : Coordinator {
 std::deque<std::function<void()>> jobs;
 std::shared_ptr<QueuedSettings> service;
 std::unique_ptr<FrontendController> menu;
 UI(){
  service=std::make_shared<QueuedSettings>(backend(),[this](auto f){jobs.push_back(std::move(f));return true;},
    NativeSettingsDefaults(),NativeSettingsConfirmationKeys(),[this]{return now;});
  menu=std::make_unique<FrontendController>(service,[this]{return now;});
 }
 ~UI(){service->ShutdownFromUI();}
 void flush(){for(unsigned i=0;i<32;++i){if(!jobs.empty()){auto f=std::move(jobs.front());jobs.pop_front();f();}menu->Poll();
   if(jobs.empty()&&!menu->busy())return;}CHECK(false);}
 void advance(int ms=100){now+=std::chrono::milliseconds(ms);CHECK(service->AdvanceFromUI());menu->Poll();flush();}
 size_t row(std::string_view key){for(size_t i=0;i<menu->layout().rows.size();++i)if(menu->layout().rows[i].key==key)return i;CHECK(false);return 0;}
 size_t action(RowAction action){for(size_t i=0;i<menu->layout().rows.size();++i)if(menu->layout().rows[i].action==action)return i;CHECK(false);return 0;}
 void open(){CHECK(menu->Enter(FrontendPage::kVideo));flush();}
 void apply(std::string_view key="fullscreen"){CHECK(menu->Adjust(row(key),1));flush();CHECK(menu->Activate(action(RowAction::kApply)));flush();}
};

int main(){unsigned count=0;
 {
  Coordinator c;auto t=c.transaction();display(t);CHECK(t.Apply(c.now));
  CHECK(t.phase()==Phase::kApplying&&t.notice()==Notice::kApplying&&t.deadline()==Clock::time_point{});
  CHECK(c.submits==1&&c.requests.at(1).size()==2&&c.writes==0&&c.actual==c.initial&&c.saves==0);
  CHECK(!t.Keep(c.now)&&!t.Edit("fullscreen","false")&&!t.Apply(c.now)&&!t.Open());++count;
  c.tick(t,4);CHECK(t.phase()==Phase::kApplying&&c.saves==0);
  c.complete(1);c.tick(t);CHECK(t.phase()==Phase::kPreview&&t.deadline()==c.now+std::chrono::seconds(15));
  CHECK(t.Keep(c.now)&&c.saves==1&&c.saved.size()==2&&t.phase()==Phase::kEditing);++count;
 }
 for(bool close:{false,true}){
  Coordinator c;auto t=c.transaction();display(t);CHECK(t.Apply(c.now));
  CHECK(close?t.Cancel():t.Revert());CHECK(t.phase()==Phase::kRestoring&&c.submits==2&&c.saves==0);
  CHECK(c.requests.at(2).at("fullscreen")=="false"&&c.requests.at(2).at("native_resolution_scale")=="0");
  c.complete(1);c.stale=1;c.tick(t);CHECK(t.phase()==Phase::kRestoring&&c.actual==c.initial);++count;
  c.stale=0;c.complete(2);c.tick(t);CHECK(t.phase()==(close?Phase::kClosed:Phase::kEditing)&&!t.dirty()&&c.saves==0);++count;
 }
 {
  Coordinator c;auto t=c.transaction();display(t);CHECK(t.Apply(c.now));
  c.tick(t,30);CHECK(t.phase()==Phase::kRestoring&&c.submits==2&&c.saves==0);
  c.complete(2);c.tick(t);CHECK(t.phase()==Phase::kEditing&&t.notice()==Notice::kApplyFailed&&t.dirty());++count;
 }
 {
  Coordinator c;auto t=c.transaction();display(t);CHECK(t.Apply(c.now));c.complete(1);
  c.tick(t,30);CHECK(t.phase()==Phase::kRestoring&&c.saves==0);++count;
 }
 {
  Coordinator c;auto t=c.transaction();display(t);CHECK(t.Apply(c.now));c.complete(1);c.tick(t);
  c.now+=std::chrono::seconds(100);CHECK(!t.Keep(c.now)&&t.phase()==Phase::kRestoring);
  c.tick(t);CHECK(t.phase()==Phase::kRestoring);c.complete(2);c.tick(t);
  CHECK(t.phase()==Phase::kEditing&&t.notice()==Notice::kReverted&&c.saves==0);++count;
 }
 for(bool throws:{false,true}){
  Coordinator c;c.fail_once=!throws;c.throw_submit_once=throws;auto t=c.transaction();display(t);
  CHECK(!t.Apply(c.now)&&t.phase()==Phase::kRestoring&&c.submits==2&&c.saves==0);
  c.complete(2);c.tick(t);CHECK(t.phase()==Phase::kEditing&&t.notice()==Notice::kApplyFailed&&t.dirty()&&c.actual==c.initial);++count;
 }
 for(bool observation_throws:{false,true}){
  Coordinator c;auto t=c.transaction();display(t);CHECK(t.Apply(c.now));
  if(observation_throws)c.throw_observe=true;else c.failed(1);
  c.tick(t);CHECK(t.phase()==Phase::kRestoring);c.throw_observe=false;c.complete(2);c.tick(t);
  CHECK(t.phase()==Phase::kEditing&&t.notice()==Notice::kApplyFailed&&c.saves==0);++count;
 }
 for(bool throws:{false,true}){
  Coordinator c;auto t=c.transaction();display(t);CHECK(t.Apply(c.now)&&t.Revert());
  if(throws)c.throw_observe=true;else c.failed(2);c.tick(t);CHECK(t.phase()==Phase::kRecoveryFailed&&!t.Open());
  c.throw_observe=false;CHECK(t.Revert()&&t.phase()==Phase::kRestoring&&c.latest==3);
  c.complete(3);c.tick(t);CHECK(t.phase()==Phase::kEditing&&!t.dirty()&&c.saves==0);++count;
 }
 {
  Coordinator c;auto t=c.transaction();display(t);CHECK(t.Apply(c.now)&&t.Cancel());
  c.tick(t,30);CHECK(t.phase()==Phase::kRecoveryFailed);c.complete(2);c.tick(t);CHECK(t.phase()==Phase::kRecoveryFailed);
  CHECK(t.Revert());c.complete(3);c.tick(t);CHECK(t.phase()==Phase::kClosed);++count;
 }
 {
  Coordinator c;auto t=c.transaction();display(t);CHECK(t.Apply(c.now)&&t.Revert());
  CHECK(t.Cancel()&&t.Revert()&&c.submits==2);c.complete(2);c.tick(t);CHECK(t.phase()==Phase::kClosed);++count;
 }
 {
  Coordinator c;c.immediate=true;auto t=c.transaction();display(t);CHECK(t.Apply(c.now)&&t.phase()==Phase::kPreview);
  CHECK(t.Revert()&&t.phase()==Phase::kEditing&&c.actual==c.initial);++count;
 }
 {
  Coordinator c;auto t=c.transaction();CHECK(t.Open()&&t.Edit("native_fps_limit","0")&&t.Apply(c.now));
  CHECK(t.phase()==Phase::kApplying&&c.saves==0);c.complete(1);c.tick(t,2);
  CHECK(t.phase()==Phase::kEditing&&t.notice()==Notice::kSaved&&c.saves==1&&c.saved.at("native_fps_limit")=="0");++count;
 }
 {
  Coordinator c;c.save_failed=true;auto t=c.transaction();CHECK(t.Open()&&t.Edit("native_fps_limit","0")&&t.Apply(c.now));
  c.complete(1);c.tick(t);CHECK(t.phase()==Phase::kPreview&&t.notice()==Notice::kSaveFailed);
  CHECK(t.Revert()&&t.phase()==Phase::kRestoring);c.complete(2);c.tick(t);CHECK(t.phase()==Phase::kEditing&&c.saves==0);++count;
 }
 {
  Coordinator c;auto t=c.transaction();display(t);CHECK(t.Apply(c.now));c.live["fullscreen"]="false";
  c.complete(1);c.tick(t);CHECK(t.phase()==Phase::kRestoring&&c.saves==0);
  c.complete(2);c.tick(t);CHECK(t.notice()==Notice::kApplyFailed);++count;
 }
 {
  Coordinator c;auto t=c.transaction();display(t);CHECK(t.Apply(c.now)&&t.Revert());
  c.complete(2);c.live["fullscreen"]="true";c.tick(t);CHECK(t.phase()==Phase::kRecoveryFailed&&c.saves==0);++count;
 }
 for(bool only_submit:{false,true}){
  Coordinator c;auto b=c.backend();if(only_submit)b.observe={};else b.submit={};
  Transaction t(b,NativeSettingsConfirmationKeys());display(t);CHECK(!t.Apply(c.now)&&c.submits==0&&c.writes==0&&t.phase()==Phase::kEditing);++count;
 }
 {
  Coordinator c;c.zero_once=true;auto t=c.transaction();display(t);CHECK(!t.Apply(c.now)&&t.phase()==Phase::kRestoring);
  c.complete(2);c.tick(t);CHECK(t.notice()==Notice::kApplyFailed);++count;
 }
 {
  Coordinator c;auto t=c.transaction();display(t);CHECK(t.Apply(c.now));c.complete(1);c.tick(t);CHECK(t.Keep(c.now));
  CHECK(t.Edit("fullscreen","false"));c.duplicate_once=true;CHECK(!t.Apply(c.now)&&t.phase()==Phase::kRestoring);
  c.complete(3);c.tick(t);CHECK(t.phase()==Phase::kEditing&&t.notice()==Notice::kApplyFailed&&c.saves==1);++count;
 }
 {
  Coordinator c;auto t=c.transaction();display(t);CHECK(t.Apply(c.now));c.complete(1);c.tick(t);CHECK(t.Revert());c.complete(2);c.tick(t);
  CHECK(t.Edit("fullscreen","true")&&t.Apply(c.now));c.stale=1;c.tick(t);CHECK(t.phase()==Phase::kApplying&&c.saves==0);++count;
 }
 {
  UI u;u.open();u.apply();CHECK(u.menu->reply().phase==Phase::kApplying);
  CHECK(u.menu->dialog().kind==FrontendDialog::kApplying&&u.menu->dialog().buttons==std::vector<std::string>{"Cancel"});
  CHECK(!u.menu->Choose(1)&&!u.menu->Adjust(u.row("fullscreen"),1)&&u.saves==0);++count;
  u.complete(1);u.advance(4000);CHECK(u.menu->reply().phase==Phase::kPreview&&u.menu->reply().confirmation_ms==15000);
  CHECK(u.menu->dialog().kind==FrontendDialog::kKeep&&u.menu->dialog().message.find("15 seconds")!=std::string::npos);
  CHECK(u.menu->Choose(0));u.flush();CHECK(u.saves==1&&u.menu->dialog().kind==FrontendDialog::kInfo);++count;
 }
 {
  UI u;u.open();u.apply();CHECK(u.menu->Back());u.flush();CHECK(u.menu->reply().phase==Phase::kRestoring);
  CHECK(u.menu->dialog().kind==FrontendDialog::kRestoring&&u.menu->dialog().buttons.empty());
  CHECK(!u.menu->Back()&&!u.menu->Choose(0)&&!u.menu->exit_requested());++count;
  u.complete(1);u.stale=1;u.advance();CHECK(u.menu->reply().phase==Phase::kRestoring&&!u.menu->exit_requested());
  u.stale=0;u.complete(2);u.advance();CHECK(u.menu->reply().phase==Phase::kEditing&&u.menu->dialog().kind==FrontendDialog::kInfo);++count;
 }
 {
  UI u;u.open();u.apply();u.menu->Leave();u.flush();CHECK(u.menu->reply().phase==Phase::kRestoring&&!u.menu->exit_requested());
  u.complete(2);u.advance();CHECK(u.menu->exit_requested()&&u.menu->reply().phase==Phase::kClosed);++count;
 }
 {
  UI u;u.open();u.apply();auto reply=u.service->FocusLostFromUI();CHECK(reply.ok&&reply.phase==Phase::kRestoring&&u.submits==2);
  u.complete(2);u.advance();CHECK(u.menu->reply().phase==Phase::kEditing&&!u.menu->reply().dirty&&u.saves==0);++count;
 }
 {
  UI u;u.open();u.apply();u.failed(1);u.advance();CHECK(u.menu->dialog().kind==FrontendDialog::kRestoring);
  u.complete(2);u.advance();CHECK(u.menu->dialog().kind==FrontendDialog::kError&&u.menu->dialog().message.find("Consumer rejected")!=std::string::npos);++count;
 }
 {
  UI u;u.open();u.apply();u.menu->Leave();u.flush();u.failed(2);u.advance();CHECK(u.menu->dialog().kind==FrontendDialog::kRecovery);
  CHECK(!u.menu->exit_requested()&&u.menu->Choose(0));u.flush();CHECK(u.menu->dialog().kind==FrontendDialog::kRestoring);
  u.complete(3);u.advance();CHECK(u.menu->reply().phase==Phase::kClosed&&u.menu->exit_requested());++count;
 }
 {
  UI u;u.open();u.apply("native_fps_limit");u.complete(1);u.advance();
  CHECK(u.menu->reply().phase==Phase::kEditing&&u.menu->dialog().kind==FrontendDialog::kInfo&&u.saves==1);++count;
 }
 {
  UI u;u.open();u.apply();auto pending=u.service->Submit({CommandKind::kKeep});const auto observed=u.observes;
  auto stopped=u.service->ShutdownFromUI();CHECK(!stopped.ok&&stopped.phase==Phase::kRestoring&&stopped.error.find("pending")!=std::string::npos);
  CHECK(u.submits==2&&u.saves==0&&!u.service->AdvanceFromUI());
  auto denied=u.service->Submit({CommandKind::kOpen});CHECK(denied.get().unavailable);
  u.flush();CHECK(pending.get().unavailable&&u.observes==observed);++count;
 }
 std::printf("PASS %u asynchronous settings scenarios: revision receipts, delayed confirmation/save, stale/failing acknowledgements, bounded restoration, focus/shutdown, native frontend cancellation/leave/recovery\n",count);
}
