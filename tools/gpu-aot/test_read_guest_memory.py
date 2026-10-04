"""Read actual synthetic process pages; verify Windows E/F alias offset and limits."""
import argparse
from pathlib import Path
import subprocess
import json

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('output', type=Path)
a = p.parse_args()
a.output.mkdir(parents=True, exist_ok=True)
root = Path(__file__).resolve().parents[2]
source = a.output/'child.cpp'
source.write_text(r'''
#include <windows.h>
#include <cstdio>
#include <cstring>
int main() {
  for (uintptr_t guest : {uintptr_t(0xA0000000), uintptr_t(0xE0000000)}) {
    auto* memory = VirtualAlloc(reinterpret_cast<void*>(0x100000000ull + guest),
        8192, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!memory) return 1;
    std::memset(memory, 0x19, 4096);
    std::memset(static_cast<char*>(memory) + 4096, 0x73, 4096);
  }
  std::puts("ready"); std::fflush(stdout); std::getchar();
}
'''.replace('#include <cstring>', '#include <cstring>\n#include <initializer_list>'))
child_exe = a.output/'child.exe'
reader = a.output/'reader.exe'
subprocess.run(['clang++', '-std=c++20', '-O2', str(source), '-o', str(child_exe)], check=True)
subprocess.run(['clang++', '-std=c++20', '-O2', '-municode',
    str(root/'tools/gpu-aot/read_guest_memory.cpp'), '-o', str(reader)], check=True)
child = subprocess.Popen([str(child_exe.resolve())], stdin=subprocess.PIPE,
    stdout=subprocess.PIPE, text=True, creationflags=subprocess.CREATE_NO_WINDOW)
try:
    assert child.stdout.readline().strip() == 'ready'
    def read(folder, addresses, length=None, expected=None):
        cmd = [str(reader.resolve()), str(child.pid), str((expected or child_exe).resolve()),
               '0x100000000', str(folder)]
        if length is not None:
            cmd.extend(['--bytes', str(length)])
        return subprocess.run(cmd + addresses, capture_output=True, text=True)
    folder = a.output/'valid'
    result = read(folder, ['0xA0000000', '0xE0000000'])
    assert result.returncode == 0, result.stderr
    assert (folder/'guest-2684354560.bin').read_bytes() == bytes([0x19])*1024
    assert (folder/'guest-3758096384.bin').read_bytes() == bytes([0x73])*1024
    result = read(a.output/'long', ['0xE0000000'], 4096)
    assert result.returncode == 0, result.stderr
    assert (a.output/'long/guest-3758096384.bin').read_bytes() == bytes([0x73])*4096
    for addresses, length in [(['0xE0000000'], 0), (['0xE0000000'], 8*1024*1024+1),
                              (['0xA0000000']*33, None), (['0xA0000000']*17, 8*1024*1024),
                              (['0xFFFFFFF0'], None)]:
        assert read(a.output/'invalid', addresses, length).returncode == 2
    assert read(a.output/'wrong-exe', ['0xA0000000'], expected=reader).returncode == 4
finally:
    child.communicate('\n', timeout=10)
assert child.returncode == 0
(a.output/'verification.json').write_text(json.dumps({'passed':True,
    'checks':['default1024 bytes','variable4096 bytes','E/F +4KB offset','ordinary address unchanged',
              'length/address/count/total bounds','process executable identity'],
    'game_launched':False}, indent=2)+'\n')
print('PASS: actual bounded process reads, alias offset and executable identity')
