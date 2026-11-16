"""Execute native component registration, page binding and event dispatch offline.

Real TU23 base constructor, intrusive registration/name lookup, bind-all pass,
page dispatcher and selection-change dispatcher execute. Widget children,
navigation/focus implementation, drawing and global event-bus delivery are
boundary mocks. No title, launcher, window or graphics device is started.
"""
import argparse
import hashlib
import json
import re
import subprocess
from pathlib import Path


def main():
    p = argparse.ArgumentParser(__doc__)
    p.add_argument('output', type=Path)
    p.add_argument('--image', required=True, type=Path)
    p.add_argument('--compiler', default='clang++')
    a = p.parse_args()
    root = Path(__file__).resolve().parents[2]
    gen, src = root/'rexlego/generated/default', root/'rexlego/src'
    out = a.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    names = ['sub_82EFB478', 'sub_82EFB1E8', 'sub_82EF8D80', 'sub_82F053C8',
             'sub_82F0FF38', 'sub_82EF9850', '__savegprlr_28', '__restgprlr_28',
             '__savegprlr_29', '__restgprlr_29']
    found = {}
    for f in gen.glob('legodimensions_recomp.*.cpp'):
        text = f.read_text()
        for name in names:
            start = text.find('DEFINE_REX_FUNC('+name+')')
            if start < 0:
                continue
            end = text.find('\nDEFINE_REX_FUNC(', start+1)
            found[name] = text[start:end if end >= 0 else len(text)]
    assert set(found) == set(names)
    body = '\n'.join(found[n] for n in names)
    prefix = r'''
#define NOMINMAX
#include "legodimensions_pch.h"
#include "native_frontend_component.h"
#include "native_frontend_binding.h"
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <vector>
#undef DEFINE_REX_FUNC
#define DEFINE_REX_FUNC(name) void name(PPCContext&ctx,uint8_t*base)
#define CHECK(x) do{if(!(x)){std::fprintf(stderr,"failed %d: %s\n",__LINE__,#x);std::exit(23);}}while(0)
using namespace legodimensions::native_frontend;
constexpr uint32_t arena=0x20000000,ui=arena+0x1000,list=arena+0x2000,
 compA=arena+0x3000,compB=arena+0x3100,vtA=arena+0x4000,vtB=arena+0x4100,
 pageA=arena+0x5000,pageB=arena+0x6000,widgetVT=arena+0x7000,
 nameA=arena+0x8000,nameB=arena+0x8100,registry=arena+0x9000,
 manager=arena+0xA000,pages=arena+0xB000,interfaces=arena+0xC000;
struct Event {uint32_t component,source,event,payload;};
std::vector<Event> events;unsigned native_binder_finalizes=0;
void Controller(PPCContext&ctx,uint8_t*){
 CHECK(ctx.r3.u32==compA||ctx.r3.u32==compB);
 events.push_back({ctx.r3.u32,ctx.r4.u32,ctx.r5.u32,ctx.r6.u32});
}
void Name(PPCContext&ctx,uint8_t*){CHECK(ctx.r3.u32==pageB);ctx.r3.u32=nameB;}
namespace rex::runtime {PPCFunc* ResolveIndirectFunction(uint32_t target){
 switch(target){case 0x1000:return Controller;case 0x1004:return Name;default:CHECK(false);return nullptr;}
}}
void sub_82A2F6C8(PPCContext&ctx,uint8_t*base){ctx.r3.s64=std::strcmp(reinterpret_cast<char*>(base+ctx.r3.u32),reinterpret_cast<char*>(base+ctx.r4.u32));}
void sub_82F04DD8(PPCContext&,uint8_t*){++native_binder_finalizes;}
void sub_82F0D228(PPCContext&,uint8_t*){}
void sub_82F0FE08(PPCContext&,uint8_t*){}
void sub_82F0D508(PPCContext&,uint8_t*){}
void sub_82EFCB70(PPCContext&,uint8_t*){}
void sub_82A58570(PPCContext&,uint8_t*){}
'''
    definitions = set(names) | set(re.findall(r'void (sub_\w+)\(PPCContext', prefix))
    called = set(re.findall(r'\b(\w+)\(ctx, base\);', body))
    mocks = '\n'.join('void '+n+'(PPCContext&,uint8_t*){CHECK(false);}' for n in sorted(called-definitions))
    declarations = '\n'.join('void '+n+'(PPCContext&,uint8_t*);' for n in names)
    suffix = r'''
ComponentRequest Request(uint32_t component=compA,uint32_t vt=vtA,uint32_t name=nameA){
 return {component,vt,name,{0x1008,0x1000,0x1008,0x1008,0x1008},19};
}
void Reset(uint8_t*base){
 std::memset(base+arena,0,0x100000);events.clear();native_binder_finalizes=0;
 REX_STORE_U32(ui+104,list);REX_STORE_U32(0x849E21CC,ui);REX_STORE_U32(0x849E220C,registry);
 REX_STORE_U32(pageA,widgetVT);REX_STORE_U32(pageB,widgetVT);
 REX_STORE_U32(widgetVT+332,0x82F0FF38);REX_STORE_U32(widgetVT+56,0x1004);
 std::strcpy(reinterpret_cast<char*>(base+nameA),"NativeVideoPage");
 std::strcpy(reinterpret_cast<char*>(base+nameB),"NativeEffectsPage");
}
int main(int argc,char**argv){
 SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);CHECK(argc==2);
 auto*base=static_cast<uint8_t*>(VirtualAlloc(nullptr,0x100000000ull,MEM_RESERVE,PAGE_NOACCESS));CHECK(base);
 CHECK(VirtualAlloc(base+0x82000000,0x2B80000,MEM_COMMIT,PAGE_READWRITE));
 CHECK(VirtualAlloc(base+arena,0x100000,MEM_COMMIT,PAGE_READWRITE));
 CHECK(VirtualAlloc(base+REX_IMAGE_BASE+REX_IMAGE_SIZE,(uint64_t(REX_CODE_SIZE)+REX_THUNK_RESERVE_SIZE)*2,MEM_COMMIT,PAGE_READWRITE));
 REX_LOOKUP_FUNC(base,0x82F0FF38)=sub_82F0FF38;
 std::ifstream image(argv[1],std::ios::binary);CHECK(image);image.read(reinterpret_cast<char*>(base+0x82000000),45613056);CHECK(image.gcount()==45613056);
 PPCContext ctx{};ctx.r1.u32=arena+0xF0000;ctx.r31.u64=0x123456789ABCDEF0ull;ctx.lr=0x13579;
 const auto saved=ctx;unsigned count=0;
 // Reject incomplete/aliased/occupied owned storage before invoking native ctor.
 for(unsigned fault=0;fault<11;++fault){
  Reset(base);auto r=Request();
  switch(fault){case 0:r.component=0;break;case 1:r.vtable=0;break;case 2:r.name=0;break;
   case 3:r.callbacks[2]=0;break;case 4:r.vtable=compA+24;break;case 5:r.component=UINT32_MAX-8;break;
   case 6:r.vtable=UINT32_MAX-8;break;case 7:base[compA+27]=1;break;
   case 8:base[vtA+19]=1;break;case 9:r.component=vtA+16;break;case 10:r.vtable=compA-16;break;}
  std::vector<uint8_t> before(base+arena,base+arena+0x10000);
  CHECK(!InitializeComponent(sub_82EFB478,ctx,base,r));
  CHECK(std::memcmp(before.data(),base+arena,before.size())==0);++count;
 }
 Reset(base);CHECK(InitializeComponent(sub_82EFB478,ctx,base,Request()));
 CHECK(REX_LOAD_U32(compA)==vtA&&REX_LOAD_U32(compA+16)==nameA&&REX_LOAD_U32(compA+20)==0);
 CHECK(REX_LOAD_U8(compA+12)==1&&REX_LOAD_U32(compA+24)==19);++count;
 CHECK(AttachComponentPage(sub_82EFB1E8,sub_82EF8D80,ctx,base,ui,compA,pageA));
 CHECK(REX_LOAD_U32(list)==compA&&REX_LOAD_U32(list+4)==compA&&REX_LOAD_U32(list+8)==1);
 CHECK(REX_LOAD_U32(pageA+476)==compA&&REX_LOAD_U32(compA+20)==pageA&&events.empty());++count;
 CHECK(!AttachComponentPage(sub_82EFB1E8,sub_82EF8D80,ctx,base,ui,compA,pageA));++count;
 // Compare the bridge's reciprocal root binding against the real bind-all
 // implementation for a separately registered page, without resetting A.
 CHECK(InitializeComponent(sub_82EFB478,ctx,base,Request(compB,vtB,nameB)));
 CallGuest(sub_82EFB1E8,ctx,base,ui,compB);
 CHECK(REX_LOAD_U32(compA+8)==compB&&REX_LOAD_U32(compB+4)==compA&&REX_LOAD_U32(list+8)==2);
 REX_STORE_U32(registry+24,pageB);CallGuest(sub_82F053C8,ctx,base,ui);
 CHECK(REX_LOAD_U32(pageB+476)==compB&&REX_LOAD_U32(compB+20)==pageB);
 CHECK(events.size()==1&&events[0].component==compB&&events[0].source==pageB&&events[0].event==1);
 CHECK(native_binder_finalizes==1);++count;
 // Real page dispatcher reaches each owning component with the native ABI.
 for(auto page:{pageA,pageB})for(auto event:{1u,2u,3u,4u,5u,9u,10u,11u,12u,15u,16u,18u}){
  events.clear();CallGuest(sub_82F0FF38,ctx,base,page,event,arena+0xE000);
  CHECK(events.size()==1&&events[0].component==(page==pageA?compA:compB));
  CHECK(events[0].source==page&&events[0].event==event&&events[0].payload==arena+0xE000);
  CHECK(std::memcmp(&saved,&ctx,sizeof(ctx))==0);++count;
 }
 // Existing title selection-change delivery is event 22, not Accept.
 for(auto page:{pageA,pageB}){
  events.clear();CallGuest(sub_82EF9850,ctx,base,page,arena+0xD000);
  CHECK(REX_LOAD_U32(page+484)==arena+0xD000);
  CHECK(events.size()==1&&events[0].component==(page==pageA?compA:compB)&&events[0].source==page&&events[0].event==22&&events[0].payload==0);++count;
 }
 // Fail atomically on an already owned page, registered name or linked object.
 for(unsigned fault=0;fault<6;++fault){
  Reset(base);CHECK(InitializeComponent(sub_82EFB478,ctx,base,Request()));
  CHECK(InitializeComponent(sub_82EFB478,ctx,base,Request(compB,vtB,nameB)));
  switch(fault){case 0:REX_STORE_U32(pageA+476,compB);break;case 1:REX_STORE_U32(compA+20,pageB);break;
   case 2:REX_STORE_U32(compA+4,compB);break;case 3:REX_STORE_U32(compA+8,compB);break;
   case 4:REX_STORE_U32(compB+16,nameA);CallGuest(sub_82EFB1E8,ctx,base,ui,compB);break;
   case 5:REX_STORE_U32(ui+104,0);break;}
  std::vector<uint8_t> before(base+arena,base+arena+0x10000);
  CHECK(!AttachComponentPage(sub_82EFB1E8,sub_82EF8D80,ctx,base,ui,compA,pageA));
  CHECK(std::memcmp(before.data(),base+arena,before.size())==0);++count;
 }
 // Distinct Video/Effects/Confirm components retain their individual identities.
 Reset(base);REX_STORE_U32(manager+140,pages);REX_STORE_U32(manager+148,61);
 REX_STORE_U32(manager+152,interfaces);REX_STORE_U32(manager+160,61);
 for(auto off:{164u,168u,172u})REX_STORE_U32(manager+off,UINT32_MAX);
 SettingsPageBinding binding;CHECK(binding.Install(base,manager,{compA,compB,compB+128},{vtA,vtB,vtB+128}));
 CHECK(binding.Owns(base,manager));CHECK(REX_LOAD_U32(pages+84)==compA&&REX_LOAD_U32(pages+88)==compB&&REX_LOAD_U32(pages+92)==compB+128);
 CHECK(REX_LOAD_U32(interfaces+84)==vtA&&REX_LOAD_U32(interfaces+88)==vtB&&REX_LOAD_U32(interfaces+92)==vtB+128);
 CHECK(binding.Release(base,manager));++count;
 CHECK(std::memcmp(&saved,&ctx,sizeof(ctx))==0);VirtualFree(base,0,MEM_RELEASE);
 std::printf("PASS %u native component construction/registration/binding/event cases; distinct page ownership; real native component callback ABI and selection event 22\n",count);
}
'''
    headers = {name:(src/name).read_text() for name in ['native_frontend_component.h', 'native_frontend_binding.h']}

    def run(label, translated=body, component_header=headers['native_frontend_component.h']):
        inc = out/label
        inc.mkdir(exist_ok=True)
        for name, text in headers.items():
            (inc/name).write_text(component_header if name == 'native_frontend_component.h' else text)
        source, exe = inc/'fixture.cpp', inc/'fixture.exe'
        source.write_text(prefix+declarations+mocks+translated+suffix)
        cmd = [a.compiler, '-std=c++23', '-O2', '-mavx2', '-D_CRT_SECURE_NO_WARNINGS',
               '-DSPDLOG_FMT_EXTERNAL', '-Wno-unused-value', '-I'+str(inc), '-I'+str(gen),
               '-I'+str(src), '-I'+str(root.parent/'dev-assets/sdk-install/include'), str(source), '-o', str(exe)]
        result = subprocess.run(cmd, text=True, capture_output=True, timeout=60)
        (inc/'compile.log').write_text(result.stdout+result.stderr)
        result.check_returncode()
        result = subprocess.run([str(exe), str(a.image.resolve())], text=True, capture_output=True, timeout=15)
        (inc/'run.log').write_text(result.stdout+result.stderr)
        return result

    positive = run('production')
    positive.check_returncode()
    print(positive.stdout, end='')
    negative = []
    changes = [
        ('wrong-component-event-slot', body, 'REX_LOAD_U32(ctx.r11.u32 + 4);', 'REX_LOAD_U32(ctx.r11.u32 + 8);', False),
        ('wrong-event-payload-register', body, 'ctx.r6.u64 = ctx.r29.u64;', 'ctx.r6.u64 = 0;', False),
        ('omit-reciprocal-root', headers['native_frontend_component.h'], 'Store(base, component + 20, page);', '(void)page;', True),
        ('ignore-page-owner', headers['native_frontend_component.h'], 'Load(base, component + 20) || Load(base, page + 476)', 'Load(base, component + 20)', True),
    ]
    for label, text, old, new, is_header in changes:
        # The native page dispatcher and selection helper both use event slot
        # +4; changing both must still be detected by a distinct fixture thunk.
        assert text.count(old) >= 1, (label, text.count(old))
        changed = text.replace(old, new)
        result = run(label, component_header=changed) if is_header else run(label, translated=changed)
        assert result.returncode == 23, (label, result.returncode, result.stderr)
        negative.append(dict(name=label, exit=result.returncode, error=result.stderr))
    (out/'verification.json').write_text(json.dumps(dict(passed=True, game_launched=False,
        output=positive.stdout, source_sha256={str(src/n):hashlib.sha256(t.encode()).hexdigest() for n,t in headers.items()},
        image_sha256=hashlib.sha256(a.image.read_bytes()).hexdigest(),
        translated_sha256={n:hashlib.sha256(found[n].encode()).hexdigest() for n in names},
        negative=negative, scope=__doc__), indent=2))


if __name__ == '__main__':
    main()
