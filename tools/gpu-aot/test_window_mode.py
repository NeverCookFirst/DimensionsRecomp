"""Exercise the actual app Alt+Enter handler with a fake window/event SDK."""
import argparse
from pathlib import Path
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('output', type=Path)
a = p.parse_args()
a.output.mkdir(parents=True, exist_ok=True)
root = Path(__file__).resolve().parents[2]
s = (root/'rexlego/src/legodimensions_app.h').read_text()
start = s.index('  void OnKeyDown(rex::ui::KeyEvent& e) override {')
i = s.index('{', start)
depth, end = 1, i+1
while depth:
    depth += (s[end] == '{') - (s[end] == '}')
    end += 1
handler = s[start:end].replace(' override', '')
code = r'''
#include "window_mode.h"
#include <cassert>
#include <string>
#include <iostream>
int delegated=0, flags=0;
namespace rex::ui {
enum class VirtualKey { kReturn,kF4,kA };
struct KeyEvent {
 VirtualKey key=VirtualKey::kReturn;bool alt=true,ctrl=false,shift=false,super=false,repeat=false,handled=false;
 VirtualKey virtual_key()const{return key;}bool is_alt_pressed()const{return alt;}
 bool is_ctrl_pressed()const{return ctrl;}bool is_shift_pressed()const{return shift;}
 bool is_super_pressed()const{return super;}bool prev_state()const{return repeat;}
 void set_handled(bool v){handled=v;}
};
void ProcessKeyEvent(KeyEvent&){++delegated;}
}
namespace rex::cvar { void SetFlagByName(const char* name,const char* value){assert(std::string(name)=="fullscreen");assert(std::string(value)=="true"||std::string(value)=="false");++flags;} }
struct Window {
 bool fs=false;unsigned w=900,h=600,changes=0,monitor=3;
 bool IsFullscreen(){return fs;}unsigned GetActualLogicalWidth(){return w;}unsigned GetActualLogicalHeight(){return h;}
 void SetFullscreen(bool v){fs=v;++changes;if(v){w=1920;h=1080;}}
 void SetDesiredLogicalSize(unsigned width,unsigned height){w=width;h=height;}
};
struct App {
 Window win;bool has_window=true;legodimensions::WindowedSize windowed_size_;
 Window* window(){return has_window?&win:nullptr;}
''' + handler + r'''
};
int main(){
 App a;rex::ui::KeyEvent e;
 a.OnKeyDown(e);assert(e.handled&&a.win.fs&&flags==1&&delegated==0&&a.win.monitor==3);
 e.repeat=true;a.OnKeyDown(e);assert(a.win.fs&&a.win.changes==1&&flags==1&&delegated==0);
 e.repeat=false;a.OnKeyDown(e);assert(!a.win.fs&&a.win.w==900&&a.win.h==600&&flags==2);
 for(int modifier=0;modifier<4;++modifier){e={};if(modifier==0)e.alt=false;if(modifier==1)e.ctrl=true;if(modifier==2)e.shift=true;if(modifier==3)e.super=true;a.OnKeyDown(e);assert(!e.handled&&!a.win.fs);}
 e={};e.key=rex::ui::VirtualKey::kF4;a.OnKeyDown(e);assert(!e.handled&&delegated==5);
 a.has_window=false;e={};a.OnKeyDown(e);assert(e.handled&&flags==2&&delegated==5);
 std::cout<<"PASS: actual Alt+Enter handler, repeats consumed, modifiers respected, SDK binds preserved, null window\n";
}
'''
source = a.output/'window-event-test.cpp'
source.write_text(code)
exe = a.output/'window-event-test.exe'
subprocess.run(['clang++', '-std=c++20', '-I'+str(root/'rexlego/src'), str(source), '-o', str(exe)], check=True)
subprocess.run([str(exe.resolve())], check=True)
