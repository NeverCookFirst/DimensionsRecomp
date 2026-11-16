"""Check that each generated __savegprlr/__restgprlr/__savefpr/__restfpr_N (N 14..31) is exactly the model native_saverest.cpp implements.

usage: check_saverest.py
"""
import pathlib, re, sys

GEN = pathlib.Path(__file__).resolve().parents[2] / 'rexlego/generated/default'


def expected(kind, n):
    out = []
    if kind in ('savefpr', 'restfpr'): out.append('ctx.fpscr.disableFlushMode();')
    for k in range(n, 32):
        if kind == 'savegprlr': out.append('REX_STORE_U64(ctx.r1.u32 + -%d, ctx.r%d.u64);' % (8 * (33 - k), k))
        if kind == 'restgprlr': out.append('ctx.r%d.u64 = REX_LOAD_U64(ctx.r1.u32 + -%d);' % (k, 8 * (33 - k)))
        if kind == 'savefpr': out.append('REX_STORE_U64(ctx.r12.u32 + -%d, ctx.f%d.u64);' % (8 * (32 - k), k))
        if kind == 'restfpr': out.append('ctx.f%d.u64 = REX_LOAD_U64(ctx.r12.u32 + -%d);' % (k, 8 * (32 - k)))
    if kind == 'savegprlr': out.append('REX_STORE_U32(ctx.r1.u32 + -8, ctx.r12.u32);')
    if kind == 'restgprlr': out += ['ctx.r12.u64 = REX_LOAD_U32(ctx.r1.u32 + -8);', 'ctx.lr = ctx.r12.u64;']
    return ['REX_FUNC_PROLOGUE();'] + out + ['return;']


found = {}
for path in GEN.glob('legodimensions_recomp.*.cpp'):
    for m in re.finditer(r'^DEFINE_REX_FUNC\(__(savegprlr|restgprlr|savefpr|restfpr)_(\d+)\) \{\n(.*?)^\}\n',
                         path.read_text(), re.M | re.S):
        lines = [l.strip() for l in m.group(3).split('\n')]
        found[(m.group(1), int(m.group(2)))] = [l for l in lines if l and not l.startswith('//')]
bad = 0
for kind in ('savegprlr', 'restgprlr', 'savefpr', 'restfpr'):
    for n in range(14, 32):
        body = found.get((kind, n))
        if body is None:
            print('missing __%s_%d' % (kind, n)); bad += 1
        elif body != expected(kind, n):
            print('__%s_%d differs from the model' % (kind, n)); bad += 1
print('%d of 72 helpers match' % (72 - bad))
sys.exit(1 if bad else 0)
