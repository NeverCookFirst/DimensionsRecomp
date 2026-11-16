"""Read-only Linux inventory. Does not start Wine, Proton, Steam or the game."""
import argparse
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output', type=Path, help='Save JSON to this new file (default: stdout)')
args = parser.parse_args()
report = dict(game_launched=False, native_linux_renderer_available=False,
              platform=platform.system(), machine=platform.machine(), kernel=platform.release(),
              proton_gameplay_validated=False, checks={})
if platform.system() == 'Linux':
    report['distribution'] = platform.freedesktop_os_release()
    cpuinfo = Path('/proc/cpuinfo').read_text(errors='replace')
    flag_lines = [line.partition(':')[2].split() for line in cpuinfo.splitlines() if line.startswith('flags\t')]
    common = set.intersection(*(set(flags) for flags in flag_lines)) if flag_lines else set()
    report['cpu_flags_common_to_all_cores'] = sorted(common)
    report['current_game_required_flags'] = ['avx', 'avx2', 'bmi1', 'bmi2', 'f16c', 'fma', 'lahf_lm', 'abm', 'movbe', 'popcnt', 'pni', 'ssse3', 'sse4_1', 'sse4_2', 'cx16']
    # Linux exposes LZCNT as abm or lzcnt. This is an inventory, not a CPUID/XCR0 proof.
    report['current_game_missing_flags'] = [flag for flag in report['current_game_required_flags']
        if flag not in common and not (flag == 'abm' and 'lzcnt' in common)]
    report['memory_info'] = Path('/proc/meminfo').read_text().splitlines()[:3]
    for name, arguments in [('vulkaninfo', ['--summary']), ('lspci', ['-nnk']), ('ldd', ['--version'])]:
        executable = shutil.which(name)
        if not executable:
            report['checks'][name] = dict(available=False)
            continue
        try:
            completed = subprocess.run([executable, *arguments], capture_output=True, text=True,
                                       errors='replace', timeout=30, env={**os.environ, 'LC_ALL': 'C'})
            report['checks'][name] = dict(available=True, exit_code=completed.returncode,
                stdout=completed.stdout, stderr=completed.stderr)
        except subprocess.TimeoutExpired:
            report['checks'][name] = dict(available=True, timed_out=True)
report['limitations'] = ('The native game renderer is currently Windows D3D12 only. Vulkan loader availability '
    'does not prove device features or vkd3d-proton compatibility. This report does not install or modify anything '
    'and does not validate rendering, performance, input, audio, saves, memory faults or FSR on Linux.')
text = json.dumps(report, indent=2) + '\n'
if args.output:
    with args.output.open('x', encoding='utf-8') as output:
        output.write(text)
else:
    print(text, end='')
