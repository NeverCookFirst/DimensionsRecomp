"""CPU-only observer checks with real SDK memory and a kernel-memory binding seam."""
from pathlib import Path
import argparse
import hashlib
import json
import shutil
import subprocess
import tomllib

p = argparse.ArgumentParser(__doc__)
p.add_argument('output', type=Path)
p.add_argument('--compiler', default='clang++')
a = p.parse_args()
root = Path(__file__).resolve().parents[2]
src = root/'rexlego/src'
sdk = root.parent/'dev-assets/sdk-install'
out = a.output.resolve()
out.mkdir(parents=True, exist_ok=True)
for name in ('rexruntime.dll', 'amd_fidelityfx_dx12.dll'):
    shutil.copy2(root.parent/'build/worlds-candidate-v18'/name, out/name)
production = (src/'native_effects.cpp').read_text()
kernel_binding = '''  auto* kernel = rex::system::kernel_state();
  if (!kernel || !kernel->memory()) return;
  auto& memory = *kernel->memory();'''
assert production.count(kernel_binding) == 1
observed = production.replace(kernel_binding, '''  extern rex::memory::Memory* fixture_memory;
  extern unsigned fixture_memory_accesses;
  ++fixture_memory_accesses;
  if (!fixture_memory) return;
  auto& memory = *fixture_memory;''')
config = tomllib.loads((root/'rexlego/legodimensions_config.toml').read_text(encoding='utf-8-sig'))
expected = {
    'NativeCelShadingTrace': (0x82BC459C, ['r9', 'r22', 'r30', 'r19']),
    'NativeCelShadingDefaultBinding': (0x82BC4970, ['r4', 'r26', 'r22', 'r30']),
    'NativeCelShadingIndexedBinding': (0x82BC4A84, ['r4', 'r26', 'r22', 'r30']),
}
assert {h['name']: (h['address'], h['registers']) for h in config['midasm_hook']
        if h['name'] in expected} == expected
generated = (root/'rexlego/generated/default/legodimensions_recomp.61.cpp').read_text()
for call, after in [
    ('NativeCelShadingTrace(ctx.r9, ctx.r22, ctx.r30, ctx.r19);', 'ctx.r27.u64 = REX_LOAD_U32(ctx.r30.u32 + 40);'),
    ('NativeCelShadingDefaultBinding(ctx.r4, ctx.r26, ctx.r22, ctx.r30);', 'ctx.lr = 0x82BC4974;'),
    ('NativeCelShadingIndexedBinding(ctx.r4, ctx.r26, ctx.r22, ctx.r30);', 'ctx.lr = 0x82BC4A88;'),
]:
    assert generated.count(call) == 1
    assert generated[generated.index(call):].splitlines()[1].strip() == after

fixture = r'''
#include "observed-production.cpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
using namespace legodimensions::gpu_native;
namespace legodimensions::gpu_native {
rex::memory::Memory* fixture_memory = nullptr;
unsigned fixture_memory_accesses = 0;
}
#define CHECK(x) do {if(!(x)){std::fprintf(stderr,"line %u: %s\n",__LINE__,#x);std::exit(23);}} while(0)
int main(int argc, char** argv) {
  CHECK(argc == 3);
  const bool enabled = argv[2][0] == '1';
  CHECK(rex::cvar::SetFlagByName("native_effects_diagnostics", enabled ? "true" : "false"));
  rex::InitLogging(argv[1], spdlog::level::info);
  CHECK(NativeEffectsDiagnosticsEnabled() == enabled);
  rex::memory::Memory memory;
  CHECK(memory.Initialize()); fixture_memory = &memory;
  auto* heap = memory.LookupHeap(0x10000000); CHECK(heap);
  uint32_t address = 0;
  CHECK(heap->Alloc(8192, 4096, rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit,
      rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite, false, &address));
  auto* bytes = memory.TranslateVirtual<uint8_t*>(address);
  std::memset(bytes, 0, 8192);
  auto word = [&](uint32_t offset, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) bytes[offset+i] = uint8_t(value >> (24-i*8));
  };
  constexpr unsigned v = 0x100, c = 0x800, t = 0x1800;
  word(v+8, 1); bytes[v+331] = 0;
  word(v+96, 0x1000); word(v+100, 0);
  word(c+40, address+t); bytes[c+1120] = 180; bytes[c+1125] = 0x40;
  word(t+96, 0xA0010000);
  std::array<uint8_t,8192> before{}; std::memcpy(before.data(),bytes,before.size());
  for (auto mode : {NativeCelShadingMode::kOriginal, NativeCelShadingMode::kEverywhere, NativeCelShadingMode::kOff})
   for (bool bloom : {false, true}) {
    PublishNativeVisualEffects({bloom,mode});
    PPCRegister technique{}, view{}, context{}, special{}, program{}, index{};
    technique.u64 = 24; view.u64 = address+v; context.u64 = address+c;
    special.u64 = 0; program.u64 = 0xA0010000; index.u64 = 96;
    for (unsigned n = 0; n < 40; ++n) {
      NativeCelShadingTrace(technique,view,context,special);
      NativeCelShadingDefaultBinding(program,index,view,context);
      NativeCelShadingIndexedBinding(program,index,view,context);
      CHECK(technique.u64 == 24 && view.u64 == address+v && context.u64 == address+c &&
          special.u64 == 0 && program.u64 == 0xA0010000 && index.u64 == 96);
      CHECK(!std::memcmp(before.data(),bytes,before.size()));
    }
    // Invalid and overflowing metadata placement must leave all state alone.
    view.u64 = 0xFFFFFFF0; NativeCelShadingTrace(technique,view,context,special);
    CHECK(view.u64 == 0xFFFFFFF0 && technique.u64 == 24);
    CHECK(!std::memcmp(before.data(),bytes,before.size()));
    // Saturate this choice's log budget, then prove other choices retain one.
    TraceNativeBloomBinding(UINT64_C(0x025D4B4261D8A4D1),9,bloom,0xA2000000,9999,7000,false);
    TraceNativeBloomBinding(UINT64_C(0x967A1882A7502FB3),9,bloom,0xA3000000,7000,7001,false);
    TraceNativeBloomBinding(UINT64_C(0x76B999E82C424E40),0x19,bloom,0xA3000001,7001,7002,false);
    for (unsigned n = 0; n < 1000; ++n)
      TraceNativeBloomBinding(UINT64_C(0x3A47E5DDE66B42C6),0xD9,bloom,0xA1000000+n,n,5000+n,false);
    TraceNativeBloomBinding(UINT64_C(0x025D4B4261D8A4D1),9,bloom,0xA2000000,9999,7000,false);
   }
  CHECK(enabled ? fixture_memory_accesses > 0 : fixture_memory_accesses == 0);
  rex::FlushLogging();
  std::printf("PASS enabled=%d: observer register/guest immutability, actual SDK metadata placement, invalid range, choice budgets, shader scope\n",enabled);
}
'''
(out/'fixture.cpp').write_text(fixture)
(out/'observed-production.cpp').write_text(observed)
flags = ['-std=c++23', '-O2', '-DLEGODIMENSIONS_NATIVE_GPU', '-DWIN32_LEAN_AND_MEAN',
         '-DNOMINMAX', '-DSPDLOG_FMT_EXTERNAL', '-DFMT_HEADER_ONLY', '-Wno-deprecated-literal-operator',
         '-I'+str(src), '-I'+str(sdk/'include')]

def compile_run(label, text):
    (out/'observed-production.cpp').write_text(text)
    exe = out/(label+'.exe')
    r = subprocess.run([a.compiler, *flags, str(out/'fixture.cpp'), str(sdk/'lib/rexruntime.lib'),
                        '-o', str(exe)], capture_output=True, text=True, timeout=90)
    (out/(label+'-compile.log')).write_text(r.stdout+r.stderr)
    r.check_returncode()
    records = []
    for enabled in (False, True):
        log = out/(label+('-on' if enabled else '-off')+'.log')
        if log.exists(): log.unlink()
        r = subprocess.run([str(exe), str(log), str(int(enabled))], capture_output=True, text=True, timeout=40)
        records.append(dict(enabled=enabled, exit_code=r.returncode, stdout=r.stdout, stderr=r.stderr))
        if label != 'production':
            if r.returncode == 23: break
            continue
        r.check_returncode()
        lines = log.read_text().splitlines()
        bloom = [line for line in lines if 'Native bloom binding:' in line]
        cel = [line for line in lines if 'Native cel selection:' in line]
        if enabled:
            assert len(bloom) == 6*64, len(bloom)
            assert len(cel) == 6*3, len(cel)
            for packed in ('80000000','80000001','80000002','80000003','80000004','80000005'):
                assert sum(f'packed={packed}' in line for line in bloom) == 64
                assert sum(f'packed={packed}' in line for line in cel) == 3
            assert all('variant24=A0010000 candidate=24' in line for line in cel)
            assert all('material_type=180 compile_flags=40' in line for line in cel)
            assert all(any('PS='+h in line for h in ('3A47E5DDE66B42C6','967A1882A7502FB3','76B999E82C424E40')) for line in bloom)
            for h in ('967A1882A7502FB3','76B999E82C424E40'):
                assert sum('PS='+h in line for line in bloom) == 6
        else:
            assert not bloom and not cel
        print(r.stdout.strip())
    return records

records = compile_run('production', observed)
negative = []
for label, bad in [
    ('disabled-guard-removed', observed.replace('if (!NativeEffectsDiagnosticsEnabled()) return;', '')),
    ('observer-register-write', observed.replace('technique.u32, view.u32, context.u32, special.u32, 0, 0);',
         'technique.u32, view.u32, context.u32, special.u32, 0, 0);\n  technique.u64 = 0;')),
]:
    assert bad != observed
    runs = compile_run(label,bad)
    assert any(r['exit_code'] == 23 for r in runs), (label,runs)
    negative.append(dict(name=label, records=runs))
(out/'observed-production.cpp').write_text(observed)
(out/'verification.json').write_text(json.dumps(dict(passed=True, game_launched=False, gpu_tested=False,
    actual_sdk_memory=True, kernel_memory_binding_seam=True, records=records, negative=negative,
    source_sha256=hashlib.sha256(production.encode()).hexdigest(),
    generated_sha256=hashlib.sha256(generated.encode()).hexdigest(),
    limits=['CPU observer and metadata tests only; no game/world output or material availability claim.']),indent=2))
