"""Execute real TU23 Options Video/Effects transitions without launching a title."""
import argparse,hashlib,json,re,subprocess
from pathlib import Path

def main():
    p=argparse.ArgumentParser(__doc__);p.add_argument('output',type=Path);p.add_argument('--image',type=Path,required=True);p.add_argument('--compiler',default='clang++');a=p.parse_args()
    root=Path(__file__).resolve().parents[2];gen=root/'rexlego/generated/default';src=root/'rexlego/src';out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
    names=['sub_82FEA950','sub_82FEA648','__savegprlr_27','__restgprlr_27'];found={}
    for f in gen.glob('legodimensions_recomp.*.cpp'):
        text=f.read_text()
        for name in names:
            start=text.find('DEFINE_REX_FUNC('+name+')')
            if start<0:continue
            end=text.find('\nDEFINE_REX_FUNC(',start+1);found[name]=text[start:end if end>=0 else len(text)]
    assert set(found)==set(names)
    prefix=r'''
#define NOMINMAX
#include "legodimensions_pch.h"
#include "native_frontend_guest.h"
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <vector>
#undef DEFINE_REX_FUNC
#define DEFINE_REX_FUNC(name) void name(PPCContext&ctx,uint8_t*base)
#define CHECK(x) do{if(!(x)){std::fprintf(stderr,"failed %d: %s\n",__LINE__,#x);std::exit(23);}}while(0)
constexpr uint32_t arena=0x20000000,object=arena+0x1000,manager=arena+0x2000,pages=arena+0x3000,
 session=arena+0x4000,globalstate=arena+0x5000,video=arena+0x6000,effects=arena+0x7000;
bool allowed=false,transition=false;uint32_t selected=0,sounds=0;
void sub_82D70708(PPCContext&ctx,uint8_t*){CHECK(ctx.r3.u32==object+0x400);ctx.r3.u64=allowed;}
void sub_82EF9560(PPCContext&ctx,uint8_t*){CHECK(ctx.r3.u32==object+0x800);ctx.r3.u32=selected;}
void sub_82EF61F8(PPCContext&ctx,uint8_t*){CHECK(ctx.r3.u32==object+0x800);ctx.r3.u32=1;}
void sub_82FBEFC8(PPCContext&ctx,uint8_t*){CHECK(ctx.r3.u32==0);++sounds;}
void sub_82A36908(PPCContext&ctx,uint8_t*){ctx.r3.u32=session;}
void sub_82A465F0(PPCContext&ctx,uint8_t*){CHECK(ctx.r3.u32==session+0x100);ctx.r3.u64=transition;}
namespace rex::runtime {PPCFunc* ResolveIndirectFunction(uint32_t){CHECK(false);return nullptr;}}
'''
    body='\n'.join(found[n] for n in names)
    defined=set(names)|set(re.findall(r'void (sub_\w+)\(PPCContext',prefix))
    called=set(re.findall(r'\b(\w+)\(ctx, base\);',body))
    mocks='\n'.join('void '+name+'(PPCContext&,uint8_t*){CHECK(false);}' for name in sorted(called-defined))
    declarations='\n'.join('void '+name+'(PPCContext&,uint8_t*);' for name in names)
    suffix=r'''
int main(int argc,char**argv){
 CHECK(argc==2);auto*base=static_cast<uint8_t*>(VirtualAlloc(nullptr,0x100000000ull,MEM_RESERVE,PAGE_NOACCESS));CHECK(base);
 CHECK(VirtualAlloc(base+0x82000000,0x2B80000,MEM_COMMIT,PAGE_READWRITE));CHECK(VirtualAlloc(base+arena,0x100000,MEM_COMMIT,PAGE_READWRITE));
 std::ifstream image(argv[1],std::ios::binary);CHECK(image);image.read(reinterpret_cast<char*>(base+0x82000000),45613056);CHECK(image.gcount()==45613056);
 REX_STORE_U32(0x849F5640,manager);REX_STORE_U32(0x849E21CC,globalstate);REX_STORE_U32(globalstate+76,0);
 REX_STORE_U32(manager+140,pages);REX_STORE_U32(pages+84,video);REX_STORE_U32(pages+88,effects);
 REX_STORE_U32(session,session+0x100);REX_STORE_U32(object+40,object+0x400);REX_STORE_U32(object+44,object+0x800);
 for(unsigned offset=96;offset<=144;offset+=4)REX_STORE_U32(object+offset,object+0x1000+offset);
 unsigned count=0;
 for(unsigned code:{21u,22u})for(bool mode:{false,true})for(bool pending:{false,true})for(bool enable:{false,true}){
  transition=mode;allowed=enable;selected=REX_LOAD_U32(object+(code==21?108:112));sounds=0;
  REX_STORE_U32(manager+168,pending?7:0xFFFFFFFF);REX_STORE_U32(manager+172,0xAABBCCDD);REX_STORE_U32(manager+184,0x11223344);
  REX_STORE_U32(object+148,0x55667788);
  PPCContext ctx{};ctx.r1.u32=arena+0xF0000;ctx.r3.u32=object;ctx.r4.u32=0x1234;ctx.r5.u32=15;ctx.r6.u32=0x5678;
  const auto saved=ctx;legodimensions::native_frontend::CallGuest(sub_82FEA950,ctx,base,object,0x1234,15,0x5678);
  CHECK(std::memcmp(&saved,&ctx,sizeof(ctx))==0);
  if(!enable){CHECK(sounds==0);CHECK(REX_LOAD_U32(manager+172)==0xAABBCCDD&&REX_LOAD_U32(manager+184)==0x11223344);CHECK(REX_LOAD_U32(object+148)==0x55667788);}
  else{
   CHECK(sounds==1&&REX_LOAD_U32(object+148)==selected);
   if(!mode){CHECK(REX_LOAD_U32(manager+172)==code);CHECK(REX_LOAD_U32(manager+168)==(pending?7:0xFFFFFFFF));CHECK(REX_LOAD_U32(manager+184)==0x11223344);}
   else if(pending){CHECK(REX_LOAD_U32(manager+168)==7&&REX_LOAD_U32(manager+184)==0x11223344);}
   else{CHECK(REX_LOAD_U32(manager+168)==code);CHECK(REX_LOAD_U32(manager+184)==(code==21?video:effects));}
  }++count;
 }
 VirtualFree(base,0,MEM_RELEASE);std::printf("PASS %u translated Options routing cases: Accept 15, Video/Effects page IDs and slots, pending transitions, native input gate, full PPC context restoration\n",count);
}
'''
    def run(label,body):
        source=out/(label+'.cpp');source.write_text(prefix+declarations+mocks+body+suffix);exe=out/(label+'.exe')
        cmd=[a.compiler,'-std=c++23','-O2','-mavx2','-D_CRT_SECURE_NO_WARNINGS','-DSPDLOG_FMT_EXTERNAL','-Wno-unused-value','-I'+str(gen),'-I'+str(src),'-I'+str(root.parent/'dev-assets/sdk-install/include'),str(source),'-o',str(exe)]
        result=subprocess.run(cmd,capture_output=True,text=True,timeout=50);(out/(label+'-compile.log')).write_text(result.stdout+result.stderr);result.check_returncode()
        result=subprocess.run([str(exe),str(a.image.resolve())],capture_output=True,text=True,timeout=15);(out/(label+'.log')).write_text(result.stdout+result.stderr);return result
    positive=run('production',body);positive.check_returncode();print(positive.stdout,end='');negative=[]
    for label,old,new in [('wrong-video-id','ctx.r11.s64 = 21;','ctx.r11.s64 = 22;'),
                          ('ignore-input-gate','if (ctx.cr6.eq) goto loc_82FEA93C;','if (false) goto loc_82FEA93C;'),
                          ('wrong-video-slot','REX_LOAD_U32(ctx.r11.u32 + 84);','REX_LOAD_U32(ctx.r11.u32 + 88);')]:
        assert body.count(old)==1,(label,body.count(old));result=run(label,body.replace(old,new));assert result.returncode==23,(label,result.returncode,result.stderr)
        negative.append(dict(name=label,exit=result.returncode,error=result.stderr))
    (out/'verification.json').write_text(json.dumps(dict(passed=True,game_launched=False,image_sha256=hashlib.sha256(a.image.read_bytes()).hexdigest(),
        translated_sha256={n:hashlib.sha256(found[n].encode()).hexdigest() for n in names},output=positive.stdout,negative=negative,
        scope='Actual translated Options dispatcher and Accept handler. Input selection, sound, session mode and widget focus are boundary mocks. Does not prove native page construction, menu entry visibility/layout, main/pause entry reachability or rendering.'),indent=2))
if __name__=='__main__':main()
