"""Run TU23 manager publication and root lifecycle ordering without a title.

The actual translated manager, request, transition and page push/pop execute.
Device/session decisions, event-bus delivery, widget implementation and list
allocation are explicit boundary mocks. This does not prove menu rendering or
that a custom page receives real controller input.
"""
import argparse
import hashlib
import json
import re
import subprocess
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument('output', type=Path)
    parser.add_argument('--image', required=True, type=Path)
    parser.add_argument('--compiler', default='clang++')
    a = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    gen, src = root/'rexlego/generated/default', root/'rexlego/src'
    out = a.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    names = ['sub_82FD8C30', 'sub_82FEA350', 'sub_82FE3780', 'sub_82A7B0A8',
             'sub_82EFC738', 'sub_82F02940', 'sub_82F02A08',
             '__savegprlr_24', '__savegprlr_25', '__savegprlr_27', '__savegprlr_28',
             '__restgprlr_24', '__restgprlr_25', '__restgprlr_27', '__restgprlr_28']
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
    prefix = r'''
#define NOMINMAX
#include "legodimensions_pch.h"
#include "native_frontend_guest.h"
#include "native_frontend_binding.h"
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <vector>
#include <array>
#undef DEFINE_REX_FUNC
#define DEFINE_REX_FUNC(name) void name(PPCContext&ctx,uint8_t*base)
#define CHECK(x) do{if(!(x)){std::fprintf(stderr,"failed %d: %s\n",__LINE__,#x);std::exit(23);}}while(0)
using legodimensions::native_frontend::CallGuest;
using legodimensions::native_frontend::SettingsPageBinding;
constexpr uint32_t arena=0x20000000,manager=arena+0x1000,pages=arena+0x2000,
 interfaces=arena+0x3000,generic=arena+0x4000,options=arena+0x5000,
 generic_root=arena+0x6000,options_root=arena+0x7000,controller=arena+0x8000,
 old_controller=arena+0x8100,ui=arena+0x9000,layers=arena+0xA000,
 widget_vtable=arena+0xB000,session=arena+0xC000,footer=arena+0xD000,
 state=arena+0xE000;
bool ready=true,hold=false;
struct Observed { uint32_t root,event,interface_id,current,pending; };
std::vector<Observed> observed;
unsigned list_adds=0,list_removes=0,bus_events=0;
void Widget(PPCContext&ctx,uint8_t*base){
 observed.push_back({ctx.r3.u32,ctx.r4.u32,REX_LOAD_U32(generic+176),
  REX_LOAD_U32(manager+164),REX_LOAD_U32(manager+168)});
}
namespace rex::runtime { PPCFunc* ResolveIndirectFunction(uint32_t target){
 CHECK(target==0x1000);return Widget;
}}
void sub_82A36908(PPCContext&ctx,uint8_t*){ctx.r3.u32=session;}
void sub_82A465F0(PPCContext&ctx,uint8_t*){CHECK(ctx.r3.u32==session+0x100);ctx.r3.u64=ready;}
void sub_82FE35C0(PPCContext&ctx,uint8_t*){CHECK(ctx.r3.u32==manager);ctx.r3.u64=hold;}
void sub_82FD8F70(PPCContext&ctx,uint8_t*){CHECK(ctx.r3.u32==manager);}
void sub_82EFE8F0(PPCContext&,uint8_t*){}
void sub_82FBF270(PPCContext&,uint8_t*){}
void sub_82FBE2A8(PPCContext&,uint8_t*){}
void sub_82FE7B90(PPCContext&,uint8_t*){}
void sub_82A3E238(PPCContext&,uint8_t*){}
void sub_82A3E2B8(PPCContext&,uint8_t*){}
void sub_82A4C508(PPCContext&,uint8_t*){}
void sub_82A3C8B0(PPCContext&,uint8_t*){}
void sub_82A2C868(PPCContext&,uint8_t*){}
void sub_82A58570(PPCContext&,uint8_t*){++bus_events;}
void sub_82EFB300(PPCContext&ctx,uint8_t*base){
 CHECK(ctx.r3.u32==ui+0x400);++list_adds;
 const uint32_t layer=REX_LOAD_U32(ctx.r4.u32+448);CHECK(layer==2);
 REX_STORE_U32(layers+layer*12+8,REX_LOAD_U32(layers+layer*12+8)+1);
}
void sub_82EFCDA0(PPCContext&ctx,uint8_t*base){
 CHECK(ctx.r3.u32==layers+24);++list_removes;
 const uint32_t count=REX_LOAD_U32(layers+32);CHECK(count>0);
 REX_STORE_U32(layers+32,count-1);
}
'''
    body = '\n'.join(found[n] for n in names)
    defined = set(names) | set(re.findall(r'void (sub_\w+)\(PPCContext', prefix))
    called = set(re.findall(r'\b(\w+)\(ctx, base\);', body))
    mocks = '\n'.join('void '+n+'(PPCContext&,uint8_t*){CHECK(false);}' for n in sorted(called-defined))
    declarations = '\n'.join('void '+n+'(PPCContext&,uint8_t*);' for n in names)
    suffix = r'''
void Reset(uint8_t*base){
 std::memset(base+arena,0,0x100000);
 REX_STORE_U32(0x849F562C,generic);REX_STORE_U32(0x849F566C,footer);
 REX_STORE_U32(0x849F5F34,state);REX_STORE_U32(state+8,0);
 REX_STORE_U32(0x849E21CC,ui);REX_STORE_U32(ui+84,layers);REX_STORE_U32(ui+96,ui+0x400);
 REX_STORE_U32(manager+140,pages);REX_STORE_U32(manager+148,61);
 REX_STORE_U32(manager+152,interfaces);REX_STORE_U32(manager+160,61);
 for(auto off:{164u,168u,172u})REX_STORE_U32(manager+off,0xFFFFFFFF);
 REX_STORE_U8(manager+188,1);REX_STORE_U32(session,session+0x100);
 REX_STORE_U32(generic+20,generic_root);REX_STORE_U32(options+20,options_root);
 REX_STORE_U32(generic+176,old_controller);REX_STORE_U32(pages+19*4,options);
 for(auto root:{generic_root,options_root}){
  REX_STORE_U32(root,widget_vtable);REX_STORE_U32(root+448,2);
 }
 REX_STORE_U32(widget_vtable+332,0x1000);REX_STORE_U32(layers+32,1);
 ready=true;hold=false;observed.clear();list_adds=list_removes=bus_events=0;
}
int main(int argc,char**argv){
 SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
 CHECK(argc==2);auto*base=static_cast<uint8_t*>(VirtualAlloc(nullptr,0x100000000ull,MEM_RESERVE,PAGE_NOACCESS));CHECK(base);
 CHECK(VirtualAlloc(base+0x82000000,0x2B80000,MEM_COMMIT,PAGE_READWRITE));
 CHECK(VirtualAlloc(base+arena,0x100000,MEM_COMMIT,PAGE_READWRITE));
 std::ifstream image(argv[1],std::ios::binary);CHECK(image);
 image.read(reinterpret_cast<char*>(base+0x82000000),45613056);CHECK(image.gcount()==45613056);
 PPCContext ctx{};ctx.r1.u32=arena+0xF0000;ctx.lr=0x12345678;ctx.r31.u64=0x1122334455667788ull;
 unsigned count=0;
 // Every rejected registration must leave both live arrays byte-identical.
 for(unsigned fault=0;fault<22;++fault){
  Reset(base);SettingsPageBinding binding;
  switch(fault){
   case 0:REX_STORE_U32(manager+148,23);break;
   case 1:REX_STORE_U32(manager+160,23);break;
   case 2:REX_STORE_U32(manager+140,0);break;
   case 3:REX_STORE_U32(manager+152,0xFFFFFFF0);break;
   default:
    if(fault<10)REX_STORE_U32((fault<7?pages:interfaces)+(21+(fault-4)%3)*4,0xBAD);
    else if(fault<19)REX_STORE_U32(manager+164+((fault-10)/3)*4,21+(fault-10)%3);
  }
  std::array<uint8_t,8192> before;std::memcpy(before.data(),base+pages,before.size());
  uint32_t m=fault==19?0:manager,c=fault==20?0:generic,i=fault==21?0:controller;
  CHECK(!binding.Install(base,m,c,i));CHECK(std::memcmp(before.data(),base+pages,before.size())==0);++count;
 }
 // Valid install preserves unrelated entries, and ownership guards release.
 Reset(base);SettingsPageBinding binding;CHECK(binding.Install(base,manager,generic,controller));
 CHECK(binding.Owns(base,manager));CHECK(!binding.Install(base,manager,generic,controller));
 CHECK(REX_LOAD_U32(pages+19*4)==options&&REX_LOAD_U32(pages+24*4)==0);++count;
 for(auto off:{164u,168u,172u})for(auto id:{21u,22u,23u}){
  REX_STORE_U32(manager+off,id);CHECK(!binding.Release(base,manager));CHECK(binding.Owns(base,manager));
  REX_STORE_U32(manager+off,0xFFFFFFFF);++count;
 }
 REX_STORE_U32(interfaces+22*4,old_controller);CHECK(!binding.Owns(base,manager));
 CHECK(!binding.Release(base,manager));CHECK(REX_LOAD_U32(interfaces+22*4)==old_controller);++count;
 REX_STORE_U32(interfaces+22*4,controller);REX_STORE_U32(manager+140,pages+256);
 CHECK(!binding.Owns(base,manager)&&!binding.Release(base,manager));++count;
 REX_STORE_U32(manager+140,pages);CHECK(binding.Release(base,manager));
 CHECK(!binding.Owns(base,manager));for(auto id:{21u,22u,23u})CHECK(REX_LOAD_U32(pages+id*4)==0&&REX_LOAD_U32(interfaces+id*4)==0);++count;
 // Actual translated request/frame/transition and push/pop. Publish precedes
 // old-root event 4, even when old and incoming IDs share GenericPage.
 for(auto previous:{19u,21u})for(auto next:{21u,22u,23u})for(bool deferred:{false,true}){
  Reset(base);SettingsPageBinding b;CHECK(b.Install(base,manager,generic,controller));
  REX_STORE_U32(manager+164,previous);ready=!deferred;
  const auto saved=ctx;CallGuest(sub_82FD8C30,ctx,base,manager,next);
  CHECK(std::memcmp(&saved,&ctx,sizeof(ctx))==0);
  CHECK(REX_LOAD_U32(manager+(deferred?172:168))==next);ready=true;
  CallGuest(sub_82FEA350,ctx,base,manager,arena+0xF100);
  CHECK(std::memcmp(&saved,&ctx,sizeof(ctx))==0);
  CHECK(REX_LOAD_U32(manager+164)==next&&REX_LOAD_U32(manager+168)==0xFFFFFFFF);
  CHECK(REX_LOAD_U32(manager+172)==0xFFFFFFFF&&REX_LOAD_U32(manager+184)==0);
  CHECK(REX_LOAD_U32(generic+176)==controller);CHECK(observed.size()==3);
  CHECK(observed[0].root==(previous==19?options_root:generic_root)&&observed[0].event==4);
  CHECK(observed[1].root==generic_root&&observed[1].event==2);
  CHECK(observed[2].root==generic_root&&observed[2].event==3);
  for(const auto&o:observed)CHECK(o.interface_id==controller&&o.current==next&&o.pending==0xFFFFFFFF);
  CHECK(list_adds==1&&list_removes==1&&REX_LOAD_U32(layers+32)==1);
  CHECK(!b.Release(base,manager));++count;
 }
 // A pending request must not be displaced; a readiness gate must not pop.
 Reset(base);SettingsPageBinding b;CHECK(b.Install(base,manager,generic,controller));
 REX_STORE_U32(manager+168,7);REX_STORE_U32(manager+184,options);
 CallGuest(sub_82FD8C30,ctx,base,manager,21);
 CHECK(REX_LOAD_U32(manager+168)==7&&REX_LOAD_U32(manager+184)==options);++count;
 Reset(base);SettingsPageBinding c;CHECK(c.Install(base,manager,generic,controller));
 REX_STORE_U32(manager+164,19);CallGuest(sub_82FD8C30,ctx,base,manager,22);
 REX_STORE_U8(manager+188,0);CallGuest(sub_82FEA350,ctx,base,manager,arena+0xF100);
 CHECK(observed.empty()&&list_adds==0&&list_removes==0&&REX_LOAD_U32(manager+168)==22);++count;
 VirtualFree(base,0,MEM_RELEASE);
 std::printf("PASS %u native frontend registration/lifecycle cases; incoming interface precedes old-root removal; actual manager/request/push/pop, full PPC context restoration\n",count);
}
'''
    header = (src/'native_frontend_binding.h').read_text()

    def run(label, translated=body, binding=header):
        inc = out/label
        inc.mkdir(exist_ok=True)
        (inc/'native_frontend_binding.h').write_text(binding)
        source, exe = inc/'fixture.cpp', inc/'fixture.exe'
        source.write_text(prefix+declarations+mocks+translated+suffix)
        cmd = [a.compiler, '-std=c++23', '-O2', '-mavx2', '-D_CRT_SECURE_NO_WARNINGS',
               '-DSPDLOG_FMT_EXTERNAL', '-Wno-unused-value', '-I'+str(inc),
               '-I'+str(gen), '-I'+str(src), '-I'+str(root.parent/'dev-assets/sdk-install/include'),
               str(source), '-o', str(exe)]
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
        (inc/'compile.log').write_text(result.stdout+result.stderr)
        result.check_returncode()
        result = subprocess.run([str(exe), str(a.image.resolve())], capture_output=True, text=True, timeout=15)
        (inc/'run.log').write_text(result.stdout+result.stderr)
        return result

    positive = run('production')
    positive.check_returncode()
    print(positive.stdout, end='')
    negative = []
    changes = [
        ('wrong-interface-index', body, 'REX_LOAD_U32(ctx.r8.u32 + ctx.r9.u32);',
         'REX_LOAD_U32(ctx.r8.u32 + ctx.r9.u32 + 4);', False),
        ('skip-interface-publication', body, 'REX_STORE_U32(ctx.r11.u32 + 176, ctx.r7.u32);',
         '(void)0;', False),
        ('overwrite-occupied-slots', header,
         'if (Load(base, pages + id * 4) || Load(base, interfaces + id * 4)) return Availability::kOccupied;',
         'if (false) return Availability::kOccupied;', True),
        ('clear-active-controller', header,
         'if (!Owns(base, manager) || Busy(base, manager)) return false;',
         'if (!Owns(base, manager)) return false;', True),
    ]
    for label, text, old, new, is_header in changes:
        assert text.count(old) == 1, (label, text.count(old))
        changed = text.replace(old, new)
        result = run(label, binding=changed) if is_header else run(label, translated=changed)
        assert result.returncode == 23, (label, result.returncode, result.stderr)
        negative.append(dict(name=label, exit=result.returncode, error=result.stderr))
    (out/'verification.json').write_text(json.dumps(dict(
        passed=True, game_launched=False, output=positive.stdout,
        image_sha256=hashlib.sha256(a.image.read_bytes()).hexdigest(),
        source_sha256={str(src/'native_frontend_binding.h'):hashlib.sha256(header.encode()).hexdigest()},
        translated_sha256={n:hashlib.sha256(found[n].encode()).hexdigest() for n in names},
        negative=negative,
        scope=__doc__,
        finding='Frame publishes the incoming GenericPage interface before old-root event 4. A shared-component controller must not infer outgoing ownership from its current interface. Actual event-bus delivery and component routing are not executed here.',
    ), indent=2))


if __name__ == '__main__':
    main()
