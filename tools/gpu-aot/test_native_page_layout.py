"""Check the production page bridge with translated TU23 widget construction routing."""
import argparse,hashlib,json,re,subprocess
from pathlib import Path

def main():
    p=argparse.ArgumentParser(__doc__);p.add_argument('output',type=Path);p.add_argument('--image',type=Path,required=True);p.add_argument('--compiler',default='clang++');a=p.parse_args()
    root=Path(__file__).resolve().parents[2];gen=root/'rexlego/generated/default';src=root/'rexlego/src';out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
    names=['sub_82F0FAC0','sub_82F01880','sub_82EFA438','sub_82EF63A0','sub_82F03BB8','sub_82F03C40','sub_82EF9730','sub_82F0B420']
    for n in [14,25,28,29]:names += ['__savegprlr_'+str(n),'__restgprlr_'+str(n)]
    found={}
    for f in gen.glob('legodimensions_recomp.*.cpp'):
        text=f.read_text()
        for name in names:
            s=text.find('DEFINE_REX_FUNC('+name+')')
            if s>=0:
                e=text.find('\nDEFINE_REX_FUNC(',s+1);found[name]=text[s:e if e>=0 else len(text)]
    assert set(found)==set(names),set(names)-found.keys()
    prefix=r'''
#define NOMINMAX
#include "legodimensions_pch.h"
#include "native_frontend_layout.h"
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <vector>
#undef DEFINE_REX_FUNC
#define DEFINE_REX_FUNC(name) void name(PPCContext&ctx,uint8_t*base)
#define CHECK(x) do{if(!(x)){std::fprintf(stderr,"failed %d: %s\n",__LINE__,#x);std::exit(23);}}while(0)
using namespace legodimensions::native_frontend;
constexpr uint32_t arena=0x20000000,registry=arena+0x1000,text_registry=arena+0x2000,
 page_node=arena+0x3000,elements_node=arena+0x4000,text1_node=arena+0x5000,text2_node=arena+0x6000,
 xml=arena+0x7000,page_name=arena+0x8000,text1_name=arena+0x8100,text2_name=arena+0x8200,
 foreign_name=arena+0x8300,missing_name=arena+0x8400,foreign_page=arena+0x9000,vtable=arena+0xA000;
uint32_t document=0,next_widget=arena+0x20000,pages_created=0,texts_created=0,parses=0;
bool deny_root=false;
unsigned steps=0;
void Step(const char*op,uint32_t a,uint32_t b=0){if(++steps>10000){std::fprintf(stderr,"fixture budget: %s %08X %08X\n",op,a,b);CHECK(false);}}
std::map<uint32_t,uint32_t> widget_names;
void Name(PPCContext&ctx,uint8_t*){Step("name",ctx.r3.u32);CHECK(widget_names.contains(ctx.r3.u32));ctx.r3.u32=widget_names.at(ctx.r3.u32);}
void sub_82CE5308(PPCContext&ctx,uint8_t*){document=ctx.r3.u32;}
void sub_82CE77C0(PPCContext&ctx,uint8_t*base){CHECK(ctx.r3.u32==document&&ctx.r4.u32==xml);++parses;ctx.r3.u32=1;}
void sub_82CE2C20(PPCContext&ctx,uint8_t*base){Step("first",ctx.r3.u32,document);ctx.r3.u32=ctx.r3.u32==document?page_node:REX_LOAD_U32(ctx.r3.u32);}
void sub_82CE2D38(PPCContext&ctx,uint8_t*base){Step("next",ctx.r3.u32);ctx.r3.u32=REX_LOAD_U32(ctx.r3.u32+4);}
void sub_82A74C50(PPCContext&ctx,uint8_t*){CHECK(ctx.r3.u32==document);}
void sub_82A296D8(PPCContext&ctx,uint8_t*base){Step("compare",ctx.r3.u32,ctx.r4.u32);ctx.r3.s64=std::strcmp(reinterpret_cast<char*>(base+ctx.r3.u32),reinterpret_cast<char*>(base+ctx.r4.u32));}
void sub_82F021F0(PPCContext&ctx,uint8_t*){Step("filter",ctx.r3.u32);ctx.r3.u32=!(deny_root&&ctx.r3.u32==page_node);}
void sub_82CE3F90(PPCContext&ctx,uint8_t*base){const auto node=ctx.r3.u32;const auto*attribute=reinterpret_cast<const char*>(base+ctx.r4.u32);
 if(std::strcmp(attribute,"name")==0){CHECK(node==page_node||node==text1_node||node==text2_node);ctx.r3.u32=node==page_node?page_name:node==text1_node?text1_name:text2_name;}
 else ctx.r3.u32=0;
}
void sub_82784740(PPCContext&ctx,uint8_t*base){REX_STORE_U32(ctx.r3.u32,ctx.r4.u32);}
void sub_82F02270(PPCContext&ctx,uint8_t*base){const auto factory=ctx.r3.u32,node=ctx.r4.u32,parent=ctx.r5.u32;
 const auto created=next_widget;next_widget+=0x1000;REX_STORE_U32(created,vtable);REX_STORE_U32(created+436,0);
 if(factory==0x11111111){CHECK(node==page_node&&parent==0);++pages_created;widget_names[created]=page_name;
  REX_STORE_U32(created+32,REX_LOAD_U32(registry+24));REX_STORE_U32(registry+24,created);
 }else{CHECK(factory==0x22222222&&(node==text1_node||node==text2_node));CHECK(parent&&widget_names.at(parent)==page_name);
  ++texts_created;widget_names[created]=node==text1_node?text1_name:text2_name;REX_STORE_U32(created+8,parent);
 }ctx.r3.u32=created;
}
'''
    body='\n'.join(found[n] for n in names)
    custom=set(re.findall(r'void (sub_\w+)\(PPCContext',prefix));called=set(re.findall(r'\b(\w+)\(ctx, base\);',body))
    mocks='\n'.join('void '+name+'(PPCContext&,uint8_t*){CHECK(false);}' for name in sorted(called-set(names)-custom))
    declarations='\n'.join('void '+name+'(PPCContext&,uint8_t*);' for name in names)
    resolver=r'''
namespace rex::runtime {PPCFunc* ResolveIndirectFunction(uint32_t target){
 switch(target){case 0x82F021F0:return sub_82F021F0;case 0x82F0B420:return sub_82F0B420;
 case 0x82F03BB8:return sub_82F03BB8;case 0x82F03C40:return sub_82F03C40;case 0x1000:return Name;
 default:std::fprintf(stderr,"unexpected native target %08X\n",target);CHECK(false);return nullptr;}}}
'''
    suffix=r'''
int main(int argc,char**argv){
 SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
 SetUnhandledExceptionFilter([](EXCEPTION_POINTERS*e)->LONG{std::fprintf(stderr,"exception %08lX offset %llX access %llu address %llX\n",e->ExceptionRecord->ExceptionCode,
  static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(e->ExceptionRecord->ExceptionAddress)-reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr))),
  static_cast<unsigned long long>(e->ExceptionRecord->ExceptionInformation[0]),static_cast<unsigned long long>(e->ExceptionRecord->ExceptionInformation[1]));return EXCEPTION_EXECUTE_HANDLER;});
 CHECK(argc==3);auto*base=static_cast<uint8_t*>(VirtualAlloc(nullptr,0x100000000ull,MEM_RESERVE,PAGE_NOACCESS));CHECK(base);
 CHECK(VirtualAlloc(base+0x82000000,0x2B80000,MEM_COMMIT,PAGE_READWRITE));CHECK(VirtualAlloc(base+arena,0x100000,MEM_COMMIT,PAGE_READWRITE));
 CHECK(VirtualAlloc(base+REX_IMAGE_BASE+REX_IMAGE_SIZE,(REX_CODE_SIZE+REX_THUNK_RESERVE_SIZE)*2,MEM_COMMIT,PAGE_READWRITE));
 if(std::strcmp(argv[2],"table")==0){
  for(auto [target,fn]:std::vector<std::pair<uint32_t,PPCFunc*>>{{0x82F021F0,sub_82F021F0},{0x82F0B420,sub_82F0B420},{0x82F03BB8,sub_82F03BB8},{0x82F03C40,sub_82F03C40}})
   REX_LOOKUP_FUNC(base,target)=fn;
 }
 std::ifstream image(argv[1],std::ios::binary);CHECK(image);image.read(reinterpret_cast<char*>(base+0x82000000),45613056);CHECK(image.gcount()==45613056);
 REX_STORE_U32(0x849E220C,registry);REX_STORE_U32(0x849E2200,text_registry);REX_STORE_U32(registry+4,0x11111111);REX_STORE_U32(text_registry+4,0x22222222);
 REX_STORE_U8(uint32_t(-2070872064LL-25320+944),0);
 std::strcpy(reinterpret_cast<char*>(base+xml),"<page name=\"native_video\"><uielements><text name=\"fps\"/><text name=\"vsync\"/></uielements></page>");
 std::strcpy(reinterpret_cast<char*>(base+page_name),"native_video");std::strcpy(reinterpret_cast<char*>(base+text1_name),"fps");std::strcpy(reinterpret_cast<char*>(base+text2_name),"vsync");
 std::strcpy(reinterpret_cast<char*>(base+foreign_name),"some_mod_page");std::strcpy(reinterpret_cast<char*>(base+missing_name),"missing");
 REX_STORE_U32(vtable+56,0x1000);REX_STORE_U32(foreign_page,vtable);widget_names[foreign_page]=foreign_name;REX_STORE_U32(registry+24,foreign_page);
 auto node=[&](uint32_t address,const char*tag,uint32_t child,uint32_t next){REX_STORE_U32(address,child);REX_STORE_U32(address+4,next);REX_STORE_U32(address+32,address+0x100);std::strcpy(reinterpret_cast<char*>(base+address+0x108),tag);};
 node(page_node,"page",elements_node,0);node(elements_node,"uielements",text1_node,0);node(text1_node,"text",0,text2_node);node(text2_node,"text",0,0);
 PPCContext ctx{};ctx.r1.u32=arena+0xF0000;ctx.r31.u64=0x8899AABBCCDDEEFFull;ctx.lr=0x12345678;const auto saved=ctx;
 PageLayoutRequest request{registry,xml,page_name};auto load=[&](const auto&r){auto result=LoadPageLayout(sub_82F0FAC0,sub_82EF9730,ctx,base,r);CHECK(std::memcmp(&ctx,&saved,sizeof(ctx))==0);return result;};
 auto created=load(request);CHECK(created.status==PageLoadStatus::kCreated&&created.page&&pages_created==1&&texts_created==2&&parses==1);
 CHECK(REX_LOAD_U32(created.page+32)==foreign_page&&widget_names.at(foreign_page)==foreign_name);
 auto existing=load(request);CHECK(existing.status==PageLoadStatus::kExisting&&existing.page==created.page&&parses==1&&texts_created==2);
 auto collision=request;collision.name=foreign_name;auto preserved=load(collision);CHECK(preserved.status==PageLoadStatus::kExisting&&preserved.page==foreign_page&&parses==1);
 auto absent=request;absent.name=missing_name;deny_root=true;auto declined=load(absent);CHECK(declined.status==PageLoadStatus::kNotCreated&&declined.page==0&&parses==2&&texts_created==2);
 for(unsigned bad=0;bad<4;++bad){auto invalid=request;if(bad==0)invalid.registry=0;if(bad==1)invalid.xml=0;if(bad==2)invalid.name=0;if(bad==3)invalid.registry=0xFFFFFFE5;
  CHECK(load(invalid).status==PageLoadStatus::kInvalid&&parses==2);}
 VirtualFree(base,0,MEM_RELEASE);
 std::puts("PASS: translated buffer loader/tag dispatch/native page lookup and page/text factory routing; parent propagation, existing-name preservation, denied root, invalid requests and PPC context restoration");
}
'''
    header=src/'native_frontend_layout.h'
    def run(label,include=None):
        cpp=out/(label+'.cpp');cpp.write_text(prefix+declarations+mocks+resolver+body+suffix);exe=out/(label+'.exe')
        cmd=[a.compiler,'-std=c++23','-O2','-mavx2','-D_CRT_SECURE_NO_WARNINGS','-DSPDLOG_FMT_EXTERNAL']
        if include:cmd+=['-I'+str(include)]
        cmd+=['-I'+str(src),'-I'+str(gen),'-I'+str(root.parent/'dev-assets/sdk-install/include'),str(cpp),'-Wl,/map:'+str(out/(label+'.map')),'-o',str(exe)]
        result=subprocess.run(cmd,capture_output=True,text=True,timeout=70);(out/(label+'-compile.log')).write_text(result.stdout+result.stderr);result.check_returncode()
        try:result=subprocess.run([str(exe),str(a.image.resolve()),'table'],capture_output=True,text=True,timeout=15)
        except subprocess.TimeoutExpired as e:
            def decoded(value):return value.decode(errors='replace') if isinstance(value,bytes) else value or ''
            (out/(label+'.log')).write_text(decoded(e.stdout)+decoded(e.stderr));raise
        (out/(label+'.log')).write_text(result.stdout+result.stderr)
        if label=='production' and result.returncode==0:
            fallback=subprocess.run([str(exe),str(a.image.resolve()),'fallback'],capture_output=True,text=True,timeout=15)
            (out/'production-fallback.log').write_text(fallback.stdout+fallback.stderr);fallback.check_returncode()
            result.stdout+='Fallback dispatch: '+fallback.stdout
        return result
    positive=run('production');positive.check_returncode();print(positive.stdout,end='');negative=[];text=header.read_text()
    for label,old,new in [('wrong-file-route','CallGuest(load, ctx, base, 0, 0, request.xml);','CallGuest(load, ctx, base, 0, request.xml, 0);'),
                          ('wrong-registry-list','request.registry + 24;','request.registry + 28;'),
                          ('overwrite-existing','if (const auto existing = CallGuest(find, ctx, base, list, request.name))\n    return {PageLoadStatus::kExisting, existing};','if (false) return {PageLoadStatus::kExisting, 0};')]:
        assert text.count(old)==1,label;folder=out/label;folder.mkdir(exist_ok=True);(folder/header.name).write_text(text.replace(old,new))
        result=run(label,folder);assert result.returncode==23,(label,result.returncode,result.stdout,result.stderr);negative.append(dict(name=label,exit=result.returncode,error=result.stderr))
    (out/'verification.json').write_text(json.dumps(dict(passed=True,game_launched=False,window_opened=False,image_sha256=hashlib.sha256(a.image.read_bytes()).hexdigest(),
        header_sha256=hashlib.sha256(header.read_bytes()).hexdigest(),translated_sha256={n:hashlib.sha256(found[n].encode()).hexdigest() for n in names},negative=negative,output=positive.stdout,
        scope='Actual translated widget pass, XML handler dispatch, native page reuse, text constructor attributes and linked-name lookup. TinyXML parsing, generic GSC widget allocation, strings, platform node filter and vtable name access are boundary mocks. Does not prove actual resource allocation, native page/component registration, navigation, rendering or visual layout.'),indent=2))
if __name__=='__main__':main()
