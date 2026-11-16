"""Replay the actual TU23 cel selector and verify scoped native bloom bindings offline."""
from pathlib import Path
import argparse, hashlib, json, shutil, subprocess, tomllib


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument('output', type=Path)
    parser.add_argument('--compiler', default='clang++')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    src = root/'rexlego/src'
    generated = root/'rexlego/generated/default'
    sdk = root.parent/'dev-assets/sdk-install'
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    for name in ('rexruntime.dll', 'amd_fidelityfx_dx12.dll'):
        shutil.copy2(root.parent/'build/worlds-candidate-v18'/name, out/name)
    code = (generated/'legodimensions_recomp.61.cpp').read_text()
    function = code[code.index('DEFINE_REX_FUNC(sub_82BC4340)'):]
    function = function[:function.index('\nDEFINE_REX_FUNC(')]
    block = function[function.index('\t// li r21,1'):function.index('\tctx.r27.u64 = REX_LOAD_U32(ctx.r30.u32 + 40);')]
    assert block.count('NativeCelShadingViewGate(') == block.count('NativeCelShadingMaskGate(') == 1
    instructions = '\n'.join(line for line in block.splitlines() if not line.lstrip().startswith('//'))
    assert 'ctx.r10.u64 = REX_LOAD_U8(ctx.r22.u32 + 331);\n\tNativeCelShadingViewGate(ctx.r10);\n\tctx.cr6.compare<uint32_t>(ctx.r10.u32, 0, ctx.xer);' in instructions
    assert 'ctx.r8.u64 = ctx.r10.u64 & ctx.r12.u64;\n\tNativeCelShadingMaskGate(ctx.r8);\n\tctx.r10.u64 = ctx.r21.u64;' in instructions
    config = tomllib.loads((root/'rexlego/legodimensions_config.toml').read_text(encoding='utf-8-sig'))
    assert {(h['address'], tuple(h['registers'])) for h in config['midasm_hook']
            if h['name'] in ('NativeCelShadingViewGate','NativeCelShadingMaskGate')} == {(0x82BC4550, ('r10',)), (0x82BC4568, ('r8',))}
    draw = (src/'gpu_native/draw.cpp').read_text()
    bind = draw[draw.index('bool BindConstants('):draw.index('\nplume::RenderPipeline* GetPipeline(')]
    assert bind.index('SuppressNativeBloomTexture(pixel_shader_hash, i, bloom)') < bind.index('t_preparing_draw->missing_textures |=')
    assert 'u32 texture_2d[32]{};' in draw
    device = (src/'gpu_native/device.cpp').read_text()
    assert 'std::memset(mapped, 0, 8 * 0x200);' in device
    assert 'i, state.null_textures[i].get(),' in device
    cpp = r'''
#include "legodimensions_pch.h"
#include <rex/cvar.h>
#include "gpu_native/visual_effects.h"
#include <array>
#include <cstdio>
#include <cstdlib>
using namespace legodimensions::gpu_native;
extern void NativeCelShadingViewGate(PPCRegister&);
extern void NativeCelShadingMaskGate(PPCRegister&);
extern void NativeCelShadingTrace(PPCRegister&, PPCRegister&, PPCRegister&, PPCRegister&);
#define CHECK(x) do {if(!(x)){std::fprintf(stderr,"line %d failed: %s\n",__LINE__,#x);std::exit(23);}} while(0)
unsigned Select(uint8_t world,uint64_t flags,bool variant,bool special,unsigned base_technique) {
  std::array<uint8_t,0x5000> memory{};auto* base=memory.data();PPCContext ctx{};
  ctx.r19.u32=special;ctx.r20.u32=0;ctx.r22.u32=0x1000;ctx.r30.u32=0x2000;ctx.r9.u32=base_technique;
  REX_STORE_U8(0x1000+331,world);REX_STORE_U64(0x1000+96,flags);
  REX_STORE_U32(0x2000+40,0x3000);REX_STORE_U32(0x3000+96,variant?0x4000:0);
  const auto before=memory;
  // SELECTOR
  CHECK(memory==before);return ctx.r9.u32;
}
int main(){
  unsigned cases=0;
  // The real application CVar registration and validation, before the native
  // runtime publishes its first immutable proposal.
  CHECK(rex::cvar::GetFlagByName("native_cel_shading")=="original");
  CHECK(!rex::cvar::SetFlagByName("native_cel_shading","garbage"));
  CHECK(rex::cvar::SetFlagByName("native_cel_shading","off"));
  CHECK(CurrentNativeVisualEffects().cel_shading==NativeCelShadingMode::kOff);
  for(auto mode:{NativeCelShadingMode::kOriginal,NativeCelShadingMode::kEverywhere,NativeCelShadingMode::kOff})
   for(bool bloom:{false,true}){
    PublishNativeVisualEffects({bloom,mode});auto active=CurrentNativeVisualEffects();
    CHECK(active.bloom==bloom&&active.cel_shading==mode);
    // A draft/live registry mutation cannot override the committed frame.
    CHECK(rex::cvar::SetFlagByName("native_cel_shading","original"));
    CHECK(CurrentNativeVisualEffects().cel_shading==mode);
    for(unsigned world:{0,1,255})for(uint64_t flags:{uint64_t(0),uint64_t(1)<<44,~uint64_t(0)})
     for(bool variant:{false,true})for(bool special:{false,true})for(unsigned base:{0,11,14,15}){
      const bool original=world&&(flags&(uint64_t(1)<<44));
      const bool cel=variant&&!special&&(mode==NativeCelShadingMode::kEverywhere||
          (mode!=NativeCelShadingMode::kEverywhere&&original));
      CHECK(Select(world,flags,variant,special,base)==(cel?24:base+4));++cases;
     }
    for(uint64_t shader:{UINT64_C(0x3A47E5DDE66B42C6),UINT64_C(0x967A1882A7502FB3),UINT64_C(0x76B999E82C424E40),UINT64_C(0x025D4B4261D8A4D1),UINT64_C(0)})
     for(unsigned slot=0;slot<32;++slot){
      const bool final=shader==UINT64_C(0x3A47E5DDE66B42C6)||shader==UINT64_C(0x967A1882A7502FB3)||shader==UINT64_C(0x76B999E82C424E40);
      CHECK(SuppressNativeBloomTexture(shader,slot,bloom)==(!bloom&&slot==3&&final));++cases;
     }
   }
  // A commit between the two predicates must not split one material choice.
  PublishNativeVisualEffects({true,NativeCelShadingMode::kEverywhere});
  PPCRegister view{};NativeCelShadingViewGate(view);CHECK(view.u64==1);
  PublishNativeVisualEffects({false,NativeCelShadingMode::kOff});
  PPCRegister mask{};NativeCelShadingMaskGate(mask);CHECK(mask.u64==1);
  view.u64=7;NativeCelShadingViewGate(view);CHECK(view.u64==7);
  ClearNativeVisualEffects();CHECK(CurrentNativeVisualEffects().cel_shading==NativeCelShadingMode::kOriginal);
  std::printf("PASS %u effect cases: actual generated cel selector, material fallback, special passes, guest immutability, committed controls, scoped bloom input\n",cases);
}
'''.replace('  // SELECTOR', block)
    flags = ['-std=c++23', '-O2', '-DLEGODIMENSIONS_NATIVE_GPU', '-DWIN32_LEAN_AND_MEAN',
             '-DNOMINMAX', '-DSPDLOG_FMT_EXTERNAL', '-DFMT_HEADER_ONLY', '-Wno-deprecated-literal-operator',
             '-I'+str(src), '-I'+str(generated), '-I'+str(sdk/'include')]

    def run(label, header=None, selector=cpp):
        local = out/label
        local.mkdir(exist_ok=True)
        extra = []
        if header:
            path = local/'gpu_native/visual_effects.h'
            path.parent.mkdir(exist_ok=True)
            path.write_text(header)
            extra = ['-I'+str(local)]
        source = local/'fixture.cpp'
        source.write_text(selector)
        implementation = local/'native_effects.cpp'
        implementation.write_text((src/'native_effects.cpp').read_text())
        exe = out/(label+'.exe')
        result = subprocess.run([args.compiler, *extra, *flags, str(source), str(implementation),
                                 str(sdk/'lib/rexruntime.lib'), '-o', str(exe)], capture_output=True, text=True, timeout=90)
        (out/(label+'-compile.log')).write_text(result.stdout+result.stderr)
        result.check_returncode()
        result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=20)
        (out/(label+'.log')).write_text(result.stdout+result.stderr)
        return result

    result = run('production')
    result.check_returncode()
    print(result.stdout, end='', flush=True)
    header = (src/'gpu_native/visual_effects.h').read_text()
    negative = []
    for label, old, new in [
        ('everywhere-disabled', 'if (mode == NativeCelShadingMode::kEverywhere) return 1;', 'if (mode == NativeCelShadingMode::kEverywhere) return 0;'),
        ('legacy-off-unlit-regression', 'if (mode == NativeCelShadingMode::kOff) return original;', 'if (mode == NativeCelShadingMode::kOff) return 0;'),
        ('bloom-wrong-slot', '!bloom && slot == 3', '!bloom && slot == 4'),
    ]:
        assert header.count(old) == 1
        bad = run(label, header.replace(old, new))
        assert bad.returncode == 23, (label, bad.returncode, bad.stderr)
        negative.append(label)
    # Remove the original material availability branch: the replay must fail.
    bad_selector = cpp.replace('ctx.r8.u64 = REX_LOAD_U32(ctx.r10.u32 + 96);', 'ctx.r8.u64 = 1;')
    assert bad_selector != cpp
    bad = run('missing-material-check', selector=bad_selector)
    assert bad.returncode == 23
    negative.append('missing-material-check')
    files = [src/'native_effects.cpp', src/'gpu_native/visual_effects.h', src/'gpu_native/draw.cpp',
             generated/'legodimensions_recomp.61.cpp', generated/'legodimensions_funcs.61.h',
             root/'rexlego/legodimensions_config.toml', Path(__file__).resolve()]
    (out/'verification.json').write_text(json.dumps(dict(passed=True, game_launched=False, output=result.stdout,
        negative=negative, sources={str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in files},
        limits=['CPU replay uses actual generated selector block and SDK PPC operations with synthetic guest inputs.',
                'Legacy Off now intentionally aliases original material lighting; a true outline-only Off feature is deferred. No reference visual parity is asserted.']), indent=2))


if __name__ == '__main__':
    main()
