"""Run production mouse handlers with real ImGui and the controller key latch."""
import argparse
from pathlib import Path
import subprocess
p=argparse.ArgumentParser();p.add_argument('output',type=Path);p.add_argument('--compiler',default='clang++');a=p.parse_args()
root=Path(__file__).resolve().parents[2];a.output.mkdir(parents=True,exist_ok=True)
source=(root/'rexglue-sdk/src/ui/imgui_drawer.cpp').read_text()
def body(signature):
 start=source.index(signature);end=source.index('{',start)+1;depth=1
 while depth: depth+=(source[end]=='{')-(source[end]=='}');end+=1
 return source[start:end]
functions='\n'.join(body(s) for s in ['void ImGuiDrawer::OnMouseDown(', 'void ImGuiDrawer::OnMouseUp(', 'void ImGuiDrawer::UpdateMousePosition('])
cpp=a.output/'input-events.cpp'
cpp.write_text(r'''
#include <imgui.h>
#include <rex/input/mnk/key_state_latch.h>
#include <cassert>
#include <cstdint>
#include <iterator>
namespace rex::ui {
struct Window {int captures=0,releases=0;void CaptureMouse(){++captures;}void ReleaseMouse(){++releases;}int GetMediumDpi(){return 96;}int GetDpi(){return 144;}};
struct MouseEvent {enum class Button{kLeft,kRight};Button value;Button button(){return value;}};
struct ImGuiDrawer {
 Window* window_;uint32_t physical_mouse_buttons_=0;
 ImGuiIO& GetIO(){return ImGui::GetIO();}
 void SwitchToPhysicalMouseAndUpdateMousePosition(MouseEvent&) {UpdateMousePosition(150,120);}
 void OnMouseDown(MouseEvent&);void OnMouseUp(MouseEvent&);void UpdateMousePosition(float,float);
};
'''+functions+r'''
}
void frame(){ImGui::NewFrame();ImGui::EndFrame();}
int main(){
 rex::input::KeyStateLatch keys;
 keys.Set(85,true);keys.Set(85,false);assert(keys.Poll()[85]);assert(!keys.Poll()[85]);
 keys.Set(13,true);assert(keys.Poll()[13]);assert(keys.Poll()[13]);keys.Set(13,false);assert(!keys.Poll()[13]);
 keys.Set(85,true);keys.Set(85,false);keys.Clear();assert(!keys.Poll()[85]);keys.Set(65535,true);
 ImGui::CreateContext();auto& io=ImGui::GetIO();io.IniFilename=nullptr;io.DisplaySize={640,480};io.DeltaTime=1.f/30;
 unsigned char* pixels;int w,h;io.Fonts->GetTexDataAsRGBA32(&pixels,&w,&h);frame();
 rex::ui::Window window;rex::ui::ImGuiDrawer drawer{&window};rex::ui::MouseEvent left{rex::ui::MouseEvent::Button::kLeft};
 drawer.OnMouseDown(left);drawer.OnMouseUp(left);assert(window.captures==1&&window.releases==1);
 // Both events arrive between 30 FPS frames: preserve the click, then release.
 ImGui::NewFrame();assert(ImGui::IsMouseClicked(0)&&io.MouseDown[0]);assert(io.MousePos.x==100&&io.MousePos.y==80);ImGui::EndFrame();
 ImGui::NewFrame();assert(ImGui::IsMouseReleased(0)&&!io.MouseDown[0]);ImGui::EndFrame();
 ImGui::DestroyContext();
}
''')
imgui=root/'rexglue-sdk/thirdparty/imgui';exe=a.output/'input-events.exe'
subprocess.run([a.compiler,'-std=c++20','-O2','-UNDEBUG','-I'+str(imgui),'-I'+str(root/'rexglue-sdk/include'),str(cpp),
 *[str(imgui/f) for f in ['imgui.cpp','imgui_draw.cpp','imgui_tables.cpp','imgui_widgets.cpp']],'-o',str(exe)],check=True,timeout=90)
subprocess.run([str(exe.resolve())],check=True,timeout=10)
print('PASS: actual mouse handlers with real ImGui preserve fast clicks and DPI; key taps, holds, releases, focus clear')
