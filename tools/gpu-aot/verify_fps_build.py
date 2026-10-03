"""Write a separate current-build proof without overwriting older probe proofs."""
import argparse
import hashlib
import json
from pathlib import Path
import re
p=argparse.ArgumentParser(description=__doc__);p.add_argument('output',type=Path);a=p.parse_args()
root=Path(__file__).resolve().parents[2];build=root/'rexlego/out/build/win-amd64-release'
fingerprint=re.search(r'kNativeGpuBuildFingerprint\[\] = "([0-9a-f]+)"',
 (build/'generated/native_gpu_build_info.h').read_text())[1]
exe=(build/'legodimensions.exe').read_bytes();assert fingerprint.encode() in exe
for literal in [b'placement buffer relocation type',b'cpu_resource_wait_skips',b'Native synchronization timing',
 b'Native callback queue', b'deferred callback=', b'asynchronous readback generations',
 b'disabled DoF sharp/mip tonemap mix', b'render default setter=',
 b'frame-ring GPU wait failed']:
 assert literal in exe
config=(build/'legodimensions.toml').read_bytes();sha=lambda b:hashlib.sha256(b).hexdigest()
assert sha(config).upper()=='AC5DA9E5E080B4E999F28194424F3E11BEEFAC09232550047FE86BF6F75DDFA5'
native=root/'rexlego/out/native-gpu';session=native/'session-20260930'
assert len(re.findall(r'\{ 0x[A-Fa-f0-9]+, \d+, \d+, \d+, \d+, \d+ \}',(native/'runtime-cache.cpp').read_text()))==118
assert 'g_shaderMicrocodeEntryCount = 18722;' in (native/'lego-microcode-index.cpp').read_text()
tests={}
for name in ['placement-resource-type-tests','inline-hash-sdk-tests','cpu-resource-wait-tests','completion-callback-tests',
 'buffer-variant-tests','draw-upload-tests','query-retirement-tests','resize-fence-tests','alpha-tests']:
 proof=json.loads((session/name/'verification.json').read_text())
 assert proof.get('passed',proof.get('identical',False));tests[name]=proof
prelinked={}
for name,raw,linked in [('main','lego-fixed16-cache.cpp','lego-linked-dxil-cache.cpp'),
                         ('runtime','runtime-cache.cpp','runtime-linked-dxil-cache.cpp')]:
 expected=set()
 for shader,mask in re.findall(r'\{ 0x([A-Fa-f0-9]+), \d+, \d+, \d+, \d+, (\d+) \}',(native/raw).read_text()):
  mask=int(mask)
  if mask:
   expected.update((int(shader,16),value) for value in range(mask+1) if not value&~mask)
 actual=set()
 with (native/linked).open() as stream:
  for line in stream:
   if line.strip()=='};':break
   m=re.search(r'\{ 0x([A-Fa-f0-9]+), 0x([A-Fa-f0-9]+), (\d+), (\d+) \}',line)
   if m:
    assert int(m[4])>0
    actual.add((int(m[1],16),int(m[2],16)))
 assert expected==actual,(name,len(expected),len(actual))
 prelinked[name]={'variants':len(actual),'all_specialization_subsets_present':True}
report={'fingerprint':fingerprint,'exe_sha256':sha(exe),'config_sha256':sha(config),
 'runtime_containers':118,'index_entries':18722,'shared_constants_bytes':624,
 'cpu_resource_wait_optimization':'opt-in, default full waits',
 'insert_callback':'nonblocking enqueue; FIFO execution after containing submission completes',
 'profiler_marker_timing':'coarse completion-poll timing, not exact GPU timestamps',
 'query_lifetime':'fresh readback generation per BEGIN; retired at every live host fence; GetData remains fence-gated',
 'disabled_dof':'tonemap 3A47E5DDE66B42C6 host-only c12.xy=0 when depth_of_field=false; exposure/bloom/LUT unchanged',
 'alpha_test':'all 8 Xenos comparisons using original shader alpha and raw float reference; no test for depth-only shaders',
 'render_defaults':'15 audited TU23 leaf alpha/blend/color-mask setters, descriptor values and order; alpha reference default 1.0',
 'resize':'wait all submitted GPU fences before destruction; event wakeups gated by actual fence values; retry failed/minimized resize',
 'prelinked_shader_coverage':prelinked,
 'full_content_hash_validation_preserved':True,'tests':tests,'stable_30_fps_verified':False,
 'full_cutscene_verified':False,'settings_and_hub_verified':False}
a.output.write_text(json.dumps(report,indent=2));print(json.dumps({k:v for k,v in report.items() if k!='tests'},indent=2))
