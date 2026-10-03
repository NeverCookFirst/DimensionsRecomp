"""Exercise the actual hook routing macros for every native GPU replacement."""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('output', type=Path)
a = p.parse_args()
a.output.mkdir(parents=True, exist_ok=True)
root = Path(__file__).resolve().parents[2]
sources = root/'rexlego/src/gpu_native'
hooks = []
for path in sources.glob('*.cpp'):
    text = path.read_text()
    found = re.findall(r'REX_HOOK(_RAW)?\(\s*(sub_[0-9A-Fa-f]+)', text)
    if found:
        assert '#include "gpu_native/renderer_route.h"' in text, path
    hooks.extend((bool(raw), name) for raw, name in found)
assert len(hooks) == len({name for _,name in hooks}) and hooks
fake = a.output/'include/rex/hook.h'
fake.parent.mkdir(parents=True, exist_ok=True)
fake.write_text('''#pragma once
#include <cstdint>
using u8=uint8_t;
struct PPCContext { uint64_t r3=0, canary=0x123456789ABCDEF0ull; };
#define REX_FUNC(name) void name(PPCContext& ctx, u8* base)
#define REXLOG_INFO(...) ((void)0)
namespace rex::ppc { template<auto Function> void HostToGuestFunction(PPCContext& ctx,u8* base) { Function(ctx,base); } }
''')
(fake.parent/'logging.h').write_text('#pragma once\n#define REXLOG_INFO(...) ((void)0)\n#define REXLOG_ERROR(...) ((void)0)\n')
(fake.parent/'cvar.h').write_text('#pragma once\n#define REXCVAR_DECLARE(type,name) type& FLAGS_##name##_storage_()\n#define REXCVAR_GET(name) FLAGS_##name##_storage_()\n')
app_source = (root/'rexlego/src/legodimensions_app.h').read_text()
def app_body(name):
    start = app_source.index('void '+name+'(')
    pos = app_source.index('{',start)
    end, depth = pos+1, 1
    while depth:
        depth += (app_source[end]=='{')-(app_source[end]=='}')
        end += 1
    return app_source[start:end].replace(' override','')
code = ['#include <cassert>\n#include "gpu_native/renderer_route.h"',
        'bool& FLAGS_gpu_native_pm4_storage_() { static bool setting=std::getenv("TEST_PM4_CONFIG")!=nullptr; return setting; }',
        '#include <memory>\n#include <string>',
        'namespace rex { struct RuntimeConfig { std::unique_ptr<int> graphics; std::string gpu_plugin; }; }',
        'namespace legodimensions::gpu_native { struct HostDevice { inline static int creates=0; static bool Create(int) { ++creates; return true; } }; }',
        'struct App { int window(){return 0;} App& app_context(){return *this;} void QuitFromUIThread(){assert(false);}',
        app_body('OnPreSetup'),app_body('OnPreLaunchModule'),'};',
        'static int originals=0,natives=0; static u8* expected_base;',
        'static void native_call(PPCContext& ctx,u8* base) { assert(base==expected_base); ++natives; ctx.r3=22; }']
for raw,name in hooks:
    code.append(f'extern "C" REX_FUNC(__imp__{name}) {{ assert(base==expected_base); ++originals; ctx.r3=11; }}')
    code.append(f'REX_HOOK_RAW({name}) {{ native_call(ctx,base); }}' if raw else f'REX_HOOK({name},native_call);')
code.append('int main(int argc,char**) { bool reference=argc>1; u8 memory[8]{}; expected_base=memory; PPCContext ctx;')
code.append('App app; rex::RuntimeConfig config; config.graphics=std::make_unique<int>(7); config.gpu_plugin="xenos"; app.OnPreSetup(config); assert(bool(config.graphics)==reference); assert(config.gpu_plugin==(reference?"xenos":"")); app.OnPreLaunchModule(); assert(legodimensions::gpu_native::HostDevice::creates==(reference?0:1)); rex::RuntimeConfig empty; app.OnPreSetup(empty); assert(empty.gpu_plugin==(reference?"xenos":""));')
for _,name in hooks:
    code.append(f'{name}(ctx,memory); assert(ctx.r3==(reference?11:22)); assert(ctx.canary==0x123456789ABCDEF0ull);')
code.append(f'assert(originals==(reference?{len(hooks)}:0)); assert(natives==(reference?0:{len(hooks)}));')
# Selection remains locked for resource lifetime, even if the environment changes.
code.append('_putenv_s("LEGO_NATIVE_PM4_REFERENCE",reference?"0":"1"); assert(legodimensions::gpu_native::UsePm4Reference()==reference); }')
cpp = a.output/'route.cpp'
cpp.write_text('\n'.join(code))
exe = a.output/'route.exe'
subprocess.run(['clang++','-std=c++20','-I'+str(a.output/'include'),'-I'+str(root/'rexlego/src'),str(cpp),'-o',str(exe)],check=True)
for configured in (False,True):
    for value in (None,'0','1','false','yes',''):
        env = dict(os.environ)
        env.pop('LEGO_NATIVE_PM4_REFERENCE',None)
        env.pop('TEST_PM4_CONFIG',None)
        if configured: env['TEST_PM4_CONFIG']='1'
        if value is not None: env['LEGO_NATIVE_PM4_REFERENCE']=value
        selected = value=='1' if value in ('0','1') else configured
        subprocess.run([str(exe.resolve())]+(['reference'] if selected else []),env=env,check=True)
report = {'passed':True,'hooks':len(hooks),'raw_hooks':sum(raw for raw,_ in hooks),
          'environment_cases':12,'actual_header_macros':True,'mock_ppc_and_marshaler':True,
          'checks':['all native GPU hook sources covered','original path bypasses native body',
                    'configured mode and explicit environment override','ctx/base forwarded','register canary preserved',
                    'route immutable after first use','actual app setup preserves SDK plugin in reference',
                    'actual app prelaunch bypasses native initialization in reference']}
(a.output/'verification.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps(report,indent=2))
