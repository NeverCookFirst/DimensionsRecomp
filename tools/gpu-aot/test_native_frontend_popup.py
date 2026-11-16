"""Execute TU23's translated native dialog control flow without starting the game.

Text allocation, widget drawing and input focus are boundary mocks. The actual
translated configure/reset/publish/dispatch routines and PPC registers execute.
"""
import argparse,hashlib,json,re,subprocess
from pathlib import Path

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('output',type=Path);p.add_argument('--compiler',default='clang++')
    p.add_argument('--image',type=Path,required=True)
    a=p.parse_args();out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
    root=Path(__file__).resolve().parents[2];gen=root/'rexlego/generated/default'
    names=['sub_82FB74C8','sub_82FBDFF8','sub_82FBDF58','sub_82FD0508','sub_82FD9270',
           'sub_82EF61B0','sub_82EF61E0','sub_82EF42D8',
           '__savegprlr_26','__savegprlr_27','__savegprlr_28','__savegprlr_29',
           '__restgprlr_26','__restgprlr_27','__restgprlr_28','__restgprlr_29']
    found={}
    for f in gen.glob('legodimensions_recomp.*.cpp'):
        text=f.read_text()
        for name in names:
            start=text.find('DEFINE_REX_FUNC('+name+')')
            if start<0:continue
            end=text.find('\nDEFINE_REX_FUNC(',start+1)
            found[name]=text[start:end if end>=0 else len(text)]
    assert set(found)==set(names),set(names)-found.keys()
    declarations='\n'.join('void '+name+'(PPCContext&,uint8_t*);' for name in names)
    prefix=r'''
#define NOMINMAX
#include "legodimensions_pch.h"
#include "native_frontend_guest.h"
#include <windows.h>
#include <cstdio>
#include <fstream>
#include <vector>
#include <stdexcept>
#undef DEFINE_REX_FUNC
#define DEFINE_REX_FUNC(name) void name(PPCContext& ctx,uint8_t* base)
#define CHECK(x) do{if(!(x)){std::fprintf(stderr,"failed %d: %s\n",__LINE__,#x);std::exit(23);}}while(0)
using legodimensions::native_frontend::CallGuest;
using legodimensions::native_frontend::ConfigurePopup;
using legodimensions::native_frontend::PopupRequest;
using legodimensions::native_frontend::PopupButtons;
constexpr uint32_t kArena=0x20000000,kObject=kArena+0x1000,kMessage=kArena+0x2000,
 kMenu=kArena+0x3000,kYes=kArena+0x4000,kNo=kArena+0x5000,kInterface=kArena+0x6000,
 kVtable=kArena+0x7000,kInterfaceVtable=kArena+0x8000,kFooter=kArena+0x9000,
 kText=kArena+0xA000,kDefaultYes=kText+256,kDefaultNo=kText+512,kLabels=kText+768,
 kLabelYes=kText+1024,kLabelNo=kText+1280,kEmpty=kText+1536,kDefaultAccept=kText+1792;
uint32_t visible_calls=0,footer_calls=0,select_calls=0,back_calls=0,widget_calls=0,
 event_calls=0,configured=0;int selected=0;
PopupRequest request;
void Visible(PPCContext&ctx,uint8_t*base){++visible_calls;REX_STORE_U8(ctx.r3.u32+460,ctx.r4.u32);}
void Event(PPCContext&ctx,uint8_t*base);
void Select(PPCContext&ctx,uint8_t*base){++select_calls;REX_STORE_U32(ctx.r3.u32+4,ctx.r4.u32);}
void Back(PPCContext&ctx,uint8_t*base){++back_calls;REX_STORE_U32(ctx.r3.u32+4,0xFFFFFFFF);}
void Widget(PPCContext&,uint8_t*){++widget_calls;}
namespace rex::runtime {PPCFunc* ResolveIndirectFunction(uint32_t target){
 switch(target){case 0x100:return Visible;case 0x104:return Event;case 0x108:return Select;
 case 0x10C:return Back;case 0x110:return Widget;default:CHECK(false);return nullptr;}}}
void sub_82EFE8F0(PPCContext&ctx,uint8_t*base){REX_STORE_U32(ctx.r3.u32+476,ctx.r4.u32);}
void sub_82EFEFA0(PPCContext&ctx,uint8_t*base){REX_STORE_U32(ctx.r3.u32+480,ctx.r4.u32);}
void sub_82EFE788(PPCContext&ctx,uint8_t*){CHECK(ctx.r4.u32==0);ctx.r3.u32+=0x100;}
void sub_82F0DBA0(PPCContext&ctx,uint8_t*){CHECK(ctx.r3.u32==kMenu);CHECK(ctx.r4.u32==0);ctx.r3.s64=selected;}
void sub_82FBF270(PPCContext&ctx,uint8_t*){CHECK(ctx.r3.u32==kFooter);++footer_calls;}
'''
    body='\n'.join(found[name] for name in names)
    suffix=r'''
void Event(PPCContext&ctx,uint8_t*base){++event_calls;CHECK(ctx.r3.u32==kInterface);
 if(ctx.r5.u32==2){CHECK(ConfigurePopup(sub_82FBDFF8,ctx,base,request));++configured;}}
void Dispatch(PPCContext&ctx,uint8_t*base,uint32_t event){
 const auto saved=ctx;CallGuest(sub_82FD9270,ctx,base,kObject,0,event,0);
 CHECK(std::memcmp(&ctx,&saved,sizeof(ctx))==0);
}
int main(int argc,char**argv){
 CHECK(argc==2);
 auto* base=static_cast<uint8_t*>(VirtualAlloc(nullptr,0x100000000ull,MEM_RESERVE,PAGE_NOACCESS));CHECK(base);
 CHECK(VirtualAlloc(base+0x82000000,0x2B80000,MEM_COMMIT,PAGE_READWRITE));
 CHECK(VirtualAlloc(base+kArena,0x100000,MEM_COMMIT,PAGE_READWRITE));
 std::ifstream image(argv[1],std::ios::binary);CHECK(image);
 image.read(reinterpret_cast<char*>(base+0x82000000),0x2B80000);CHECK(image.gcount()==0x2B80000);
 auto str=[&](uint32_t p,const char*s){std::strcpy(reinterpret_cast<char*>(base+p),s);};
 str(kText,"Keep these display settings?");str(kDefaultYes,"Yes");str(kDefaultNo,"No");
 str(kLabelYes,"Keep");str(kLabelNo,"Revert");str(kEmpty,"");str(kDefaultAccept,"Continue");
 REX_STORE_U32(kLabels,kLabelYes);REX_STORE_U32(kLabels+4,kLabelNo);
 REX_STORE_U32(0x849C7D70,kDefaultYes);REX_STORE_U32(0x849C7D74,kDefaultNo);
 REX_STORE_U32(0x849F5CC0,kDefaultAccept);
 REX_STORE_U32(0x847BCF2C,kEmpty); // title's empty-string pointer (lis -31620, -12500)
 REX_STORE_U32(0x849F566C,kFooter);
 REX_STORE_U32(kFooter+40,kText);REX_STORE_U32(kFooter+44,kEmpty);
 REX_STORE_U32(kObject+32,kMessage);REX_STORE_U32(kObject+36,kMenu);
 REX_STORE_U32(kObject+40,kYes);REX_STORE_U32(kObject+44,kNo);
 REX_STORE_U32(kObject+176,kInterface);REX_STORE_U32(kInterface,kInterfaceVtable);
 for(uint32_t p:{kMessage,kMenu,kYes,kNo}){REX_STORE_U32(p,kVtable);}
 REX_STORE_U32(kVtable+196,0x100);REX_STORE_U32(kVtable+332,0x110);
 REX_STORE_U32(kInterfaceVtable+8,0x104);REX_STORE_U32(kInterfaceVtable+16,0x108);
 REX_STORE_U32(kInterfaceVtable+20,0x10C);
 for(unsigned i=0;i<64;++i){base[kObject+48+i]=uint8_t(i+1);base[kObject+112+i]=uint8_t(i+65);}
 PPCContext ctx{};ctx.r1.u32=kArena+0xF0000;ctx.r3.u64=0x1122334455667788ull;ctx.r31.u64=0x8877665544332211ull;
 ctx.lr=0xDEADBEEF;ctx.ctr.u64=0xCAFE;ctx.r10.u64=0x9999;
 const auto saved=ctx;
 // The production bridge must restore context even if a host boundary throws.
 bool threw=false;
 try{CallGuest([](PPCContext&c,uint8_t*){c.r3.u64=17;c.r31.u64=29;throw std::runtime_error("fixture");},ctx,base,9);}
 catch(const std::runtime_error&){threw=true;}
 CHECK(threw&&std::memcmp(&ctx,&saved,sizeof(ctx))==0);
 for(auto buttons:{PopupButtons::kAccept,PopupButtons::kAcceptCancel,PopupButtons::kNone})
 for(bool centered:{false,true})for(unsigned count=0;count<=2;++count){
  request={kObject,kText,buttons,3,count,count?kLabels:0,centered};
  const unsigned before=configured;Dispatch(ctx,base,2);CHECK(configured==before+1);
  CHECK(REX_LOAD_U32(kObject+180)==uint32_t(buttons));
  CHECK(REX_LOAD_U32(kObject+184)==3&&REX_LOAD_U8(kObject+188)==1);
  CHECK(REX_LOAD_U32(kMessage+476)==kText);
  CHECK(REX_LOAD_U32(kYes+480)==(count?kLabelYes:buttons==PopupButtons::kAccept?kDefaultAccept:kDefaultYes));
  CHECK(REX_LOAD_U32(kNo+480)==(count>1?kLabelNo:kDefaultNo));
  CHECK(REX_LOAD_U8(kYes+460)==(buttons!=PopupButtons::kNone));
  CHECK(REX_LOAD_U8(kNo+460)==(buttons==PopupButtons::kAcceptCancel));
  CHECK(REX_LOAD_U8(kMenu+544)==1);
  CHECK(std::memcmp(base+kMenu+144,base+kObject+48,64)==0);
  CHECK(std::memcmp(base+kMessage+144,base+(centered?0x847BC800:kObject+112),64)==0);
  const auto footer_before=footer_calls;Dispatch(ctx,base,3);
  CHECK(REX_LOAD_U8(kMessage+460)==1&&footer_calls==footer_before+1);
  CHECK(REX_LOAD_U8(kMenu+460)==(buttons!=PopupButtons::kNone));
  CHECK(REX_LOAD_U32(kFooter+80)==3);
  for(selected=0;selected<2;++selected){const auto n=select_calls;Dispatch(ctx,base,15);
   CHECK(select_calls==n+1&&REX_LOAD_U32(kInterface+4)==uint32_t(selected));}
  const auto n=back_calls;Dispatch(ctx,base,16);CHECK(back_calls==n+1&&REX_LOAD_U32(kInterface+4)==0xFFFFFFFF);
 }
 request={kObject,kText,PopupButtons::kAcceptCancel,3,0,0,false};
 for(unsigned bad=0;bad<5;++bad){auto invalid=request;
  if(bad==0)invalid.component=0;if(bad==1)invalid.message=0;
  if(bad==2)invalid.label_count=3;if(bad==3)invalid.label_count=1;
  if(bad==4)invalid.buttons=static_cast<PopupButtons>(99);
  CHECK(!ConfigurePopup(sub_82FBDFF8,ctx,base,invalid));CHECK(std::memcmp(&ctx,&saved,sizeof(ctx))==0);
 }
 std::printf("PASS: %u native dialog configurations; translated reset/show/publish/Accept/Back, button modes, labels, transforms, prompts and PPC context preservation\n",configured);
 VirtualFree(base,0,MEM_RELEASE);
}
'''
    cpp=out/'native-popup.cpp';cpp.write_text(prefix+declarations+'\n'+body+'\n'+suffix)
    exe=out/'native-popup.exe'
    command=[a.compiler,'-std=c++23','-O2','-mavx2','-D_CRT_SECURE_NO_WARNINGS','-DSPDLOG_FMT_EXTERNAL',
        '-I'+str(root/'rexlego/src'),'-I'+str(gen),'-I'+str(root.parent/'dev-assets/sdk-install/include'),
        str(cpp),'-o',str(exe)]
    compile_result=subprocess.run(command,capture_output=True,text=True,timeout=90)
    (out/'compile.log').write_text(compile_result.stdout+compile_result.stderr);compile_result.check_returncode()
    result=subprocess.run([str(exe),str(a.image.resolve())],capture_output=True,text=True,timeout=20)
    (out/'result.log').write_text(result.stdout+result.stderr);result.check_returncode()
    header=root/'rexlego/src/native_frontend_guest.h';header_text=header.read_text();negative=[]
    for label,old,new in [
        ('lost-context','~Restore() { target = saved; }','~Restore() {}'),
        ('dropped-custom-labels','request.label_count, request.labels, request.centered ? 1 : 0','0, request.labels, request.centered ? 1 : 0')]:
        assert header_text.count(old)==1
        mutant=out/label;mutant.mkdir(exist_ok=True)
        (mutant/header.name).write_text(header_text.replace(old,new))
        binary=mutant/'native-popup.exe'
        child_command=command.copy();child_command.insert(1,'-I'+str(mutant));child_command[-1]=str(binary)
        compiled=subprocess.run(child_command,capture_output=True,text=True,timeout=90)
        (mutant/'compile.log').write_text(compiled.stdout+compiled.stderr);compiled.check_returncode()
        control=subprocess.run([str(binary),str(a.image.resolve())],capture_output=True,text=True,timeout=20)
        (mutant/'result.log').write_text(control.stdout+control.stderr)
        assert control.returncode==23,(label,control.returncode,control.stdout,control.stderr)
        negative.append(dict(name=label,exit=control.returncode,error=control.stderr))
    (out/'verification.json').write_text(json.dumps(dict(passed=True,game_launched=False,window_opened=False,
        translated_functions=names,image_sha256=hashlib.sha256(a.image.read_bytes()).hexdigest(),
        source_sha256=hashlib.sha256(cpp.read_bytes()).hexdigest(),bridge_sha256=hashlib.sha256(header.read_bytes()).hexdigest(),
        output=result.stdout,negative=negative,
        scope='Actual TU23 translated dialog control flow and production bridge. Text allocation/localization, widget drawing, focus/selection, and footer graphics mocked. Does not prove complete native menu integration or visual layout.'),indent=2))
    print(result.stdout,end='')
if __name__=='__main__':main()
