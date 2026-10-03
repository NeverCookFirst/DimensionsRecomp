"""Summarize opt-in mesh trace evidence; hashes alone do not identify models."""
import argparse
import collections
import json
import re
from pathlib import Path

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('log', type=Path)
p.add_argument('output', type=Path)
a = p.parse_args()
draws = {}
for line in a.log.read_text(errors='replace').splitlines():
    m = re.search(r'Native mesh: id=(\d+) (.*)', line)
    if not m:
        continue
    row = draws.setdefault(m[1], {'id': int(m[1]), 'observations': []})
    payload = m[2]
    if payload.startswith('VS='):
        row.update(re.findall(r'(\w+)=([^ ]+)', payload))
        row['timestamp'] = line.split(']')[0].lstrip('[')
    elif payload.startswith('submitted='):
        row['submitted'] = payload.endswith('true')
    else:
        row['observations'].append(payload)
pairs = collections.Counter((v.get('VS'), v.get('PS')) for v in draws.values())
report = {'log': str(a.log.resolve()), 'note': 'Distinct geometry samples, not total draws. First-use opacity can be zero during fades. Same shader hash does not prove asset identity. Trace overhead excludes FPS comparison.',
          'sample_count': len(draws), 'submitted': sum(v.get('submitted', False) for v in draws.values()),
          'pairs': [{'VS': vs, 'PS': ps, 'samples': n} for (vs, ps), n in pairs.items()],
          'draws': list(draws.values())}
a.output.parent.mkdir(parents=True, exist_ok=True)
a.output.write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps({k: report[k] for k in ('sample_count', 'submitted', 'pairs')}, indent=2))
