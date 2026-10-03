"""Group bounded mesh samples by resource provenance, without asset attribution."""
import argparse
import collections
import json
import re
from pathlib import Path

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('mesh', type=Path)
p.add_argument('output', type=Path)
a = p.parse_args()
groups = collections.defaultdict(list)
for draw in json.loads(a.mesh.read_text())['draws']:
    if not any('brick_pair=true' in o for o in draw['observations']):
        continue
    observations = draw['observations']
    resource = next((o for o in observations if o.startswith('stream=0 data=')), '')
    index = next((o for o in observations if o.startswith('index_data=')), '')
    fields = dict(re.findall(r'(\w+)=([^ ]+)', resource + ' ' + index))
    key = (draw['IB'], fields.get('data'), fields.get('index_data'), draw['start'], draw['count'])
    constants = {}
    for o in observations:
        m = re.fullmatch(r'c(\d+)=\(([^)]+)\)', o)
        if m:
            constants[int(m[1])] = [float(v) for v in m[2].split(',')]
    frame = next((int(re.search(r'frame=(\d+)', o)[1]) for o in observations if o.startswith('frame=')), None)
    groups[key].append({'id':draw['id'], 'time':draw['timestamp'], 'frame':frame,
        'resource':resource, 'index':index, 'constants':constants})
report = {'note':'Bounded samples only. Shader/resource identity does not prove Vortech ownership. Stable geometry with varying instance matrices can be normal individual brick drawing.',
    'groups':[{'IB':key[0], 'vertex_data':key[1], 'index_data':key[2],
        'start':key[3], 'count':key[4], 'samples':rows,
        'distinct_uploads':len({r['resource'] for r in rows}),
        'distinct_instance_matrices':len({str([r['constants'].get(c) for c in range(48,52)]) for r in rows})}
        for key,rows in groups.items()]}
a.output.write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps([{k:g[k] for k in ('IB','vertex_data','index_data','start','count','distinct_uploads','distinct_instance_matrices')} for g in report['groups']],indent=2))
