// Standalone CPU fixture. Never used as a game entry point.
#define NOMINMAX
#include "legodimensions_pch.h"
#include "settings_native_bridge.h"
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <map>
#include <regex>
#include <vector>
#include <thread>
#include <barrier>
#undef DEFINE_REX_FUNC
#define DEFINE_REX_FUNC(name) void name(PPCContext&ctx,uint8_t*base)
#define CHECK(c) do{if(!(c)){std::fprintf(stderr,"line %d failed: %s\n",__LINE__,#c);std::exit(23);}}while(0)
#include "native-declarations.h"
using namespace legodimensions::settings;
using legodimensions::native_frontend::CallGuest;
namespace native_frontend=legodimensions::native_frontend;
constexpr uint32_t arena=0x20000000,manager=arena+0x1000,pages=arena+0x2000,
 interfaces=arena+0x3000,options=arena+0x4000,ui=arena+0x5000,
 components=arena+0x6000,registry=arena+0x7000,generic=arena+0x8000,
 footer=arena+0x9000,state=arena+0xA000,session=arena+0xB000,
 layers=arena+0xC000,foreign_interface=arena+0xD000,main_component=arena+0xE000;
struct Fixture;
Fixture* current=nullptr;
std::atomic<NativeSettingsBridge*> literal_bridge{nullptr};
void Name(PPCContext&,uint8_t*);
void Visible(PPCContext&,uint8_t*);
void ForeignEvent(PPCContext&,uint8_t*);
void Noop(PPCContext&,uint8_t*);
void OptionsEvent(PPCContext&,uint8_t*);
void PauseRow(PPCContext&,uint8_t*);
void PauseOriginal(PPCContext&,uint8_t*);
unsigned captured_input=0;
void CaptureInput(PPCContext&ctx,uint8_t*){captured_input=ctx.r4.u32;}
struct Fixture {
 uint8_t*base;PPCContext ctx{};uint32_t next=arena+0x100000,thunks=0;
 uint32_t widget_vtable=0,option_root=0,generic_root=0,message=0,menu=0,yes=0,no=0,
  video_row=0,effects_row=0,controls_row=0,dlc_row=0,main_root=0,main_child=0;
 unsigned quit_requests=0;
 unsigned plain_calls=0,translated_calls=0;uint32_t last_plain_arg=0;
 std::vector<unsigned> sounds;
 unsigned focus_events=0;
 uint32_t unreadable_root=0;
 unsigned allocations=0,fail_allocation=0,thunk_calls=0,fail_thunk=0,loads=0,
  refreshes=0,publishes=0,configs=0,writes=0,saves=0,foreign_events=0;
 bool missing_service=false,deny_layout=false,missing_widget=false,save_failure=false,restore_failure=false;
 bool scene_loading=false;
 std::vector<std::string> pause_labels;
 std::vector<double> pause_draw_y,pause_draw_width;
 std::vector<uint32_t> layer_pages;
 void publish_layer(){
  REX_STORE_U32(layers+32,uint32_t(layer_pages.size()));
  const auto node=layer_pages.empty()?0:raw(16);
  if(node)REX_STORE_U32(node+8,layer_pages.back());
  REX_STORE_U32(layers+28,node);
 }
 unsigned legacy_updates=0;
 unsigned dynamic_clones=0;
 int popup_index=0;
 uint64_t revision=0,completed_revision=0,stale_revision=0;
 std::map<uint32_t,std::string> names,texts;
 std::map<uint32_t,uint32_t> xml_names;
 std::map<uint32_t,uint32_t> name_addresses;
 std::map<std::string,uint32_t> roots;
 std::map<uint32_t,std::map<std::string,uint32_t>> children;
 std::deque<std::function<void()>> jobs;
 Values live=NativeSettingsDefaults(),saved;
 Clock::time_point time{};
 std::shared_ptr<QueuedSettings> service;
 std::unique_ptr<NativeSettingsBridge> bridge;
 explicit Fixture(uint8_t*memory,const std::vector<char>&image,bool asynchronous=false);
 ~Fixture(){literal_bridge.store(nullptr);bridge.reset();service->ShutdownFromUI();current=nullptr;}
 uint32_t raw(uint32_t bytes){const auto p=next;next=(next+bytes+31)&~31u;CHECK(next<arena+0x1000000);std::memset(base+p,0,bytes);return p;}
 uint32_t str(std::string_view value){auto p=raw(uint32_t(value.size()+1));std::memcpy(base+p,value.data(),value.size());return p;}
 std::string string(uint32_t p){CHECK(p);return reinterpret_cast<const char*>(base+p);}
 uint32_t thunk(PPCFunc*fn){auto address=REX_CODE_BASE+REX_CODE_SIZE+0x1000+(thunks++)*4;CHECK(thunks<256);REX_LOOKUP_FUNC(base,address)=fn;return address;}
 uint32_t widget(const std::string&name){auto p=raw(0x1000);REX_STORE_U32(p,widget_vtable);REX_STORE_U32(p+448,UINT32_MAX);names[p]=name;name_addresses[p]=str(name);REX_STORE_U32(p+12,name_addresses[p]);return p;}
 void pump(){while(!jobs.empty()){auto f=std::move(jobs.front());jobs.pop_front();f();}}
 bool init(){const auto before=ctx;const bool ok=bridge->Initialize(ctx,base,manager);CHECK(std::memcmp(&before,&ctx,sizeof(ctx))==0);
  if(ok){const auto video=root(FrontendPage::kVideo),effects=root(FrontendPage::kEffects);
   CHECK(names.contains(video)&&names.at(video).starts_with("NativeVideoOptions"));
   CHECK(names.contains(effects)&&names.at(effects).starts_with("NativeEffectsOptions"));}
  return ok;}
 void frame(bool pump_ui=true){
  if(pump_ui)pump();const auto before=ctx;const auto active=REX_LOAD_U32(0x849F5640);bridge->BeforeFrame(ctx,base,active);
  CallGuest(sub_82FEA350,ctx,base,active,arena+0xF000);
  bridge->AfterFrame(ctx,base,active);CHECK(std::memcmp(&before,&ctx,sizeof(ctx))==0);
  if(id()==23&&texts.contains(message)){
   const auto&text=texts.at(message);CHECK(text.size()<=96);
   CHECK(text.find('?')==std::string::npos&&text.find('\n')==std::string::npos);
   CHECK(text.find("NATIVE_FRONTEND_")==std::string::npos&&text.find("!UNREGISTERED")==std::string::npos);
   for(unsigned char c:text)CHECK(c>=32&&c<=126);
  }
 }
 void frames(unsigned n=4){while(n--)frame();}
 void main(){
  REX_STORE_U32(manager+164,17);REX_STORE_U32(manager+168,UINT32_MAX);REX_STORE_U32(manager+172,UINT32_MAX);
  REX_STORE_U32(main_root+448,2);REX_STORE_U32(0x849F5F40,main_child);
  layer_pages={main_root};publish_layer();
 }
 uint32_t id(){return REX_LOAD_U32(REX_LOAD_U32(0x849F5640)+164);}
 void request(uint32_t id){CallGuest(sub_82FD8C30,ctx,base,REX_LOAD_U32(0x849F5640),id);}
 uint32_t root(FrontendPage id){const auto array=REX_LOAD_U32(REX_LOAD_U32(0x849F5640)+140);return REX_LOAD_U32(REX_LOAD_U32(array+uint32_t(id)*4)+20);}
 void event(uint32_t page,uint32_t event){CallGuest(sub_82F0FF38,ctx,base,page,event,0);}
 void input(uint32_t page,uint32_t direction,uint32_t buttons=0){
  const auto packet=raw(160),ref=raw(32),vt=raw(32),node=raw(16),head=raw(4);
  REX_STORE_U32(ref,widget_vtable);REX_STORE_U32(packet,ref);
  REX_STORE_U32(packet+8,buttons);REX_STORE_U32(node+8,REX_LOAD_U32(page+480));REX_STORE_U8(node+12,1);REX_STORE_U32(head,node);
  // The active layer can address a native menu child, rather than the custom
  // page root. Exercise the production routing hook before the original path.
  const auto previous=id();const bool handled=bridge->Input(ctx,base,ui,packet,direction);
  if(previous==23)CHECK(handled); // owned GenericPage must not fall through to a GSC child
  if(!handled)CallGuest(sub_82F0E1B8,ctx,base,ui,packet,direction,head);
 }
 void select_row(size_t row){auto p=FrontendPage(id());const auto page=root(p),target=children.at(page).at(names.at(page)+"Row"+std::to_string(row));
  const auto size=MakeFrontendLayout(p).rows.size();for(size_t n=0;n<=size;++n){if(CallGuest(sub_82EF9560,ctx,base,page)==target)return;input(page,4);}CHECK(false);}
 void select_key(std::string_view key){auto layout=MakeFrontendLayout(FrontendPage(id()));for(size_t i=0;i<layout.rows.size();++i)if(layout.rows[i].key==key){select_row(i);return;}CHECK(false);}
 void action(RowAction action){auto p=FrontendPage(id());auto layout=MakeFrontendLayout(p);for(size_t i=0;i<layout.rows.size();++i)if(layout.rows[i].action==action){select_row(i);input(root(p),0,0x40000000);return;}CHECK(false);}
 void adjust(std::string_view key,int direction){select_key(key);input(root(FrontendPage(id())),direction<0?8:2);}
 void choose(int index){CHECK(id()==23);popup_index=index;event(generic_root,15);}
 void fullscreen_preview(){request(21);frames();CHECK(id()==21);adjust("fullscreen",1);frames();action(RowAction::kApply);frames();CHECK(id()==23&&live.at("fullscreen")=="true"&&saves==0);}
 bool recreate(bool clear_components,bool clear_registry,bool move_manager=false,bool clear_slots=true);
 uint32_t pause(){
  constexpr uint32_t menu=0x849C7370+2*240,descriptor=0x849C7EB8+9*28;
  REX_STORE_U32(manager+164,0);REX_STORE_U32(0x847D2D58,2);std::memset(base+menu,0,240);
  layer_pages.clear();publish_layer();
  REX_STORE_U16(menu+20,9);REX_STORE_U32(descriptor,4);REX_STORE_U32(descriptor+8,0x839D08A0);REX_STORE_U32(descriptor+12,0x839D4020);
  REX_STORE_U32(menu+156,9);CHECK(bridge->DrawPauseEntries(ctx,base,menu,PauseRow));
  CHECK(pause_labels==std::vector<std::string>({"Video","Effects"})&&REX_LOAD_U32(menu+156)==11);
  return menu;
 }
 void callback(uint32_t object,unsigned slot,uint32_t argument=0){const auto target=REX_LOAD_U32(REX_LOAD_U32(object)+slot);auto*fn=REX_LOOKUP_FUNC(base,target);CHECK(fn);CallGuest(fn,ctx,base,object,argument);}
};
void Name(PPCContext&ctx,uint8_t*){CHECK(current->name_addresses.contains(ctx.r3.u32));ctx.r3.u32=current->name_addresses.at(ctx.r3.u32);}
void Visible(PPCContext&ctx,uint8_t*base){REX_STORE_U8(ctx.r3.u32+460,ctx.r4.u32);}
void ForeignEvent(PPCContext&,uint8_t*){++current->foreign_events;}
void Noop(PPCContext&ctx,uint8_t*){ctx.r3.u32=1;}
void OptionsEvent(PPCContext&ctx,uint8_t*base){
 if(ctx.r5.u32==2)current->bridge->OptionsEntering(ctx,base,ctx.r3.u32);
}
void PauseRow(PPCContext&ctx,uint8_t*base){
 current->pause_labels.push_back(current->string(ctx.r4.u32));CHECK(ctx.r5.u32==255);
 REX_STORE_U32(ctx.r3.u32+156,REX_LOAD_U32(ctx.r3.u32+156)+1);
}
void PauseOriginal(PPCContext&ctx,uint8_t*base){CHECK(REX_LOAD_U32(ctx.r3.u32+64)==9);++current->legacy_updates;}
void CountPauseDraw(PPCContext&ctx,uint8_t*base){
 const auto menu=ctx.r3.u32;
 for(unsigned i=0;i<9;++i)CallGuest(sub_839C4D08,ctx,base,menu,current->str("original setting"),255);
 CHECK(current->bridge->DrawPauseEntries(ctx,base,menu,sub_839C4D08));
 CallGuest(sub_839C4D08,ctx,base,menu,current->str("Back"),255);
}
void sub_82EF3180(PPCContext&ctx,uint8_t*){current->pause_draw_y.push_back(ctx.f2.f64);current->pause_draw_width.push_back(ctx.f5.f64);}
void ProbePauseDraw(PPCContext&ctx,uint8_t*base){
 const auto menu=ctx.r3.u32;REX_STORE_U32(menu+156,0);
 for(unsigned i=0;i<12;++i){REX_STORE_U32(0x847D2D98,std::bit_cast<uint32_t>(1.0f));
  CallGuest(sub_82EF3D80,ctx,base,menu,current->str("setting"),255);}
}
namespace rex::runtime {PPCFunc* ResolveIndirectFunction(uint32_t target){std::fprintf(stderr,"unregistered fixture dispatch %08X\n",target);CHECK(false);return nullptr;}}
void LoadLayout(PPCContext&ctx,uint8_t*base){
 auto&f=*current;CHECK(!f.scene_loading);CHECK(ctx.r3.u32==0&&ctx.r4.u32==0);++f.loads;if(f.deny_layout)return;
 const auto xml=f.string(ctx.r5.u32);std::smatch match;CHECK(std::regex_search(xml,match,std::regex("<page name=\"([^\"]+)\"")));
 const auto name=match[1].str();auto root=f.widget(name);f.roots[name]=root;
 REX_STORE_U32(root+32,REX_LOAD_U32(registry+24));REX_STORE_U32(registry+24,root);
 auto regex=std::regex("<text name=\"([^\"]+)\"");
 for(auto i=std::sregex_iterator(xml.begin(),xml.end(),regex);i!=std::sregex_iterator();++i){auto child=f.widget((*i)[1].str());f.children[root][(*i)[1].str()]=child;}
 CHECK(std::regex_search(xml,match,std::regex("focus=\"([^\"]+)\"")));
 REX_STORE_U32(root+464,f.str(match[1].str()));REX_STORE_U16(root+470,match[1].str().size());
 if(std::regex_search(xml,match,std::regex("<menu name=\"([^\"]+)\""))){
  const auto node=f.raw(32);f.xml_names[node]=f.str(match[1].str());
  auto menu=CallGuest(sub_82F0DF30,ctx,base,node,root); // actual TU23 special-case loader
  CHECK(!menu); // custom name must not use the MainMenuPage-only branch
  menu=f.bridge->CompleteMenu(ctx,base,node,root);CHECK(menu&&REX_LOAD_U8(root+496)==1);
 }
 const auto nav=f.raw(32),begin=f.raw(12),end=f.raw(12);REX_STORE_U32(nav+16,begin);REX_STORE_U32(nav+20,end);uint32_t last=begin;
 const std::regex routes("<navigate from=\"([^\"]+)\" direction=\"([^\"]+)\" to=\"([^\"]+)\"");
 for(auto it=std::sregex_iterator(xml.begin(),xml.end(),routes);it!=std::sregex_iterator();++it){const auto n=f.raw(12),route=f.raw(36);REX_STORE_U32(last+4,n);REX_STORE_U32(n+8,route);
  REX_STORE_U32(route,f.str((*it)[1].str()));REX_STORE_U32(route+12,f.str((*it)[3].str()));REX_STORE_U32(route+24,(*it)[2].str()=="up"?9:10);last=n;}
 REX_STORE_U32(last+4,end);REX_STORE_U32(root+488,nav);
}
void FindWidget(PPCContext&ctx,uint8_t*){
 auto&f=*current;const auto root=ctx.r3.u32;const auto name=f.string(ctx.r4.u32);
 if(f.missing_widget&&name.find("Row0Value")!=std::string::npos){ctx.r3.u32=0;return;}
 const auto it=f.children.find(root);ctx.r3.u32=it!=f.children.end()&&it->second.contains(name)?it->second.at(name):0;
}
void CloneMenu(PPCContext&ctx,uint8_t*base){auto&f=*current;CHECK(ctx.r3.u32==REX_LOAD_U32(REX_LOAD_U32(0x849E2208)+4));
 const auto name=f.string(f.xml_names.at(ctx.r4.u32));const auto root=ctx.r5.u32;const auto menu=f.widget(name);
 REX_STORE_U32(menu+436,2);f.children[root][name]=menu;ctx.r3.u32=menu;
}
void sub_82CE3F90(PPCContext&ctx,uint8_t*){auto&f=*current;ctx.r3.u32=f.string(ctx.r4.u32)=="name"?f.xml_names.at(ctx.r3.u32):0;}
std::string LocalizedText(uint32_t key){auto&f=*current;const auto value=f.bridge?f.bridge->LookupLiteral(f.base,key):0;
 if(value)return f.string(value);const auto name=f.string(key);return name.empty()||name=="Yes"||name=="No"||name=="Continue"?name:"!UNREGISTERED_TRANSLATION!";}
void __imp__sub_82EFEFA0(PPCContext&,uint8_t*);
#include "native-adapters.inc"
void Refresh(PPCContext&ctx,uint8_t*base){++current->refreshes;RefreshOptionsVisibility(ctx,base);}
void sub_82F02C38(PPCContext&ctx,uint8_t*base){current->dynamic_clones+=REX_LOAD_U32(ctx.r3.u32+64);}
void sub_839FFC60(PPCContext&ctx,uint8_t*){ctx.r3.u32=0;}
void sub_82F0CF40(PPCContext&ctx,uint8_t*base){FindWidget(ctx,base);}
void sub_82F0D040(PPCContext&ctx,uint8_t*base){auto&f=*current;ctx.r3.u32=f.children[ctx.r3.u32].contains(f.names[ctx.r3.u32]+"Menu")?f.children[ctx.r3.u32][f.names[ctx.r3.u32]+"Menu"]:0;}
void sub_82EF9D08(PPCContext&,uint8_t*){} // text animation is a rendering boundary
void sub_82795028(PPCContext&ctx,uint8_t*base){REX_STORE_U32(ctx.r3.u32,REX_LOAD_U32(ctx.r4.u32));}
void sub_8301FAD8(PPCContext&ctx,uint8_t*){ctx.r3.u32=0;}
void Configure(PPCContext&ctx,uint8_t*base){++current->configs;sub_82FBDFF8(ctx,base);}
void Publish(PPCContext&ctx,uint8_t*base){++current->publishes;sub_82FD0508(ctx,base);}
void sub_82A296D8(PPCContext&ctx,uint8_t*base){ctx.r3.s64=std::strcmp(reinterpret_cast<char*>(base+ctx.r3.u32),reinterpret_cast<char*>(base+ctx.r4.u32));}
void sub_82A2F6C8(PPCContext&ctx,uint8_t*base){sub_82A296D8(ctx,base);}
void __imp__sub_82EFEFA0(PPCContext&ctx,uint8_t*base){const auto widget=ctx.r3.u32,child=REX_LOAD_U32(widget+536);++current->plain_calls;current->last_plain_arg=ctx.r4.u32;NativePlainText(ctx,base);current->texts[widget]=current->string(REX_LOAD_U32(widget+480));if(child)current->texts[child]=current->string(REX_LOAD_U32(child+488));}
void sub_82EFEFA0(PPCContext&ctx,uint8_t*base){SetOwnedWidgetText(ctx,base);}
void sub_82EFE8F0(PPCContext&ctx,uint8_t*base){++current->translated_calls;const auto widget=ctx.r3.u32;NativeTranslatedText(ctx,base);current->texts[widget]=current->string(REX_LOAD_U32(widget+488));}
void sub_82A435C8(PPCContext&ctx,uint8_t*base){auto&f=*current;const auto key=ctx.r3.u32;
 if(const auto literal=f.bridge?f.bridge->LookupLiteral(base,key):0){ctx.r3.u32=literal;return;}
 const auto text=f.string(key);if(text.empty()||text=="Yes"||text=="No"||text=="Continue"){ctx.r3.u32=key;return;}
 NativeOriginalLocalize(ctx,base);
}
void sub_8268A290(PPCContext&ctx,uint8_t*base){const auto property=ctx.r4.u32,bytes=ctx.r5.u32;REX_STORE_U32(property,current->raw(bytes));ctx.r3.u32=bytes;}
void sub_83E45090(PPCContext&ctx,uint8_t*base){std::memcpy(base+ctx.r3.u32,base+ctx.r4.u32,ctx.r5.u32);}
void sub_8278F7E0(PPCContext&ctx,uint8_t*base){REX_STORE_U32(ctx.r3.u32,current->str(""));}
void sub_82EFE788(PPCContext&ctx,uint8_t*){CHECK(ctx.r4.u32==0);ctx.r3.u32+=0x100;}
void sub_82F0DBA0(PPCContext&ctx,uint8_t*){CHECK(ctx.r3.u32==current->menu&&ctx.r4.u32==0);ctx.r3.s64=current->popup_index;}
void sub_82F0D228(PPCContext&,uint8_t*){}
void sub_82F0FE08(PPCContext&,uint8_t*){}
void sub_82F0D508(PPCContext&,uint8_t*){}
void sub_82EFCB70(PPCContext&ctx,uint8_t*){CHECK(ctx.r3.u32==ui&&ctx.r4.u32<4);current->sounds.push_back(ctx.r4.u32);}
void sub_82FBF270(PPCContext&,uint8_t*){}
void sub_82A36908(PPCContext&ctx,uint8_t*){ctx.r3.u32=session;}
void sub_82A465F0(PPCContext&ctx,uint8_t*){CHECK(ctx.r3.u32==session+0x100);ctx.r3.u32=1;}
void sub_82FE35C0(PPCContext&ctx,uint8_t*base){CHECK(ctx.r3.u32==REX_LOAD_U32(0x849F5640));ctx.r3.u32=0;}
void sub_82FD8F70(PPCContext&ctx,uint8_t*base){CHECK(ctx.r3.u32==REX_LOAD_U32(0x849F5640));}
void sub_82FBE2A8(PPCContext&,uint8_t*){}
void sub_82FE7B90(PPCContext&,uint8_t*){}
void sub_82A3E238(PPCContext&,uint8_t*){}
void sub_82A3E2B8(PPCContext&,uint8_t*){}
void sub_82A4C508(PPCContext&,uint8_t*){}
void sub_82A3C8B0(PPCContext&,uint8_t*){}
void sub_82A2C868(PPCContext&,uint8_t*){}
void sub_82A58570(PPCContext&ctx,uint8_t*base){
 // Event-bus delivery/audio is a fixture boundary. Record the real translated
 // SelectWidget event separately from explicit bridge sound requests.
 if(ctx.r5.u32&&REX_LOAD_U32(ctx.r5.u32)==0x823B6FC4&&
    REX_LOAD_U32(ctx.r5.u32+16)&&REX_LOAD_U32(ctx.r5.u32+12)!=REX_LOAD_U32(ctx.r5.u32+16))
  ++current->focus_events;
}
void sub_82EFB300(PPCContext&ctx,uint8_t*base){CHECK(ctx.r3.u32==ui+0x400);current->layer_pages.push_back(ctx.r4.u32);current->publish_layer();}
void sub_82EFCDA0(PPCContext&ctx,uint8_t*base){CHECK(ctx.r3.u32==layers+24);
 auto&v=current->layer_pages;auto it=std::find(v.begin(),v.end(),ctx.r4.u32);CHECK(it!=v.end());v.erase(it);current->publish_layer();}
#include "native-unexpected.inc"
#include "native-translated.inc"

Fixture::Fixture(uint8_t*memory,const std::vector<char>&image,bool asynchronous):base(memory){
 current=this;std::memcpy(base+0x82000000,image.data(),image.size());std::memset(base+arena,0,0x1000000);
 ctx.r1.u32=arena+0xF0000;ctx.r31.u64=0x12345678ABCDEF01ull;ctx.lr=0xDEADBEEF;
 widget_vtable=raw(512);REX_STORE_U32(widget_vtable+56,thunk(Name));REX_STORE_U32(widget_vtable+196,thunk(Visible));
 REX_STORE_U32(widget_vtable+20,thunk(Noop));
 REX_STORE_U32(widget_vtable+188,thunk(Noop));
 REX_STORE_U32(widget_vtable+336,thunk(Noop));
 REX_STORE_U32(widget_vtable+332,0x82F0FF38);
 option_root=widget("OptionsPage");generic_root=widget("GenericPage");
 message=widget("Generic_Message");menu=widget("GenericMenu");yes=widget("Generic_Yes");no=widget("Generic_No");
 // The real GenericMenu child does not own the page's selection pointer.
 // V25 treated its +484 word (1 in the owner's dump) as a widget address.
 REX_STORE_U32(menu+484,1);
 video_row=widget("VideoRow");effects_row=widget("EffectsRow");
 controls_row=widget("ControlsRow");dlc_row=widget("DlcRow");
 REX_STORE_U32(0x849E21CC,ui);REX_STORE_U32(0x849E220C,registry);REX_STORE_U32(0x849F562C,generic);
 const auto menu_factory=raw(16);REX_STORE_U32(0x849E2208,menu_factory);REX_STORE_U32(menu_factory+4,raw(16));
 REX_STORE_U32(0x849F5640,manager);REX_STORE_U32(0x849F566C,footer);REX_STORE_U32(0x849F5F34,state);
 REX_STORE_U32(ui+84,layers);REX_STORE_U32(ui+96,ui+0x400);REX_STORE_U32(ui+104,components);
 REX_STORE_U32(manager+140,pages);REX_STORE_U32(manager+148,61);REX_STORE_U32(manager+152,interfaces);REX_STORE_U32(manager+160,61);
 REX_STORE_U32(manager+164,19);REX_STORE_U32(manager+168,UINT32_MAX);REX_STORE_U32(manager+172,UINT32_MAX);REX_STORE_U8(manager+188,1);
 REX_STORE_U32(session,session+0x100);REX_STORE_U32(layers+32,1);REX_STORE_U32(option_root+448,2);
 layer_pages={option_root};publish_layer();
 CallGuest(sub_82EFB478,ctx,base,options,str("OptionsPage"));auto option_vt=raw(20);REX_STORE_U32(option_vt+4,thunk(OptionsEvent));REX_STORE_U32(options,option_vt);
 REX_STORE_U32(options+20,option_root);REX_STORE_U32(option_root+476,options);REX_STORE_U32(pages+19*4,options);
 // Observed V34/TU23 layout: MainMenuPage root != global MainMenu child.
 // Both base+20 and main-controller+40 hold the full page root.
 main_root=widget("MainMenuPage");main_child=widget("MainMenu");
 children[main_root]["MainMenu"]=main_child;REX_STORE_U32(main_child+436,2);REX_STORE_U32(main_child+484,1);
 REX_STORE_U32(main_root+476,main_component);REX_STORE_U32(main_root+448,2);
 CallGuest(sub_82EFB478,ctx,base,main_component,str("MainMenuPage"));
 auto main_vt=raw(20);REX_STORE_U32(main_vt+4,thunk(Noop));REX_STORE_U32(main_component,main_vt);
 REX_STORE_U32(main_component+20,main_root);REX_STORE_U32(main_component+40,main_root);
 REX_STORE_U32(pages+17*4,main_component);REX_STORE_U32(0x849F5F40,main_child);
 REX_STORE_U32(options+108,video_row);REX_STORE_U32(options+112,effects_row);REX_STORE_U32(options+152,options+0x200);
 REX_STORE_U32(options+0x200,option_root);
 REX_STORE_U32(options+116,controls_row);REX_STORE_U32(options+136,dlc_row);
 const auto option_entries=raw(48);REX_STORE_U32(options+0x200+28,option_entries);REX_STORE_U32(options+0x200+32,12);
 REX_STORE_U32(option_entries+12,video_row);REX_STORE_U32(option_entries+16,effects_row);REX_STORE_U32(option_entries+20,controls_row);REX_STORE_U32(option_entries+44,dlc_row);
 CallGuest(sub_82FB7428,ctx,base,generic);CHECK(REX_LOAD_U32(REX_LOAD_U32(generic)+4)==0x82FD9270);
 REX_STORE_U32(generic+20,generic_root);REX_STORE_U32(generic_root+476,generic);REX_STORE_U32(generic+28,generic_root);
 REX_STORE_U32(generic+32,message);REX_STORE_U32(generic+36,menu);REX_STORE_U32(generic+40,yes);REX_STORE_U32(generic+44,no);
 CallGuest(sub_82EFB1E8,ctx,base,ui,generic);
 const auto empty=str("");REX_STORE_U32(0x847BCF2C,empty);REX_STORE_U32(0x849C7D70,str("Yes"));REX_STORE_U32(0x849C7D74,str("No"));REX_STORE_U32(0x849F5CC0,str("Continue"));
 REX_STORE_U32(footer+40,widget("FooterText"));REX_STORE_U32(footer+44,empty);REX_STORE_U32(message+476,empty);
 auto foreign_vt=raw(28);for(unsigned i=0;i<7;++i)REX_STORE_U32(foreign_vt+i*4,thunk(ForeignEvent));REX_STORE_U32(foreign_interface,foreign_vt);
 REX_STORE_U32(pages+30*4,generic);REX_STORE_U32(interfaces+30*4,foreign_interface);
 Backend backend{[this]{return live;},[](auto k,auto v){return NormalizeNativeSetting(k,v);},
  [this](auto k,auto v){++writes;if(restore_failure&&((k=="fullscreen"&&v=="false")||(k=="native_fsr3_quality"&&v=="quality")))return false;live[std::string(k)]=v;return true;},
  [this](const Values&v,std::string&e){if(save_failure){e="Disk unavailable";return false;}++saves;saved=v;return true;}};
 if(asynchronous){
  backend.submit=[this,write=backend.write](const Values&changes){++revision;
   for(const auto&[key,value]:changes)if(!write(key,value))return RevisionResult{revision,RevisionState::kFailed,"Resource apply failed"};
   return RevisionResult{revision,RevisionState::kPending,{}};};
  backend.observe=[this](uint64_t id){if(stale_revision)return RevisionResult{stale_revision,RevisionState::kComplete,{}};
   return RevisionResult{id,id==completed_revision?RevisionState::kComplete:RevisionState::kPending,{}};};
 }
 service=std::make_shared<QueuedSettings>(backend,[this](auto f){jobs.push_back(std::move(f));return true;},NativeSettingsDefaults(),NativeSettingsConfirmationKeys(),[this]{return time;});
 NativeBridgeApi api;api.allocate=[this](uint32_t size){if(++allocations==fail_allocation)return 0u;return raw(size);};
 api.thunk=[this](PPCFunc*fn){if(++thunk_calls==fail_thunk)return 0u;return thunk(fn);};
 api.settings=[this]{return missing_service?std::shared_ptr<QueuedSettings>{}:service;};api.now=[this]{return time;};
 api.quit=[this]{++quit_requests;};
 api.construct_menu=CloneMenu;
 api.select_widget=sub_82EFEEB8;
 api.play_sound=sub_82EFCB70;
 api.read_metadata=[this](uint32_t p,void*data,uint32_t bytes){
  if(!p||!bytes||uint64_t(p)+bytes>0x100000000ull||
     (unreadable_root&&p<uint64_t(unreadable_root)+480&&uint64_t(p)+bytes>unreadable_root))return false;
  std::memcpy(data,base+p,bytes);return true;
 };
 api.load_layout=LoadLayout;api.find_page=sub_82EF9730;api.find_widget=FindWidget;api.construct_component=sub_82EFB478;
 api.add_component=sub_82EFB1E8;api.find_component=sub_82EF8D80;api.set_text=sub_82EFEFA0;api.page_event=sub_82F0FF38;
 api.selected_widget=sub_82EF9560;api.configure_popup=Configure;api.reset_popup=sub_82FBDF58;api.publish_popup=Publish;api.request_page=sub_82FD8C30;
 api.set_visible=sub_82EF51B0;api.refresh_list=Refresh;bridge=std::make_unique<NativeSettingsBridge>(std::move(api));literal_bridge.store(bridge.get());
}
bool Fixture::recreate(bool clear_components,bool clear_registry,bool move_manager,bool clear_slots){
 auto active=REX_LOAD_U32(0x849F5640);
 if(move_manager){const auto old=active;active=raw(256);std::memcpy(base+active,base+old,204);
  const auto new_pages=raw(256),new_interfaces=raw(256);REX_STORE_U32(active+140,new_pages);REX_STORE_U32(active+148,61);
  REX_STORE_U32(active+152,new_interfaces);REX_STORE_U32(active+160,61);REX_STORE_U32(new_pages+19*4,options);
  REX_STORE_U32(new_pages+30*4,generic);REX_STORE_U32(new_interfaces+30*4,foreign_interface);REX_STORE_U32(0x849F5640,active);
 }
 if(clear_slots){for(auto id:{21u,22u,23u}){REX_STORE_U32(REX_LOAD_U32(active+140)+id*4,0);REX_STORE_U32(REX_LOAD_U32(active+152)+id*4,0);}}
 REX_STORE_U32(active+164,19);REX_STORE_U32(active+168,UINT32_MAX);REX_STORE_U32(active+172,UINT32_MAX);REX_STORE_U32(active+184,0);
 REX_STORE_U32(layers+32,1);REX_STORE_U32(option_root+448,2);REX_STORE_U32(generic_root+448,UINT32_MAX);
 layer_pages={option_root};publish_layer();
 REX_STORE_U8(video_row+457,0);REX_STORE_U8(effects_row+457,0);
 if(clear_components){std::memset(base+components,0,12);CallGuest(sub_82EFB1E8,ctx,base,ui,generic);}
 if(clear_registry){REX_STORE_U32(registry+24,0);roots.clear();
  generic_root=widget("GenericPage");message=widget("Generic_Message");menu=widget("GenericMenu");yes=widget("Generic_Yes");no=widget("Generic_No");
  REX_STORE_U32(menu+484,1);
  REX_STORE_U32(generic+20,generic_root);REX_STORE_U32(generic_root+476,generic);REX_STORE_U32(generic+28,generic_root);
  REX_STORE_U32(generic+32,message);REX_STORE_U32(generic+36,menu);REX_STORE_U32(generic+40,yes);REX_STORE_U32(generic+44,no);
  REX_STORE_U32(message+476,str(""));
 }
 REX_STORE_U32(generic+176,foreign_interface);
 return bridge->Initialize(ctx,base,active,true);
}
int main(int argc,char**argv){
 if(argc>1&&std::strcmp(argv[1],"--crash-report")==0)return 0;
 SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);CHECK(argc==2);
 AddVectoredExceptionHandler(1,[](EXCEPTION_POINTERS*e)->LONG{
  if(e->ExceptionRecord->ExceptionCode==EXCEPTION_ACCESS_VIOLATION){
   std::fprintf(stderr,"fixture AV: host=%p guest-lr=%llX r3=%X r4=%X fault=%p\n",e->ExceptionRecord->ExceptionAddress,
    current?current->ctx.lr:0,current?current->ctx.r3.u32:0,current?current->ctx.r4.u32:0,(void*)e->ExceptionRecord->ExceptionInformation[1]);
  }
  return EXCEPTION_CONTINUE_SEARCH;
 });
 auto*base=static_cast<uint8_t*>(VirtualAlloc(nullptr,0x100000000ull,MEM_RESERVE,PAGE_NOACCESS));CHECK(base);
 CHECK(VirtualAlloc(base+0x82000000,0x2B80000,MEM_COMMIT,PAGE_READWRITE));CHECK(VirtualAlloc(base+arena,0x1000000,MEM_COMMIT,PAGE_READWRITE));
 CHECK(VirtualAlloc(base+REX_IMAGE_BASE+REX_IMAGE_SIZE,(uint64_t(REX_CODE_SIZE)+REX_THUNK_RESERVE_SIZE)*2,MEM_COMMIT,PAGE_READWRITE));
 std::ifstream file(argv[1],std::ios::binary);std::vector<char> image(45613056);file.read(image.data(),image.size());CHECK(file.gcount()==image.size());
 REX_LOOKUP_FUNC(base,0x82F0FF38)=sub_82F0FF38;REX_LOOKUP_FUNC(base,0x82FD9270)=sub_82FD9270;
 unsigned count=0;
 {Fixture f(base,image);f.live["native_fsr3"]="false";CHECK(f.init());
  f.request(22);f.frames();CHECK(f.id()==22);
  const auto page=f.root(FrontendPage::kEffects);const auto layout=MakeFrontendLayout(FrontendPage::kEffects);
  for(const auto& row:layout.rows)for(const auto& name:{row.element,row.value_element}){
   const auto widget=f.children.at(page).at(name);REX_STORE_U32(widget+436,6);
   REX_STORE_U32(widget+432,std::bit_cast<uint32_t>(0.37f));}
  const auto widget=[&](std::string_view key,bool value=false){for(const auto& row:layout.rows)if(row.key==key)
   return f.children.at(page).at(value?row.value_element:row.element);CHECK(false);return 0u;};
  const auto channel=[&](uint32_t w,unsigned offset){return std::bit_cast<float>(REX_LOAD_U32(w+offset));};
  // Reference colour conversion executes the title's original non-text
  // selection path, proving the same RGB slots used by its text renderer.
  const auto reference=f.widget("ColourReference");REX_STORE_U32(0x820D9C78,0xFF00FFFF);
  CallGuest(sub_82EFBF70,f.ctx,base,0,reference,1,0);
  CHECK(channel(reference,420)==1&&channel(reference,424)==1&&channel(reference,428)==0&&channel(reference,432)==1);
  f.frames();const auto toggle=widget("native_fsr3"),quality=widget("native_fsr3_quality");
  CHECK(channel(toggle,420)==channel(reference,420)&&channel(toggle,424)==channel(reference,424)&&channel(toggle,428)==channel(reference,428));
  CHECK(channel(quality,420)==0.32f&&channel(widget("native_fsr3_quality",true),428)==0.32f);
  f.time+=std::chrono::milliseconds(500);f.frames();CHECK(channel(toggle,420)==0.72f&&channel(toggle,428)==0);
  CHECK(channel(toggle,432)==0.37f&&channel(quality,432)==0.37f);
  f.adjust("native_fsr3_quality",1);f.frames();CHECK(f.live.at("native_fsr3_quality")=="quality"&&f.writes==0&&f.id()==22);
  CHECK(channel(quality,420)==0.50f&&channel(quality,428)==0.50f); // focused but disabled
  f.adjust("native_fsr3",1);f.frames();CHECK(channel(quality,420)==1&&channel(quality,428)==1);
  f.adjust("native_fsr3_quality",1);f.frames();CHECK(channel(quality,420)==0.72f&&channel(quality,428)==0);
  CHECK(channel(widget("native_fsr3_quality",true),428)==0&&channel(quality,432)==0.37f&&f.writes==0);
  f.unreadable_root=quality;REX_STORE_U32(quality+420,std::bit_cast<uint32_t>(0.123f));f.frames();
  CHECK(channel(quality,420)==0.123f);f.unreadable_root=0;++count;
 }
 {Fixture f(base,image);CHECK(f.init());const auto menu=f.pause();
  REX_STORE_U16(menu+10,99);REX_STORE_U16(menu+18,11);REX_STORE_U16(menu+14,0);
  REX_STORE_U32(0x847D2D78,std::bit_cast<uint32_t>(0.5f));REX_STORE_U32(0x847D2D80,std::bit_cast<uint32_t>(-0.10664f));
  REX_STORE_U32(menu+168,std::bit_cast<uint32_t>(0.6f));
  CHECK(f.bridge->RenderPauseOptions(f.ctx,base,menu,ProbePauseDraw));
  CHECK(f.pause_draw_y.size()==12&&f.pause_draw_width.size()==12&&REX_LOAD_U32(menu+156)==12);
  for(unsigned i=0;i<12;++i){CHECK(std::abs(f.pause_draw_y[i]-(0.4-i*0.10664*0.70))<0.00001);
   CHECK(std::abs(f.pause_draw_width[i]-0.35)<0.00001);}
  CHECK(REX_LOAD_U32(menu+168)==std::bit_cast<uint32_t>(0.6f)&&REX_LOAD_U32(menu+224)==0&&REX_LOAD_U32(menu+228)==0);
  REX_STORE_U32(0x847D2D58,0);CHECK(!f.bridge->RenderPauseOptions(f.ctx,base,menu,ProbePauseDraw));++count;
 }
 {Fixture f(base,image);CHECK(f.init());
  for(auto id:{FrontendPage::kVideo,FrontendPage::kEffects}){
   const auto layout=MakeFrontendLayout(id);const auto root=f.root(id);
   for(const auto&row:layout.rows){const auto widget=f.children.at(root).at(row.element);
    CHECK(f.string(REX_LOAD_U32(widget+480))==row.label&&f.texts.at(widget)==row.label);}
  }
  const auto child=f.widget("NestedText");REX_STORE_U32(f.video_row+536,child);
  const auto plain_before=f.plain_calls,translated_before=f.translated_calls;
  f.bridge->RevealOptions(f.ctx,base,options);
  CHECK(f.plain_calls==plain_before+2&&f.translated_calls==translated_before);
  uint32_t canonical=0;const auto alias=f.last_plain_arg;
  CHECK(f.bridge->LookupLiteral(base,alias,&canonical)==alias&&canonical!=0&&canonical!=alias);
  CHECK(f.bridge->LookupLiteral(base,f.str(f.string(alias)))==0);
  CHECK(f.string(REX_LOAD_U32(f.video_row+480))=="Video");
  CHECK(f.string(REX_LOAD_U32(child+488))=="Video"&&f.texts.at(child)=="Video");
  CallGuest(sub_82EFEFA0,f.ctx,base,f.effects_row,f.str("Ordinary native text"));
  CHECK(f.string(REX_LOAD_U32(f.effects_row+480))=="Ordinary native text");++count;
 }
 for(bool depart:{false,true}){
  Fixture f(base,image);CHECK(f.init());f.request(21);f.frames();
  // A scene return may arrive while cancellation is still queued on the UI
  // thread. The main-menu click must survive that asynchronous cancellation.
  f.main();
  CHECK(!f.bridge->Initialize(f.ctx,base,manager,true));
  CHECK(f.bridge->ShowMenuExtra(f.ctx,base,false));f.frame(false);CHECK(f.id()==17);
  if(depart){REX_STORE_U32(manager+164,0);REX_STORE_U32(f.main_root+448,UINT32_MAX);}
  f.frames(8);
  if(depart)CHECK(f.id()==0&&f.quit_requests==0);
  else CHECK(f.id()==23&&f.texts.at(f.message)=="Thanks for playing Dimensions Recompiled! Made by NeverCookFirst");
  ++count;
 }
 {Fixture f(base,image);CHECK(f.init()&&f.bridge->ready(base,manager)&&f.loads==2&&f.refreshes==1);
  CHECK(REX_LOAD_U8(f.video_row+457)==1&&REX_LOAD_U8(f.effects_row+457)==1);
  CHECK(REX_LOAD_U32(pages+21*4)!=REX_LOAD_U32(pages+22*4)&&REX_LOAD_U32(pages+23*4)==generic);++count;
  f.request(21);f.frames();CHECK(f.id()==21);f.adjust("native_fps_limit",1);CHECK(f.writes==0);f.frames();CHECK(f.live.at("native_fps_limit")=="240");
  f.action(RowAction::kApply);f.frames();CHECK(f.id()==23&&f.live.at("native_fps_limit")=="0"&&f.saves==1);
  CHECK(f.texts.at(f.message)=="Settings saved"&&f.texts.at(f.yes)=="Continue");f.choose(0);f.frames();CHECK(f.id()==21);
  f.event(f.root(FrontendPage::kVideo),16);f.frames();CHECK(f.id()==19);++count;}
 {Fixture f(base,image);CHECK(f.init());f.fullscreen_preview();CHECK(f.texts.at(f.message).find("15 seconds")!=std::string::npos);
  CHECK(f.texts.at(f.yes)=="Keep"&&f.texts.at(f.no)=="Revert");f.choose(1);f.frames();
  CHECK(f.id()==23&&f.live.at("fullscreen")=="false"&&f.saves==0&&f.texts.at(f.message)=="Changes restored");
  f.choose(0);f.frames();CHECK(f.id()==21);++count;}
 {Fixture f(base,image);CHECK(f.init());f.fullscreen_preview();f.choose(0);f.frames();CHECK(f.saves==1&&f.saved.at("fullscreen")=="true");++count;}
 {Fixture f(base,image);CHECK(f.init());f.fullscreen_preview();f.time+=std::chrono::seconds(1);f.frames();CHECK(f.texts.at(f.message).find("14 seconds")!=std::string::npos);
  f.time+=std::chrono::seconds(14);f.service->AdvanceFromUI();f.frames();CHECK(f.live.at("fullscreen")=="false"&&f.saves==0&&f.texts.at(f.message)=="Changes restored");++count;}
 {Fixture f(base,image);CHECK(f.init());f.request(22);f.frames();CHECK(f.id()==22);f.adjust("native_fsr3_quality",1);f.frames();f.action(RowAction::kApply);f.frames();
  CHECK(f.id()==23&&f.live.at("native_fsr3_quality")=="balanced");f.event(f.generic_root,16);f.frames();CHECK(f.live.at("native_fsr3_quality")=="quality"&&f.saves==0);++count;}
 {Fixture f(base,image);CHECK(f.init());f.request(21);f.frames();f.adjust("native_fps_limit",1);f.frames();f.event(f.root(FrontendPage::kVideo),16);f.frames();
  CHECK(f.id()==23&&f.texts.at(f.message)=="Discard unsaved changes");f.choose(0);f.frames();CHECK(f.id()==19&&f.writes==0);++count;}
 {Fixture f(base,image);CHECK(f.init());f.service->SetStartupErrorFromUI("Old preferences could not be loaded");f.request(21);f.frames();
  CHECK(f.id()==23&&f.texts.at(f.message)=="Saved settings could not be loaded");f.choose(0);f.frames();CHECK(f.id()==21&&f.saves==0);++count;}
 {Fixture f(base,image);CHECK(f.init());f.request(21);f.frames();f.adjust("native_fps_limit",1);f.frames();f.save_failure=true;f.action(RowAction::kApply);f.frames();
  CHECK(f.id()==23&&f.texts.at(f.yes)=="Retry save"&&f.texts.at(f.no)=="Revert"&&f.saves==0);f.choose(1);f.frames();CHECK(f.live.at("native_fps_limit")=="240");++count;}
 {Fixture f(base,image);CHECK(f.init());f.fullscreen_preview();f.request(30);f.frames();
  CHECK(f.id()==30&&f.foreign_events>0&&f.live.at("fullscreen")=="false"&&f.saves==0);++count;}
 {Fixture f(base,image);CHECK(f.init());f.fullscreen_preview();REX_STORE_U32(0x849F5640,manager+0x100);
  f.popup_index=0;f.event(f.generic_root,15);for(unsigned i=0;i<5;++i){f.bridge->BeforeFrame(f.ctx,base,manager+0x100);f.pump();}
  CHECK(!f.bridge->ready(base,manager)&&f.saves==0&&f.live.at("fullscreen")=="false");++count;}
 {Fixture f(base,image);CHECK(f.init());const auto old=f.root(FrontendPage::kVideo),thunks=f.thunk_calls;
  for(unsigned i=0;i<5;++i){const auto allocated=f.allocations;CHECK(f.recreate(false,false));CHECK(f.bridge->ready(base,manager)&&f.root(FrontendPage::kVideo)==old&&f.loads==2&&f.thunk_calls==thunks);
   CHECK(f.allocations-allocated==3);}
  f.request(21);f.frames();f.adjust("native_fps_limit",1);f.frames();f.action(RowAction::kApply);f.frames();CHECK(f.saves==1);++count;}
 {Fixture f(base,image);CHECK(f.init());const auto old=f.root(FrontendPage::kVideo),old_component=REX_LOAD_U32(pages+84);
  CHECK(f.recreate(true,false));CHECK(f.root(FrontendPage::kVideo)==old&&REX_LOAD_U32(pages+84)!=old_component&&REX_LOAD_U32(old_component+20)==0&&f.loads==2);
  f.fullscreen_preview();f.choose(1);f.frames();CHECK(f.live.at("fullscreen")=="false");++count;}
 {Fixture f(base,image);CHECK(f.init());const auto old=f.root(FrontendPage::kVideo);CHECK(f.recreate(true,true));
  CHECK(f.root(FrontendPage::kVideo)!=old&&f.loads==4&&f.names.at(f.root(FrontendPage::kVideo)).ends_with("_g2"));
  f.fullscreen_preview();f.choose(0);f.frames();CHECK(f.saves==1);++count;}
 {Fixture f(base,image);CHECK(f.init());CHECK(f.recreate(false,false,true));const auto active=REX_LOAD_U32(0x849F5640);
  CHECK(active!=manager&&f.bridge->ready(base,active)&&f.loads==2);f.fullscreen_preview();f.choose(1);f.frames();CHECK(f.live.at("fullscreen")=="false");++count;}
 {Fixture f(base,image);CHECK(f.init());f.fullscreen_preview();CHECK(!f.recreate(false,false));
  CHECK(REX_LOAD_U8(f.video_row+457)==0&&f.live.at("fullscreen")=="true");f.frames();
  CHECK(f.bridge->ready(base,manager)&&f.live.at("fullscreen")=="false"&&f.saves==0&&REX_LOAD_U8(f.video_row+457)==1);
  f.fullscreen_preview();f.choose(1);f.frames();++count;}
 {Fixture f(base,image);CHECK(f.init());f.fullscreen_preview();const auto old_interface=REX_LOAD_U32(interfaces+92),old_root=f.generic_root;
  CHECK(!f.recreate(true,true));f.frames();f.fullscreen_preview();const auto live_interface=REX_LOAD_U32(interfaces+92);
  const auto dispatch=REX_LOOKUP_FUNC(base,REX_LOAD_U32(REX_LOAD_U32(live_interface)+8));CHECK(dispatch);
  CallGuest(dispatch,f.ctx,base,live_interface,f.generic_root,15,0);
  f.callback(old_interface,16,0);f.popup_index=0;f.event(old_root,15);f.frames();CHECK(f.saves==0&&f.live.at("fullscreen")=="true");
  f.choose(1);f.frames();CHECK(f.live.at("fullscreen")=="false");++count;}
 {Fixture f(base,image);CHECK(f.init());f.fullscreen_preview();f.restore_failure=true;CHECK(!f.recreate(false,false));f.frames();
  CHECK(f.bridge->ready(base,manager)&&f.live.at("fullscreen")=="true");f.request(21);f.frames();
  CHECK(f.id()==23&&f.texts.at(f.yes)=="Retry restore");f.restore_failure=false;f.choose(0);f.frames();
  CHECK(f.live.at("fullscreen")=="false"&&f.saves==0&&f.texts.at(f.message)=="Changes restored");++count;}
 {Fixture f(base,image);CHECK(f.init());const auto component=REX_LOAD_U32(pages+84);f.callback(component,0);CHECK(!f.bridge->ready(base,manager));
  CHECK(f.recreate(false,false));CHECK(REX_LOAD_U32(pages+84)!=component&&f.names.at(f.root(FrontendPage::kVideo)).ends_with("_g2"));++count;}
 {Fixture f(base,image);CHECK(f.init());CHECK(f.recreate(false,false,false,false));CHECK(f.bridge->ready(base,manager)&&f.loads==2);++count;}
 {Fixture f(base,image);CHECK(f.init());f.fullscreen_preview();CHECK(!f.bridge->Initialize(f.ctx,base,manager,true));
  f.frames(7);CHECK(f.id()==19&&f.bridge->ready(base,manager)&&f.live.at("fullscreen")=="false"&&f.saves==0);++count;}
 {Fixture f(base,image);f.missing_service=true;CHECK(!f.init());f.missing_service=false;f.frames();
  CHECK(f.bridge->ready(base,manager)&&f.loads==2);++count;}
 {Fixture f(base,image);CHECK(f.init());f.request(22);f.frames();f.adjust("native_fsr3_quality",1);f.frames();f.action(RowAction::kApply);f.frames();
  f.restore_failure=true;CHECK(!f.recreate(false,false));f.frames();f.request(21);f.frames();CHECK(f.id()==23&&f.texts.at(f.yes)=="Retry restore");
  f.restore_failure=false;f.choose(0);f.frames();f.choose(0);f.frames();CHECK(f.id()==21&&f.live.at("native_fsr3_quality")=="quality");
  f.adjust("native_fps_limit",1);f.frames();f.action(RowAction::kApply);f.frames();CHECK(f.live.at("native_fps_limit")=="0"&&f.saves==1);++count;}
 for(unsigned failure=0;failure<6;++failure){Fixture f(base,image);
  if(failure==0)f.missing_service=true;if(failure==1)f.fail_thunk=2;if(failure==2)f.fail_allocation=1;
  if(failure==3)f.deny_layout=true;if(failure==4)f.missing_widget=true;if(failure==5)REX_STORE_U32(pages+21*4,0xBAD);
  CHECK(!f.init()&&!f.bridge->ready(base,manager));CHECK(!f.bridge->error().empty());
  CHECK(REX_LOAD_U8(f.video_row+457)==0&&REX_LOAD_U8(f.effects_row+457)==0&&f.refreshes==0);
  CHECK(REX_LOAD_U32(pages+22*4)==0&&REX_LOAD_U32(pages+23*4)==0);++count;
 }
 {Fixture f(base,image,true);CHECK(f.init());f.fullscreen_preview();
  CHECK(f.texts.at(f.message).find("Applying settings")!=std::string::npos&&f.texts.at(f.yes)=="Cancel"&&f.revision==1);
  f.event(f.generic_root,16);f.frames();CHECK(f.revision==2&&f.saves==0);
  CHECK(REX_LOAD_U32(generic+180)==3&&REX_LOAD_U32(generic+184)==0&&REX_LOAD_U8(f.menu+460)==0);
  CHECK(f.texts.at(f.message)=="Restoring settings");
  f.choose(0);f.event(f.generic_root,16);f.frames();CHECK(f.revision==2&&f.id()==23&&f.saves==0);++count;
  f.stale_revision=1;f.time+=std::chrono::milliseconds(100);f.frames();CHECK(f.id()==23&&REX_LOAD_U32(generic+180)==3);
  f.stale_revision=0;f.completed_revision=2;f.time+=std::chrono::milliseconds(100);f.frames();
  CHECK(f.texts.at(f.message)=="Changes restored"&&REX_LOAD_U32(generic+180)==0);
  f.choose(0);f.frames();CHECK(f.id()==21&&f.saves==0);++count;}
 {Fixture f(base,image,true);CHECK(f.init());f.fullscreen_preview();f.time+=std::chrono::seconds(4);f.completed_revision=1;f.frames();
  CHECK(f.texts.at(f.message).find("15 seconds")!=std::string::npos&&f.texts.at(f.yes)=="Keep"&&f.texts.at(f.no)=="Revert");
  f.choose(0);f.frames();CHECK(f.saves==1&&f.saved.at("fullscreen")=="true");++count;}
 {Fixture f(base,image,true);CHECK(f.init());f.fullscreen_preview();f.request(30);f.frames();CHECK(f.id()==30&&f.revision==2);
  f.completed_revision=2;f.time+=std::chrono::milliseconds(100);f.frames();CHECK(f.id()==30&&f.saves==0);
  f.request(21);f.frames();CHECK(f.id()==21&&f.live.at("fullscreen")=="false");++count;}
 {Fixture f(base,image,true);CHECK(f.init());f.fullscreen_preview();CHECK(!f.recreate(true,true));f.frames();
  CHECK(f.revision==2&&!f.bridge->ready(base,manager)&&REX_LOAD_U8(f.video_row+457)==0);
  f.stale_revision=1;f.time+=std::chrono::milliseconds(100);f.frames();CHECK(!f.bridge->ready(base,manager));
  f.stale_revision=0;f.completed_revision=2;f.time+=std::chrono::milliseconds(100);f.frames();
  CHECK(f.bridge->ready(base,manager)&&f.id()==19&&f.saves==0&&f.live.at("fullscreen")=="false");++count;}
 {Fixture f(base,image);CHECK(f.init());CHECK(REX_LOAD_U8(f.controls_row+457)==0&&REX_LOAD_U8(f.dlc_row+457)==0);
  const auto node=f.raw(32);f.xml_names[node]=f.str("ForeignMenu");
  CHECK(f.bridge->CompleteMenu(f.ctx,base,node,f.option_root)==0);
  CHECK(f.texts.at(f.video_row)=="Video"&&f.texts.at(f.effects_row)=="Effects");
  f.request(21);f.frames();const auto page=f.root(FrontendPage::kVideo);CHECK(REX_LOAD_U32(page+480));
  const auto first=CallGuest(sub_82EF9560,f.ctx,base,page);CHECK(first==f.children[page].at(f.names[page]+"Row0"));
  const auto last=MakeFrontendLayout(FrontendPage::kVideo).rows.size()-1;
  f.input(page,1);CHECK(CallGuest(sub_82EF9560,f.ctx,base,page)==f.children[page].at(f.names[page]+"Row"+std::to_string(last)));
  f.input(page,4);CHECK(CallGuest(sub_82EF9560,f.ctx,base,page)==first);++count;}
 {Fixture f(base,image);CHECK(f.init());f.main();CHECK(f.bridge->ShowMenuExtra(f.ctx,base,false));f.frames();
  CHECK(f.id()==23&&f.texts.at(f.message)=="Thanks for playing Dimensions Recompiled! Made by NeverCookFirst"&&f.texts.at(f.yes)=="Close");
  const auto key=REX_LOAD_U32(f.message+476),old_text=f.bridge->LookupLiteral(base,key);const auto old_value=f.string(old_text);
  const auto raw=f.str(old_value);CHECK(CallGuest(NativeOriginalLocalize,f.ctx,base,raw,0)==REX_LOAD_U32(0x847BCF2C));
  CHECK(CallGuest(sub_82A435C8,f.ctx,base,key,0)==old_text);
  CHECK(!f.bridge->LookupLiteral(base,f.str("Not an owned key"))&&!f.bridge->LookupLiteral(base,f.str("NATIVE_FRONTEND_UNKNOWN")));
  std::barrier start_readers(3);std::atomic<unsigned> alias_reads{0};
  auto lookup_retained=[&]{start_readers.arrive_and_wait();for(unsigned n=0;n<5000;++n){uint32_t stable=0;
    CHECK(f.bridge->LookupLiteral(base,old_text,&stable)==old_text&&stable!=0);
    CHECK(f.bridge->LookupLiteral(base,key)==old_text&&f.bridge->LookupLiteral(base,raw)==0);
    ++alias_reads;}};
  std::thread reader1(lookup_retained),reader2(lookup_retained);start_readers.arrive_and_wait();
  f.choose(0);f.frames();CHECK(f.id()==17&&f.writes==0&&f.saves==0);
  CHECK(f.bridge->ShowMenuExtra(f.ctx,base,true));f.frames();reader1.join();reader2.join();CHECK(alias_reads==10000);
  CHECK(f.string(old_text)==old_value&&f.bridge->LookupLiteral(base,key)==old_text&&f.bridge->LookupLiteral(base,old_text)==old_text);
  const auto next_key=REX_LOAD_U32(f.message+476);CHECK(f.bridge->LookupLiteral(base,next_key)!=old_text);
  uint32_t canonical=0;CHECK(f.bridge->LookupLiteral(base,key,&canonical)==old_text&&canonical!=key&&f.string(canonical)==f.string(key));
  CHECK(f.texts.at(f.message)=="Quit to desktop");
  f.choose(1);f.frames();CHECK(f.id()==17&&f.writes==0&&f.saves==0&&f.quit_requests==0);++count;}
 {Fixture f(base,image);CHECK(f.init());f.main();CHECK(f.bridge->ShowMenuExtra(f.ctx,base,false));f.frames();f.event(f.generic_root,16);f.frames();CHECK(f.id()==17&&f.quit_requests==0);++count;}
 {Fixture f(base,image);CHECK(f.init());f.main();CHECK(f.bridge->ShowMenuExtra(f.ctx,base,true));f.frames();CHECK(f.texts.at(f.yes)=="Quit Game"&&f.texts.at(f.no)=="Cancel");
  f.choose(1);f.frames();CHECK(f.id()==17&&f.quit_requests==0);CHECK(f.bridge->ShowMenuExtra(f.ctx,base,true));f.frames();f.choose(0);CHECK(f.quit_requests==1);++count;}
 {Fixture f(base,image);CHECK(f.init());const auto target=f.widget("input_reference"),vt=f.raw(512),node=f.raw(16),head=f.raw(4),packet=f.raw(160),ref=f.raw(32);
  REX_STORE_U32(vt+332,f.thunk(CaptureInput));REX_STORE_U32(target,vt);REX_STORE_U32(ref,f.widget_vtable);REX_STORE_U32(packet,ref);
  REX_STORE_U32(node+8,target);REX_STORE_U8(node+12,1);REX_STORE_U32(head,node);unsigned cases=0;
  for(uint32_t forced:{0u,9u,10u,11u,12u,15u,16u,17u})for(uint32_t mask=0;mask<16;++mask)for(uint32_t buttons:{0u,0x40000000u,0x80000000u,0xC0000000u}){
   const auto expected=NativeMenuInputEvent(forced,mask,buttons);if(!expected)continue;
   REX_STORE_U32(ui+148,forced);REX_STORE_U32(packet+8,buttons);captured_input=0;
   CallGuest(sub_82F0E1B8,f.ctx,base,ui,packet,mask,head);CHECK(captured_input==expected);++cases;
  }
  CHECK(cases==511);++count;}
 {Fixture f(base,image);CHECK(f.init());f.request(22);f.frames();const auto page=f.root(FrontendPage::kEffects);
  const auto foreign=f.raw(160);REX_STORE_U32(foreign+8,0x40000000u);
  CHECK(!f.bridge->Input(f.ctx,base,ui,foreign,4,0)&&!f.bridge->Input(f.ctx,base,ui,foreign,4,1));
  // Navigation and adjustment must not depend on a GSC menu child forwarding
  // events, or even on a cached native selected-widget pointer surviving.
  REX_STORE_U32(page+488,0);f.input(page,4);REX_STORE_U32(page+484,0);f.input(page,4);
  CHECK(CallGuest(sub_82EF9560,f.ctx,base,page)==f.children[page].at(f.names[page]+"Row2"));
  f.input(page,2);f.frames();f.action(RowAction::kApply);f.frames();CHECK(f.live.at("native_fsr3_sharpen")=="false");++count;}
 {Fixture f(base,image);CHECK(f.init());f.request(22);f.frames();f.sounds.clear();
  const auto page=f.root(FrontendPage::kEffects);const auto before=f.focus_events;
  f.input(page,4);CHECK(f.focus_events==before+1&&f.sounds==std::vector<unsigned>({0}));
  f.input(page,2);CHECK(f.sounds==std::vector<unsigned>({0,1}));f.frames();
  f.input(page,0,0x80000000);CHECK(f.sounds==std::vector<unsigned>({0,1,2}));++count;}
 for(unsigned mode=0;mode<4;++mode){Fixture f(base,image);CHECK(f.init());
  f.request(22);f.frames();f.event(f.root(FrontendPage::kEffects),16);f.frames();CHECK(f.id()==19);
  const auto dead=f.root(FrontendPage::kEffects),old_component=REX_LOAD_U32(pages+22*4);
  // Model the observed zero-vtable fault after returning to the main menu:
  // retain manager, UI, components and slots, without an init notification.
  REX_STORE_U32(dead,0);if(mode==0)REX_STORE_U32(registry+24,0);
  if(mode==2)f.unreadable_root=dead;
  CHECK(!f.bridge->ready(base,manager));
  if(mode==3){for(auto id:{21u,22u,23u}){REX_STORE_U32(pages+id*4,0);REX_STORE_U32(interfaces+id*4,0);}f.frames();}
  f.request(22);
  if(mode==2){f.frames();CHECK(f.id()==19&&REX_LOAD_U32(manager+168)==UINT32_MAX);f.unreadable_root=0;}
  f.frames(8);CHECK(f.bridge->ready(base,manager)&&f.id()==22&&f.root(FrontendPage::kEffects)!=dead);
  CHECK(REX_LOAD_U32(old_component+20)==0);f.adjust("native_fsr3_quality",1);f.frames();f.action(RowAction::kApply);f.frames();
  CHECK(f.id()==23&&f.live.at("native_fsr3_quality")=="balanced");f.choose(1);f.frames();++count;
 }
 {Fixture f(base,image);CHECK(f.init());const auto old=REX_LOAD_U32(pages+22*4);f.callback(old,0);
  CHECK(!f.bridge->ready(base,manager));f.frames();CHECK(!f.bridge->ready(base,manager));
  f.event(f.option_root,2);CHECK(f.bridge->ready(base,manager)&&REX_LOAD_U32(pages+22*4)!=old);
  f.request(22);f.frames();CHECK(f.id()==22);++count;}
 for(unsigned mode=0;mode<4;++mode){Fixture f(base,image);CHECK(f.init());
  const auto dead=f.root(FrontendPage::kEffects);REX_STORE_U32(dead,0);
  // Loading can retain manager/slot addresses, or a stale Options ID. It
  // must never enter the native layout loader from per-frame maintenance.
  if(mode==0){f.request(30);f.frames();}
  if(mode==1)REX_STORE_U32(f.option_root+448,UINT32_MAX);
  if(mode==2)REX_STORE_U32(manager+168,30);
  if(mode==3){f.bridge->OptionsEntering(f.ctx,base,options+4);}
  f.scene_loading=true;const auto loads=f.loads,allocations=f.allocations;
  f.frames(16);CHECK(f.loads==loads&&f.allocations==allocations&&!f.bridge->ready(base,manager));
  f.scene_loading=false;
  if(f.id()!=19){f.request(19);f.frames(8);}else{
   REX_STORE_U32(manager+168,UINT32_MAX);REX_STORE_U32(f.option_root+448,UINT32_MAX);
   f.event(f.option_root,2);REX_STORE_U32(f.option_root+448,2);f.frames(8);
  }
  CHECK(f.bridge->ready(base,manager)&&f.loads==loads+1);
  f.request(22);f.frames();CHECK(f.id()==22);f.adjust("native_fsr3_quality",1);f.frames();++count;
 }
 {Fixture f(base,image);CHECK(f.init());const auto dead=f.root(FrontendPage::kEffects);REX_STORE_U32(dead,0);
  f.main();f.frames();CHECK(!f.bridge->ready(base,manager));
  CHECK(f.bridge->ShowMenuExtra(f.ctx,base,false));f.frames();
  CHECK(f.id()==23&&f.texts.at(f.message)=="Thanks for playing Dimensions Recompiled! Made by NeverCookFirst");++count;}
 {Fixture f(base,image);CHECK(f.init());f.fullscreen_preview();f.choose(1);f.frames();
  CHECK(f.id()==23&&f.texts.at(f.message)=="Changes restored");
  f.input(f.generic_root,0,0x40000000);f.frames();CHECK(f.id()==21);++count;}
 {Fixture f(base,image);CHECK(f.init());f.fullscreen_preview();
  f.input(f.generic_root,2);f.input(f.generic_root,0,0x40000000);f.frames();
  CHECK(f.id()==23&&f.live.at("fullscreen")=="false"&&f.saves==0);
  f.input(f.generic_root,0,0x40000000);f.frames();CHECK(f.id()==21);++count;}
 {Fixture f(base,image);CHECK(f.init());f.main();CHECK(f.bridge->ShowMenuExtra(f.ctx,base,false));f.frames();
  f.input(f.generic_root,0,0x40000000);f.frames();CHECK(f.id()==17);
  CHECK(f.bridge->ShowMenuExtra(f.ctx,base,true));f.frames();
  f.input(f.generic_root,4);CHECK(CallGuest(sub_82EF9560,f.ctx,base,f.generic_root)==f.no);
  f.input(f.generic_root,0,0x40000000);f.frames();CHECK(f.id()==17&&f.quit_requests==0);
  CHECK(f.bridge->ShowMenuExtra(f.ctx,base,true));f.frames();
  CHECK(CallGuest(sub_82EF9560,f.ctx,base,f.generic_root)==f.yes);
  f.input(f.generic_root,0,0x40000000);f.frames();CHECK(f.quit_requests==1);++count;}
 {Fixture f(base,image);CHECK(f.init());const auto menu=f.pause();
  REX_STORE_U32(menu+8,9);REX_STORE_U32(menu+64,9);REX_STORE_U32(menu+112,1);
  CHECK(f.bridge->UpdatePauseOptions(f.ctx,base,menu,PauseOriginal));f.frames(8);
  CHECK(f.id()==21&&f.bridge->PauseOverlayActive(base,menu)&&f.legacy_updates==0);
  for(auto off:{104u,108u,112u,116u})REX_STORE_U32(menu+off,1);
  REX_STORE_U32(menu+8,3);REX_STORE_U32(menu+64,3);
  CHECK(f.bridge->UpdatePauseOptions(f.ctx,base,menu,PauseOriginal));
  for(auto off:{104u,108u,112u,116u})CHECK(REX_LOAD_U32(menu+off)==0);
  CHECK(REX_LOAD_U32(menu+8)==9&&REX_LOAD_U32(menu+64)==9);
  f.event(f.root(FrontendPage::kVideo),16);f.frames(8);CHECK(f.id()==0&&!f.bridge->PauseOverlayActive(base,menu));
  REX_STORE_U32(menu+116,1);CHECK(f.bridge->UpdatePauseOptions(f.ctx,base,menu,PauseOriginal));CHECK(REX_LOAD_U32(menu+116)==0&&f.legacy_updates==0);
  CHECK(REX_LOAD_U32(0x847D2D58)==2);++count;}
 {Fixture f(base,image);CHECK(f.init());const auto menu=f.pause();
  REX_STORE_U32(menu+8,10);REX_STORE_U32(menu+64,10);REX_STORE_U32(menu+112,1);
  CHECK(f.bridge->UpdatePauseOptions(f.ctx,base,menu,PauseOriginal));f.frames(8);CHECK(f.id()==22);
  f.adjust("native_fsr3_quality",1);f.frames();f.action(RowAction::kApply);f.frames();CHECK(f.id()==23);
  f.input(f.generic_root,2);f.input(f.generic_root,0,0x40000000);f.frames();CHECK(f.live.at("native_fsr3_quality")=="quality");
  f.input(f.generic_root,0,0x40000000);f.frames();CHECK(f.id()==22);
  f.event(f.root(FrontendPage::kEffects),16);f.frames(8);CHECK(f.id()==0&&REX_LOAD_U32(0x847D2D58)==2);++count;}
 {Fixture f(base,image);CHECK(f.init());const auto menu=f.pause();
  REX_STORE_U32(menu+64,11);REX_STORE_U32(menu+112,1);CHECK(f.bridge->UpdatePauseOptions(f.ctx,base,menu,PauseOriginal));
  CHECK(f.legacy_updates==1&&REX_LOAD_U32(menu+64)==11);
  REX_STORE_U32(menu+64,1);CHECK(!f.bridge->UpdatePauseOptions(f.ctx,base,menu,PauseOriginal));
  REX_STORE_U32(0x847D2D58,0);CHECK(!f.bridge->DrawPauseEntries(f.ctx,base,menu,PauseRow));
  CHECK(!f.bridge->UpdatePauseOptions(f.ctx,base,menu,PauseOriginal));++count;}
 {Fixture f(base,image);CHECK(f.init());const auto dead=f.root(FrontendPage::kEffects),menu=f.pause();
  const auto refreshes=f.refreshes;REX_STORE_U32(dead,0);REX_STORE_U32(f.option_root,0);
  REX_STORE_U32(menu+64,10);REX_STORE_U32(menu+112,1);
  CHECK(f.bridge->UpdatePauseOptions(f.ctx,base,menu,PauseOriginal));f.frames(8);
  CHECK(f.id()==22&&f.bridge->ready(base,manager)&&f.root(FrontendPage::kEffects)!=dead&&f.refreshes==refreshes);
  f.event(f.root(FrontendPage::kEffects),16);f.frames(8);CHECK(f.id()==0);++count;}
 {Fixture f(base,image);CHECK(f.init());const auto menu=f.pause();
  REX_LOOKUP_FUNC(base,0x839D08A0)=CountPauseDraw;
  // The actual title initialization performs a non-rendering count pass.
  // Descriptor +20 stays 9: row bounds come from emitted row count instead.
  REX_STORE_U32(0x849C7EB8+9*28+20,9);
  for(unsigned selected:{9u,10u,11u}){
   CallGuest(sub_82EDE1D0,f.ctx,base,menu,selected,0);
   CHECK(REX_LOAD_U32(menu+156)==12&&REX_LOAD_U16(menu+18)==11&&REX_LOAD_U16(menu+10)==selected);
  }
  CHECK(REX_LOAD_U32(0x849C7EB8+9*28+20)==9);++count;}
 {Fixture f(base,image);CHECK(f.init());const auto list=options+0x200;
  REX_STORE_U32(list+64,3);CallGuest(sub_82F0F958,f.ctx,base,list);CHECK(f.dynamic_clones==3);
  for(unsigned i=0;i<24;++i){f.bridge->OptionsEntering(f.ctx,base,options);}
  CHECK(f.dynamic_clones==3&&REX_LOAD_U8(f.video_row+457)==1&&REX_LOAD_U8(f.effects_row+457)==1);++count;}
 // Owner regression: VSync Apply enters a popup while the GenericMenu child
 // has a non-pointer +484 word. Exercise both values and every confirmation.
 for(bool initial:{false,true})for(unsigned outcome=0;outcome<3;++outcome){
  Fixture f(base,image);f.live["native_vsync"]=initial?"true":"false";CHECK(f.init());
  f.request(21);f.frames();f.adjust("native_vsync",1);f.frames();f.action(RowAction::kApply);f.frames();
  CHECK(f.id()==23&&REX_LOAD_U32(f.menu+484)==1);
  CHECK(CallGuest(sub_82EF9560,f.ctx,base,f.generic_root)==f.yes);
  if(outcome==2){f.time+=std::chrono::seconds(15);f.service->AdvanceFromUI();f.frames();}
  else{
   if(outcome==1){f.input(f.generic_root,2);CHECK(CallGuest(sub_82EF9560,f.ctx,base,f.generic_root)==f.no);}
   f.input(f.generic_root,0,0x40000000);f.frames();
  }
  CHECK(f.live.at("native_vsync")==((outcome==0?!initial:initial)?"true":"false"));
  CHECK(f.saves==(outcome==0?1u:0u)&&f.id()==23);
  f.input(f.generic_root,0,0x40000000);f.frames();CHECK(f.id()==21);++count;
 }
 for(bool quit:{false,true})for(bool back:{false,true})for(bool root_gap:{false,true}){
  Fixture f(base,image);CHECK(f.init());f.main();
  const auto credits=f.widget("xml_help"),quit_row=f.widget("xml_switchprofile");
  f.children[f.main_root]["xml_help"]=credits;f.children[f.main_root]["xml_switchprofile"]=quit_row;
  const auto expected=quit?quit_row:credits;
  CallGuest(sub_82EFEEB8,f.ctx,base,f.main_root,expected,1);
  CHECK(f.bridge->ShowMenuExtra(f.ctx,base,quit));f.frames();CHECK(f.id()==23);
  // Model the original page enter handler resetting selection on return.
  CallGuest(sub_82EFEEB8,f.ctx,base,f.main_root,f.video_row,1);
  if(quit&&!back){f.input(f.generic_root,4);CHECK(CallGuest(sub_82EF9560,f.ctx,base,f.generic_root)==f.no);}
  f.input(f.generic_root,0,back?0x80000000:0x40000000);
  if(root_gap)f.unreadable_root=f.main_root;
  f.frame();CHECK(f.id()==17);
  if(root_gap){CHECK(CallGuest(sub_82EF9560,f.ctx,base,f.main_root)==f.video_row);f.unreadable_root=0;}
  f.frames();
  CHECK(f.id()==17&&f.quit_requests==0);
  CHECK(CallGuest(sub_82EF9560,f.ctx,base,f.main_root)==expected);
  CallGuest(sub_82EFEEB8,f.ctx,base,f.main_root,f.video_row,1);f.frames();
  CHECK(CallGuest(sub_82EF9560,f.ctx,base,f.main_root)==f.video_row);++count;
 }
 // First main-menu click must work without visiting Options or prebinding.
 for(bool quit:{false,true}){
  Fixture f(base,image);f.main();CHECK(f.main_root!=f.main_child);
  CHECK(REX_LOAD_U32(main_component+20)==f.main_root&&REX_LOAD_U32(main_component+40)==f.main_root);
  CHECK(REX_LOAD_U32(f.main_child+448)==UINT32_MAX&&REX_LOAD_U32(f.main_child+476)==0&&REX_LOAD_U32(f.main_child+484)==1);
  CHECK(f.bridge->ShowMenuExtra(f.ctx,base,quit));f.frames();CHECK(f.id()==23&&f.loads==2);
  f.event(f.generic_root,16);f.frames();CHECK(f.id()==17&&f.quit_requests==0);++count;
 }
 // Reject stale/unrelated main-menu objects, but retain an explicit click
 // through a temporary resource binding gap and retry once identities agree.
 for(unsigned invalid=0;invalid<7;++invalid){
  Fixture f(base,image);CHECK(f.init());f.main();const auto owner=REX_LOAD_U32(f.main_root+476);
  const auto vtable=REX_LOAD_U32(f.main_root);
  if(invalid==0)REX_STORE_U32(0x849F5F40,f.option_root);
  if(invalid==1)REX_STORE_U32(main_component+40,f.option_root);
  if(invalid==2)REX_STORE_U32(f.main_root+476,options);
  if(invalid==3)REX_STORE_U32(f.main_root+448,UINT32_MAX);
  if(invalid==4)f.unreadable_root=f.main_root;
  if(invalid==5)REX_STORE_U32(f.main_root,0);
  if(invalid==6)f.children[f.main_root]["MainMenu"]=f.option_root;
  CHECK(f.bridge->ShowMenuExtra(f.ctx,base,false));f.frames();CHECK(f.id()==17&&f.loads==2&&f.configs==0);
  REX_STORE_U32(0x849F5F40,f.main_child);REX_STORE_U32(main_component+40,f.main_root);
  REX_STORE_U32(f.main_root+476,owner);REX_STORE_U32(f.main_root+448,2);REX_STORE_U32(f.main_root,vtable);
  f.unreadable_root=0;f.children[f.main_root]["MainMenu"]=f.main_child;
  f.frames();CHECK(f.id()==23);++count;
 }
 {Fixture f(base,image);CHECK(f.init());CHECK(f.id()==19);
  CHECK(!f.bridge->ShowMenuExtra(f.ctx,base,false)&&!f.bridge->ShowMenuExtra(f.ctx,base,true));f.frames();CHECK(f.id()==19);++count;}
 for(auto offset:{168u,172u})for(bool depart:{false,true}){
  Fixture f(base,image);f.main();REX_STORE_U32(manager+offset,34);
  const auto allocations=f.allocations;
  CHECK(f.bridge->ShowMenuExtra(f.ctx,base,false));CHECK(f.loads==0&&f.allocations==allocations);
  // Inspect the pre-frame retry without running the title's pending34 page.
  f.bridge->BeforeFrame(f.ctx,base,manager);CHECK(f.loads==0&&f.allocations==allocations);
  REX_STORE_U32(manager+offset,UINT32_MAX);
  if(depart){REX_STORE_U32(manager+164,0);f.frames();CHECK(f.id()==0&&f.loads==0&&f.allocations==allocations);}
  else {f.frames();CHECK(f.id()==23&&f.loads==2);}
  ++count;
 }
 // Quit starts on its visible first row. One Down selects Cancel after
 // initial entry or reopening; a late title first-row selection agrees with
 // the input model. Accept must activate the visibly selected row.
 for(bool late_default:{false,true})for(bool reopen:{false,true}){
  Fixture f(base,image);CHECK(f.init());f.main();
  if(reopen){CHECK(f.bridge->ShowMenuExtra(f.ctx,base,false));f.frames();f.input(f.generic_root,0,0x80000000);f.frames();CHECK(f.id()==17);}
  CHECK(f.bridge->ShowMenuExtra(f.ctx,base,true));f.frames();
  if(late_default)CallGuest(sub_82EFEEB8,f.ctx,base,f.generic_root,f.yes,1);
  CHECK(CallGuest(sub_82EF9560,f.ctx,base,f.generic_root)==f.yes);
  f.input(f.generic_root,4);CHECK(CallGuest(sub_82EF9560,f.ctx,base,f.generic_root)==f.no);
  f.input(f.generic_root,0,0x40000000);f.frames();CHECK(f.id()==17&&f.quit_requests==0);
  CHECK(f.bridge->ShowMenuExtra(f.ctx,base,true));f.frames();
  f.input(f.generic_root,4);CHECK(CallGuest(sub_82EF9560,f.ctx,base,f.generic_root)==f.no);
  f.input(f.generic_root,1);CHECK(CallGuest(sub_82EF9560,f.ctx,base,f.generic_root)==f.yes);
  f.input(f.generic_root,0,0x40000000);f.frames();CHECK(f.quit_requests==1);++count;
 }
 for(unsigned invalid=0;invalid<7;++invalid){
  Fixture f(base,image);CHECK(f.init());const auto group=f.widget("OptionsMenu"),list=options+0x200;
  f.children[f.option_root]["OptionsMenu"]=group;REX_STORE_U32(list,group);REX_STORE_U32(group+436,2);
  for(auto matrix:{144u,208u})for(unsigned i=0;i<16;++i)
   REX_STORE_U32(group+matrix+i*4,std::bit_cast<uint32_t>(i%5==0?1.0f:0.0f));
  REX_STORE_U32(group+196,std::bit_cast<uint32_t>(-0.085f));REX_STORE_U32(group+260,std::bit_cast<uint32_t>(-0.085f));
  // Historical owner matrices contain a two-ULP identity coefficient.
  REX_STORE_U32(group+144,0x3F800002);REX_STORE_U32(group+208,0x3F800002);
  if(invalid==1)REX_STORE_U32(list,f.option_root);
  if(invalid==2)REX_STORE_U32(group+436,6);
  if(invalid==3)f.children[f.option_root]["OptionsMenu"]=f.video_row;
  if(invalid==4)REX_STORE_U32(group+212,std::bit_cast<uint32_t>(0.2f));
  if(invalid==5)REX_STORE_U32(group+208,0x7FC00000);
  if(invalid==6)f.unreadable_root=group;
  std::array<uint8_t,128> before{};std::memcpy(before.data(),base+group+144,before.size());
  const auto root_before=REX_LOAD_U32(f.option_root+144),row_before=REX_LOAD_U32(f.video_row+144);
  for(unsigned entry=0;entry<20;++entry)f.bridge->RevealOptions(f.ctx,base,options);
  CHECK(REX_LOAD_U32(f.option_root+144)==root_before&&REX_LOAD_U32(f.video_row+144)==row_before);
  if(invalid)CHECK(std::memcmp(before.data(),base+group+144,before.size())==0);
  else {
   for(auto offset:{144u,164u,208u,228u})CHECK(REX_LOAD_U32(group+offset)==std::bit_cast<uint32_t>(0.85f));
   CHECK(REX_LOAD_U32(group+196)==std::bit_cast<uint32_t>(-0.085f)&&REX_LOAD_U32(group+260)==std::bit_cast<uint32_t>(-0.085f));
  }
  ++count;
 }
 VirtualFree(base,0,MEM_RELEASE);std::printf("PASS %u native bridge scenarios plus 511 differential input masks: owned input controller, native selection/dialogs, scene graph loss, native sound routing, distinct MainMenuPage/MainMenu Credits/Quit\n",count);
}
