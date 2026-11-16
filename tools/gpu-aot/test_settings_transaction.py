"""Test the real native-settings transaction core with injected backend faults."""
from pathlib import Path
import argparse,hashlib,json,subprocess

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('output',type=Path)
    p.add_argument('--compiler',default='clang++');a=p.parse_args()
    root=Path(__file__).resolve().parents[2];out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
    production=root/'rexlego/src/settings_transaction.cpp';header=root/'rexlego/src/settings_transaction.h'
    fixture=out/'transactions.cpp'
    fixture.write_text(r'''
#include "settings_transaction.h"
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
using namespace legodimensions::settings;
#define CHECK(x) do{if(!(x)){std::fprintf(stderr,"failed %d: %s\n",__LINE__,#x);std::exit(23);}}while(0)
unsigned checks=0;
struct Store {
 Values initial{{"fullscreen","false"},{"native_fps_limit","0"},{"native_fsr3","false"},{"native_resolution_scale","150"},{"other_setting","old"}};
 Values live=initial,disk=initial,last_saved;
 unsigned writes=0,saves=0,fail_at=0,throw_at=0;
 bool partial=false,fail_restore=false,save_fail=false,reject=false,read_fail=false;
 Backend backend(){return {
  [this]{if(read_fail)throw std::runtime_error("read failure");return live;},
  [this](std::string_view key,std::string_view value){
   if(reject||value=="invalid")return NormalizedValue{false,{},"unavailable"};
   return NormalizedValue{true,std::string(value),{}};},
  [this](std::string_view key,std::string_view value){
   ++writes;const std::string k(key),v(value);
   if(partial||writes!=fail_at)live[k]=v;
   if(writes==throw_at)throw std::runtime_error("setter failure");
   if(fail_restore&&v==initial.at(k))return false;
   return writes!=fail_at;},
  [this](const Values& changes,std::string& error){
   if(save_fail){error="config locked";return false;}
   ++saves;last_saved=changes;for(const auto&[k,v]:changes)disk[k]=v;return true;}
 };}
};
void edit_all(Transaction&t){CHECK(t.Edit("fullscreen","true"));CHECK(t.Edit("native_fps_limit","120"));
 CHECK(t.Edit("native_fsr3","true"));CHECK(t.Edit("native_resolution_scale","300"));}
auto make(Store&s){return Transaction(s.backend(),{"fullscreen","native_resolution_scale","native_fsr3"});}
int main(){
 const Clock::time_point now=Clock::time_point(std::chrono::seconds(100));
 {
 Store s;auto t=make(s);CHECK(!t.Edit("fullscreen","true"));CHECK(!t.Cancel());
 CHECK(t.Open()&&!t.Open());CHECK(t.Apply(now)&&s.writes==0&&s.saves==0);
 CHECK(t.Edit("native_fps_limit","120"));CHECK(s.live==s.initial);
 CHECK(!t.Edit("unknown","3"));const auto draft=t.draft();
 CHECK(!t.Edit("native_fps_limit","invalid")&&t.draft()==draft);
 CHECK(!t.Defaults({{"native_fps_limit","60"},{"unknown","x"}})&&t.draft()==draft);
 CHECK(t.Cancel()&&s.live==s.initial&&s.disk==s.initial);++checks;
 }
 {
 Store s;auto t=make(s);CHECK(t.Open());edit_all(t);CHECK(t.Apply(now));
 CHECK(t.phase()==Phase::kPreview&&t.notice()==Notice::kKeepChanges&&s.saves==0);
 CHECK(!t.Edit("native_fps_limit","60")&&!t.Apply(now)&&!t.Open());
 CHECK(t.Keep(now+std::chrono::seconds(1)));CHECK(t.phase()==Phase::kEditing&&!t.dirty());
 CHECK(t.notice()==Notice::kSaved&&s.saves==1&&s.disk==s.live);
 CHECK(t.Cancel()&&s.disk.at("native_resolution_scale")=="300");++checks;
 }
 for(bool cancel:{false,true}){
 Store s;auto t=make(s);CHECK(t.Open());edit_all(t);CHECK(t.Apply(now));
 CHECK(cancel?t.Cancel():t.Revert());CHECK(s.live==s.initial&&s.disk==s.initial&&s.saves==0);
 CHECK(!t.dirty()&&t.phase()==(cancel?Phase::kClosed:Phase::kEditing));++checks;
 }
 for(bool keep:{false,true}){
 Store s;auto t=make(s);CHECK(t.Open());edit_all(t);CHECK(t.Apply(now));
 const auto deadline=t.deadline();t.Tick(deadline-std::chrono::nanoseconds(1));CHECK(t.phase()==Phase::kPreview);
 if(keep)CHECK(!t.Keep(deadline));else t.Tick(deadline);
 CHECK(t.phase()==Phase::kEditing&&t.notice()==Notice::kReverted&&s.live==s.initial&&s.saves==0);++checks;
 }
 for(unsigned failure=1;failure<=4;++failure)for(bool partial:{false,true})for(bool throws:{false,true}){
 Store s;s.partial=partial;s.fail_at=throws?0:failure;s.throw_at=throws?failure:0;
 auto t=make(s);CHECK(t.Open());edit_all(t);CHECK(!t.Apply(now));
 CHECK(t.phase()==Phase::kEditing&&t.notice()==Notice::kApplyFailed);
 CHECK(s.live==s.initial&&s.disk==s.initial&&s.saves==0&&t.dirty());++checks;
 }
 {
 Store s;auto t=make(s);CHECK(t.Open());edit_all(t);s.live["native_fps_limit"]="90";
 CHECK(!t.Apply(now)&&t.notice()==Notice::kConflict&&s.writes==0&&s.saves==0);++checks;
 }
 {
 Store s;auto t=make(s);CHECK(t.Open());edit_all(t);s.reject=true;
 CHECK(!t.Apply(now)&&t.notice()==Notice::kInvalidValue&&s.writes==0);++checks;
 }
 for(bool display:{false,true}){
 Store s;auto t=make(s);CHECK(t.Open());
 if(display)edit_all(t);else CHECK(t.Edit("native_fps_limit","120"));
 s.save_fail=true;
 if(display){CHECK(t.Apply(now));CHECK(!t.Keep(now));}else CHECK(!t.Apply(now));
 CHECK(t.phase()==Phase::kPreview&&t.notice()==Notice::kSaveFailed&&s.disk==s.initial);
 s.save_fail=false;CHECK(t.Keep(now+std::chrono::seconds(2)));
 CHECK(s.disk==s.live&&!t.dirty()&&s.saves==1);++checks;
 }
 {
 Store s;auto t=make(s);CHECK(t.Open());edit_all(t);CHECK(t.Apply(now));s.save_fail=true;
 CHECK(!t.Keep(now));t.Tick(t.deadline());CHECK(s.live==s.initial&&s.disk==s.initial);++checks;
 }
 {
 Store s;auto t=make(s);CHECK(t.Open());edit_all(t);CHECK(t.Apply(now));s.fail_restore=true;
 CHECK(!t.Cancel()&&t.phase()==Phase::kRecoveryFailed&&t.notice()==Notice::kRecoveryFailed);
 CHECK(s.saves==0);s.fail_restore=false;CHECK(t.Revert());CHECK(s.live==s.initial&&t.Cancel());++checks;
 }
 {
 Store s;auto t=make(s);CHECK(t.Open());CHECK(t.Edit("native_fps_limit","60"));
 CHECK(t.Apply(now)&&t.notice()==Notice::kSaved&&s.saves==1&&t.phase()==Phase::kEditing);
 CHECK(s.last_saved.size()==1&&s.last_saved.at("native_fps_limit")=="60");++checks;
 }
 {
 Store s;auto t=make(s);CHECK(t.Open());edit_all(t);CHECK(t.Apply(now));s.live["native_fps_limit"]="45";
 CHECK(!t.Keep(now)&&t.notice()==Notice::kConflict&&s.saves==0);CHECK(t.Revert());++checks;
 }
 {
 Store s;auto t=make(s);CHECK(t.Open());edit_all(t);CHECK(t.Apply(now));s.live["other_setting"]="external";
 CHECK(t.Keep(now)&&!t.dirty()&&t.draft().at("other_setting")=="external");
 CHECK(!s.last_saved.contains("other_setting"));++checks;
 }
 {
 Store s;s.read_fail=true;auto t=make(s);CHECK(!t.Open()&&t.phase()==Phase::kClosed);
 s.read_fail=false;CHECK(t.Open());CHECK(t.Defaults({{"native_fps_limit","60"},{"native_resolution_scale","100"}}));
 CHECK(s.live==s.initial&&t.dirty());CHECK(t.Apply(now,std::chrono::seconds(-30)));
 CHECK(t.deadline()==now+std::chrono::seconds(1));t.Tick(t.deadline());CHECK(s.live==s.initial);++checks;
 }
 std::printf("PASS %u transaction scenarios: staged edits/defaults, partial and throwing setters, save failures, conflicts, confirmation deadline, keep/revert/cancel, recovery and unchanged-key preservation\n",checks);
}
''')
    def run(label,source):
        exe=out/(label+'.exe')
        result=subprocess.run([a.compiler,'-std=c++20','-O2','-I'+str(root/'rexlego/src'),str(fixture),str(source),'-o',str(exe)],capture_output=True,text=True,timeout=45)
        (out/(label+'-compile.log')).write_text(result.stdout+result.stderr);result.check_returncode()
        result=subprocess.run([str(exe)],capture_output=True,text=True,timeout=15)
        (out/(label+'.log')).write_text(result.stdout+result.stderr);return result
    positive=run('production',production);positive.check_returncode();print(positive.stdout,end='')
    text=production.read_text();negative=[]
    for label,old,new in [('save-before-keep','if (needs_confirmation) {','if (false) {'),
                          ('keep-after-expiry','if (now >= deadline_) {','if (false) {'),
                          ('skip-rollback','if (!backend_.write(*it, baseline_.at(*it))) {','if (false) {')]:
        assert text.count(old)==1
        mutant=out/(label+'.cpp');mutant.write_text(text.replace(old,new))
        result=run(label,mutant);assert result.returncode==23,(label,result.returncode,result.stdout,result.stderr)
        negative.append(dict(name=label,exit=result.returncode,error=result.stderr))
    (out/'verification.json').write_text(json.dumps(dict(passed=True,game_launched=False,
        source_sha256=hashlib.sha256(production.read_bytes()).hexdigest(),header_sha256=hashlib.sha256(header.read_bytes()).hexdigest(),
        output=positive.stdout,negative=negative,
        scope='Actual transaction implementation; registry/window/persistence callbacks are fault-injected backend boundaries. Native UI binding, actual window/render acknowledgement and atomic file writer need separate verification.'),indent=2))
if __name__=='__main__':main()
