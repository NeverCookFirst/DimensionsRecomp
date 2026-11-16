"""Exercise the production draw camera-selection block with world and menu views."""
import argparse, os
from pathlib import Path
import subprocess
p=argparse.ArgumentParser();p.add_argument('output',type=Path);p.add_argument('--compiler',default='clang++');a=p.parse_args()
root=Path(__file__).resolve().parents[2];draw=(root/'rexlego/src/gpu_native/draw.cpp').read_text()
profiles=draw[draw.index('bool HasVerifiedTemporalCamera('):draw.index('struct SharedConstants')]
begin=draw.index('  float temporal_jitter_x = 0, temporal_jitter_y = 0;')
selection=draw[begin:draw.index('  // A raw depth texture view',begin)]
history_begin=draw.index('    auto previous = TemporalPreviousConstants{};',draw.index('  const auto bind_temporal_previous'))
history=draw[history_begin:draw.index('    TemporalDrawKey identity{};',history_begin)]
a.output.mkdir(parents=True,exist_ok=True);cpp=a.output/'camera-owner.cpp';exe=a.output/'camera-owner.exe'
cpp.write_text(r'''
#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <set>
#include <tuple>
#include <vector>
#include "gpu_native/temporal_camera.h"
#include "gpu_native/temporal_camera_authority.h"
#include "gpu_native/temporal_texture_history.h"
using namespace legodimensions::gpu_native;
using u32=uint32_t;using u64=uint64_t;
#undef assert
#define assert(x) ((x)?void(0):std::exit(23))
#define REXLOG_INFO(...) ((void)0)
struct TemporalPreviousConstants {};
enum class ShaderStage { kVertex };
unsigned vertex_texture_mask=0;
bool verified_texture_contents=false;
unsigned BoundShaderTextureMask(ShaderStage){return vertex_texture_mask;}
struct Blend {bool enabled;};
Blend DecodeColorBlend(unsigned word){return {word!=0};}
std::atomic<u64> g_probe_frame{1};
TemporalCameraAuthority g_temporal_camera_authority;
bool HasTemporalCameraConstants(u64,u32){return true;}
std::vector<std::optional<TemporalCamera>> observed;
void PrepareTemporalDraw(void*,u64,u32,u32,const std::optional<TemporalCamera>& camera,float&x,float&y){
 observed.push_back(camera);x=.25f;y=-.25f;
}
'''+profiles+r'''
void Draw(u64 shader,float n,float f,float fov){
 struct {u64 vertex_shader_hash,pixel_shader_hash;u32 spec_constants;} key{shader,1,0};
 struct {u32 render_targets[1],depth_stencil;} bindings{{10},11};
 struct {u32 width=1280,height=720;} raster_extent;
 std::array<unsigned char,8816> temporal_snapshot{};
 const float y=1/std::tan(fov/2),a=f/(f-n),b=-n*a;
 const std::array<float,16> matrix{y,0,0,0,0,y,0,0,0,0,a,1,0,0,b,0};
 std::memcpy(temporal_snapshot.data(),matrix.data(),sizeof(matrix));
 const bool temporal_scene=true;void* commands=nullptr;
'''+selection+r'''
 assert(temporal_jitter_x==.25f&&temporal_jitter_y==-.25f);
}
bool HistoryEligible(const TemporalCameraAuthority::Matrix& matrix,
    unsigned flags=0, bool blend=false, bool full_scene=true, bool screen_space=false,
    u64 shader=0xACAF01F01E1C03D2ull){
 struct {u64 vertex_shader_hash=0xACAF01F01E1C03D2ull;unsigned pixel_shader=1,depth_control;unsigned blend_controls[1];}key;
 key.depth_control=flags;key.blend_controls[0]=blend;
 key.vertex_shader_hash=shader;
 struct {void*texture=reinterpret_cast<void*>(1);}color,depth;
 struct {unsigned logical_width=1280,logical_height=720;}raster_extent;
 struct {struct {float x=0,y=0;}viewport;}object;
 auto*device=&object;float viewport_width=full_scene?1280:640,viewport_height=720;
 std::array<unsigned char,8816>temporal_snapshot{};
 std::memcpy(temporal_snapshot.data(),matrix.data(),sizeof(matrix));
 const bool indexed=false;
 TemporalTextureHistory temporal_textures;temporal_textures.required=vertex_texture_mask;
 if(verified_texture_contents)for(unsigned i=0;i<32;++i)temporal_textures.Observe(i,123,true);
'''+history+r'''
 return eligible;
}
int main(int argc,char**){
 assert(HasVerifiedTemporalTransform(0xC29DAB774F404DB9ull));
 assert(!HasVerifiedTemporalCamera(0xC29DAB774F404DB9ull));
 assert(HasVerifiedTemporalTransform(0xB8D5551806386C1Bull));
 assert(!HasVerifiedTemporalCamera(0xB8D5551806386C1Bull));
 // Menu background and 3D UI have valid, different perspective matrices.
 // The UI's matrix-register layout does not authorize ownership of world depth.
 Draw(0x6CB6E1C4106F4571ull,.05f,1000.f,.66f);
 Draw(0xACAF01F01E1C03D2ull,.2f,500.f,.75f);
 Draw(0xC0C579A9C7D3ACD9ull,.05f,1000.f,.66f);
 Draw(0xB8D5551806386C1Bull,.05f,1000.f,.66f);
 assert(observed.size()==4&&observed[0]&&!observed[1]&&observed[2]&&!observed[3]);
 assert(std::abs(observed[0]->near_plane-.05f)<.00001f);
 assert(std::abs(observed[2]->vertical_fov-.66f)<.00001f);
 TemporalCameraAuthority::Matrix scene{1,0,0,0,0,1,0,0,0,0,1.001f,1,0,0,-.05f,0};
 g_temporal_camera_authority.Reset();
 assert(!HistoryEligible(scene));
 assert(!HistoryEligible(scene,0,false,true,false,0xB8D5551806386C1Bull));
 g_temporal_camera_authority.Observe(1,scene);
 assert(HistoryEligible(scene));
 auto separate_view=scene;separate_view[12]=10;
 assert(!HistoryEligible(separate_view)); // Same FOV/depth, different camera.
 assert(!HistoryEligible(scene,1)); // Stencil remains conservative.
 assert(!HistoryEligible(scene,0,true));
 assert(!HistoryEligible(scene,0,false,false));
 assert(!HistoryEligible(scene,0,false,true,true));
 vertex_texture_mask=1;assert(!HistoryEligible(scene));
 verified_texture_contents=true;assert(HistoryEligible(scene)==(argc==1));
 assert(HistoryEligible(scene,0,false,true,false,0xB8D5551806386C1Bull)==(argc==1));
 assert(!HistoryEligible(separate_view,0,false,true,false,0xB8D5551806386C1Bull));
 assert(!HistoryEligible(separate_view));
 verified_texture_contents=false;vertex_texture_mask=0;
 g_probe_frame=2;assert(!HistoryEligible(scene));
}
''')
subprocess.run([a.compiler,'-std=c++20','-O2','-UNDEBUG','-I'+str(root/'rexlego/src'),str(cpp),'-o',str(exe)],check=True)
env=dict(os.environ);env.pop('LEGO_NATIVE_VERTEX_TEXTURE_HISTORY',None)
subprocess.run([str(exe.resolve())],env=env,check=True)
env['LEGO_NATIVE_VERTEX_TEXTURE_HISTORY']='0'
subprocess.run([str(exe.resolve()),'disabled'],env=env,check=True)
mutant=a.output/'camera-owner-mutant.cpp';bad_exe=a.output/'camera-owner-mutant.exe'
mutation='g_temporal_camera_authority.Matches(g_probe_frame.load(),current_camera)'
assert mutation in cpp.read_text()
mutant.write_text(cpp.read_text().replace(mutation,'true'))
subprocess.run([a.compiler,'-std=c++20','-O2','-UNDEBUG','-I'+str(root/'rexlego/src'),str(mutant),'-o',str(bad_exe)],check=True,capture_output=True)
assert subprocess.run([str(bad_exe.resolve())]).returncode==23
print('PASS production camera ownership and history eligibility: secondary views, stale frames, stencil, blending, unverified vertex textures and non-scene viewports stay rejected')
print('PASS removing the full scene-matrix gate fails the production eligibility checks')
