#include "gpu_native/native_overlay.h"
#include "gpu_native/overlay_packet.h"
#include "gpu_native/fsr3_settings.h"
#include "gpu_native/temporal_present.h"
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/immediate_drawer.h>
#include <rex/ui/overlay/debug_overlay.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/windowed_app_context.h>
#include <rex/cvar.h>
#include <imgui.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>

namespace legodimensions::gpu_native {
namespace {
struct Bridge {
  rex::ui::WindowedAppContext* context=nullptr;
  rex::ui::ImGuiDrawer* drawer=nullptr;
  std::atomic<bool> alive{true},pending{false};
  std::atomic<uint32_t> width{0},height{0};
  std::atomic<std::shared_ptr<const OverlayPacket>> packet;
};
std::atomic<std::shared_ptr<Bridge>> bridge;
std::atomic<bool> device_ready{false};
std::atomic<double> stats_fps{0},stats_ms{0};
std::atomic<uint64_t> stats_frame{0};
thread_local std::shared_ptr<OverlayPacket> built_packet;
class Texture final:public rex::ui::ImmediateTexture {
 public:
  explicit Texture(std::shared_ptr<OverlayTexture> p)
      :ImmediateTexture(p->width,p->height),pixels(std::move(p)){}
  std::shared_ptr<OverlayTexture> pixels;
};
class Drawer final:public rex::ui::ImmediateDrawer {
 public:
  std::unique_ptr<rex::ui::ImmediateTexture> CreateTexture(uint32_t w,uint32_t h,
      rex::ui::ImmediateTextureFilter filter,bool repeat,const uint8_t* data) override {
    const uint64_t bytes=uint64_t(w)*h*4;
    if(!device_ready.load()||!data||!w||!h||w>8192||h>8192||bytes>32*1024*1024)return {};
    auto p=std::make_shared<OverlayTexture>();p->width=w;p->height=h;
    p->linear=filter==rex::ui::ImmediateTextureFilter::kLinear;p->repeat=repeat;
    p->rgba.assign(data,data+size_t(bytes));return std::make_unique<Texture>(std::move(p));
  }
  void Begin(rex::ui::UIDrawContext& context,float w,float h) override {
    ImmediateDrawer::Begin(context,w,h);packet_=std::make_shared<OverlayPacket>();
    packet_->logical_width=w;packet_->logical_height=h;
    packet_->width=context.render_target_width();packet_->height=context.render_target_height();
    valid_batch_=false;
  }
  void BeginDrawBatch(const rex::ui::ImmediateDrawBatch& batch) override {
    valid_batch_=false;
    if(!packet_||!batch.vertices||batch.vertex_count<=0||batch.vertex_count>65536||
        batch.index_count<0||packet_->vertices.size()+batch.vertex_count>1048576||
        packet_->indices.size()+std::max(batch.index_count,batch.vertex_count)>2097152)return;
    vertex_base_=uint32_t(packet_->vertices.size());index_base_=uint32_t(packet_->indices.size());
    static_assert(sizeof(OverlayVertex)==sizeof(rex::ui::ImmediateVertex));
    const auto previous=packet_->vertices.size();packet_->vertices.resize(previous+batch.vertex_count);
    std::memcpy(packet_->vertices.data()+previous,batch.vertices,size_t(batch.vertex_count)*sizeof(OverlayVertex));
    if(batch.index_count){
      if(!batch.indices)return;
      packet_->indices.insert(packet_->indices.end(),batch.indices,batch.indices+batch.index_count);
    }else for(int i=0;i<batch.vertex_count;++i)packet_->indices.push_back(uint16_t(i));
    batch_count_=batch.index_count?batch.index_count:batch.vertex_count;valid_batch_=true;
  }
  void Draw(const rex::ui::ImmediateDraw& d) override {
    if(!valid_batch_||d.count<=0||d.index_offset<0||d.base_vertex<0||
        uint64_t(d.index_offset)+d.count>batch_count_||packet_->draws.size()>=65536)return;
    OverlayDraw draw;
    if(d.texture)draw.texture=static_cast<Texture*>(d.texture)->pixels;
    draw.count=d.count;draw.first=index_base_+d.index_offset;draw.base=vertex_base_+d.base_vertex;
    draw.lines=d.primitive_type==rex::ui::ImmediatePrimitiveType::kLines;
    draw.scissor=d.scissor;draw.clip={d.scissor_left,d.scissor_top,d.scissor_right,d.scissor_bottom};
    if(OverlayDrawRange(*packet_,draw))packet_->draws.push_back(std::move(draw));
  }
  void EndDrawBatch() override {valid_batch_=false;}
  void End() override {built_packet=std::move(packet_);ImmediateDrawer::End();}
 private:
  std::shared_ptr<OverlayPacket> packet_;
  uint32_t vertex_base_=0,index_base_=0,batch_count_=0;
  bool valid_batch_=false;
};
class GraphicsMenu final:public rex::ui::ImGuiDialog {
 public:
  explicit GraphicsMenu(rex::ui::ImGuiDrawer* drawer):ImGuiDialog(drawer){}
  void OnDraw(ImGuiIO& io) override {
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::SetNextWindowSize(ImVec2(530,0),ImGuiCond_FirstUseEver);
    if(ImGui::Begin("Native graphics - F5 to close")){
      const auto scene=NativeSceneRenderSize();
      const auto shared=bridge.load();
      const uint32_t window_width=shared?shared->width.load():0,window_height=shared?shared->height.load():0;
      if(NativeFsr3Enabled())
        ImGui::Text("%ux%u (%up) FSR 3 to 1920x1080 | Window %ux%u",
            scene.width,scene.height,scene.height,window_width,window_height);
      else
        ImGui::Text("%ux%u (%up) at %u%% | Window %ux%u",
            scene.width,scene.height,scene.height,NativeResolutionScale(),window_width,window_height);
      auto stats=NativeOverlayStats();ImGui::Text("%.0f FPS | %.2f ms",stats.fps,stats.frame_time_ms);
      bool show_fps=rex::cvar::Query<bool>("native_show_fps");
      if(ImGui::Checkbox("FPS meter",&show_fps))rex::cvar::SetFlagByName("native_show_fps",show_fps?"true":"false");
      bool bloom=rex::cvar::Query<bool>("native_bloom");
      if(ImGui::Checkbox("Bloom glow",&bloom))rex::cvar::SetFlagByName("native_bloom",bloom?"true":"false");
      const char* cel_names[]{"Original","Everywhere"};
      const char* cel_values[]{"original","everywhere"};
      const auto cel_value=rex::cvar::GetFlagByName("native_cel_shading");
      int cel=cel_value=="everywhere"?1:0;
      if(ImGui::Combo("Cel shading",&cel,cel_names,2))rex::cvar::SetFlagByName("native_cel_shading",cel_values[cel]);
      ImGui::TextWrapped("Presentation: %s",NativeTemporalPresentationStatus());
      const char* names[]{"Off","FSR 1 (ReXGlue)","FSR 3 Native AA","FSR 3 Quality","FSR 3 Balanced","FSR 3 Performance","FSR 3 Ultra Performance"};
      const char* values[]{"native","quality","balanced","performance","ultra_performance"};
      auto mode=rex::cvar::GetFlagByName("native_fsr3_quality");
      const bool enabled=rex::cvar::GetFlagByName("native_fsr3")=="true";
      int selected=enabled?3:(rex::cvar::GetFlagByName("present_effect")=="fsr"?1:0);
      if(enabled)for(int i=0;i<5;++i)if(mode==values[i])selected=i+2;
      if(ImGui::IsWindowAppearing())ImGui::SetKeyboardFocusHere();
      if(ImGui::Combo("Upscaling",&selected,names,7)){
        if(selected>=2)rex::cvar::SetFlagByName("native_fsr3_quality",values[selected-2]);
        rex::cvar::SetFlagByName("present_effect",selected==1?"fsr":"bilinear");
        rex::cvar::SetFlagByName("native_fsr3",selected>=2?"true":"false");
      }
      // A drag commits once on release so scene surfaces are not rebuilt every frame.
      static int dragged_scale=-1;
      int percent=dragged_scale>=0?dragged_scale:int(NativeRequestedResolutionScale());
      const auto preview=NativeScaledSceneSize(uint32_t(percent));
      char format[64];
      std::snprintf(format,sizeof(format),"%%d%%%% (%ux%u, %up)",preview.width,preview.height,preview.height);
      ImGui::BeginDisabled(enabled);
      if(ImGui::SliderInt("Internal resolution",&percent,20,300,format,ImGuiSliderFlags_AlwaysClamp))
        dragged_scale=int(NativeResolutionScaleStep(percent));
      if(dragged_scale>=0 && !ImGui::IsItemActive()) {
        rex::cvar::SetFlagByName("native_resolution_scale",std::to_string(dragged_scale));
        dragged_scale=-1;
      }
      ImGui::EndDisabled();
      if(enabled)ImGui::TextDisabled("Set by FSR 3");
      if(selected==1) {
        ImGui::TextWrapped("FSR 1: EASU + RCAS, as in ReXGlue. Internal resolution sets the input size; 100%% is 1280x720.");
        float reduction=float(rex::cvar::Query<double>("present_fsr_sharpness_reduction"));
        if(ImGui::SliderFloat("FSR 1 sharpness reduction",&reduction,0,2))
          rex::cvar::SetFlagByName("present_fsr_sharpness_reduction",std::to_string(reduction));
      }
      ImGui::BeginDisabled(!enabled);
      const auto detail=NativeFsr3RequestedDetails();
      bool sharpen=detail.sharpen;
      if(ImGui::Checkbox("Sharpening",&sharpen))rex::cvar::SetFlagByName("native_fsr3_sharpen",sharpen?"true":"false");
      float sharpness=detail.sharpness;
      if(ImGui::SliderFloat("Sharpness",&sharpness,0,1))rex::cvar::SetFlagByName("native_fsr3_sharpness",std::to_string(sharpness));
      float bias=detail.mip_bias_adjustment;
      if(ImGui::SliderFloat("Texture detail adjustment",&bias,-1,1))rex::cvar::SetFlagByName("native_fsr3_mip_bias",std::to_string(bias));
      ImGui::EndDisabled();
      int cap=240;try{cap=std::stoi(rex::cvar::GetFlagByName("native_fps_limit"));}catch(...){}
      if(ImGui::SliderInt("FPS cap (0 = uncapped)",&cap,0,360))rex::cvar::SetFlagByName("native_fps_limit",std::to_string(cap));
      ImGui::Separator();
      ImGui::TextDisabled("F3 stats  F4 settings (Save keeps these)  F7 achievements  F8 mods  Del cheats");
    }
    ImGui::End();
  }
};
}
std::unique_ptr<rex::ui::ImmediateDrawer> CreateNativeImmediateDrawer(){return std::make_unique<Drawer>();}
std::unique_ptr<rex::ui::ImGuiDialog> CreateNativeGraphicsMenu(rex::ui::ImGuiDrawer* drawer){return std::make_unique<GraphicsMenu>(drawer);}
void AttachNativeOverlays(rex::ui::WindowedAppContext& context,rex::ui::ImGuiDrawer* drawer){
  DetachNativeOverlays();auto b=std::make_shared<Bridge>();b->context=&context;b->drawer=drawer;bridge.store(std::move(b));
}
void DetachNativeOverlays(){if(auto b=bridge.exchange({})){b->alive=false;b->packet.store({});}}
void QueueNativeOverlayFrame(uint32_t w,uint32_t h){
  auto b=bridge.load();if(!b||!b->alive||!w||!h)return;
  b->width=w;b->height=h;if(b->pending.exchange(true))return;
  if(!b->context->CallInUIThreadDeferred([b]{
    if(b->alive){
      built_packet.reset();rex::ui::AppUIDrawContext context(b->width.load(),b->height.load());
      if(b->drawer&&b->drawer->HasDialogs())b->drawer->Draw(context);
      b->packet.store(std::move(built_packet));
    }
    b->pending=false;
  }))b->pending=false;
}
std::shared_ptr<const OverlayPacket> NativeOverlayPacket(){auto b=bridge.load();return b?b->packet.load():nullptr;}
void SetNativeOverlayDeviceReady(bool ready){device_ready=ready;}
void PublishNativeOverlayStats(double fps,double ms,uint64_t frame){stats_fps=fps;stats_ms=ms;stats_frame=frame;}
rex::ui::FrameStats NativeOverlayStats(){return {stats_ms.load(),stats_fps.load(),stats_frame.load()};}
} // namespace legodimensions::gpu_native
