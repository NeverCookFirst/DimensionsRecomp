"""Generate rexlego/src/native_lock_elide.cpp from the codegen output without the global-lock enter/leave.

usage: gen_lock_elide.py [--check] [--scan]
A function qualifies when every locked region is one reservation load, register-only work and one conditional store.
--check fails when the checked-in file is stale; --scan lists every qualifying function in the image.
"""
import hashlib, pathlib, re, sys

SOURCE = pathlib.Path(__file__).resolve().parents[2]
GEN = SOURCE / 'rexlego/generated/default'
OUT = SOURCE / 'rexlego/src/native_lock_elide.cpp'

# Main-thread global-lock users: freelist pop/push, small-block alloc/free, string interning, XAPI queue enqueue.
FUNCS = ['82A2CAB0', '82A2CC20', '82A34E28', '82A34FE8', '82A4CA18', '82A35D28', '83F99CB8']

FENCE = 'std::atomic_thread_fence(std::memory_order_seq_cst);'
ENTER = 'REX_ENTER_GLOBAL_LOCK();'
LEAVE = 'REX_LEAVE_GLOBAL_LOCK();'
RE_CHECK = re.compile(r'^ctx\.r\d+\.u64 = REX_CHECK_GLOBAL_LOCK\(\);$')
RE_MSR = re.compile(r'^ctx\.msr = \(ctx\.r\d+\.u32 & 0x8020\) \| \(ctx\.msr & ~0x8020\);$')
RE_EA = re.compile(r'^ea = ctx\.r\d+\.u32( \+ ctx\.r\d+\.u32)?;$')
RE_RES = re.compile(r'^ctx\.reserved\.u(32|64) = \*\(uint(32|64)_t\*\)REX_RAW_ADDR\(ea\);$')
RE_SWAP = re.compile(r'^ctx\.r\d+\.u64 = __builtin_bswap(32|64)\(ctx\.reserved\.u(32|64)\);$')
RE_CAS = re.compile(r'^ctx\.cr0\.eq = __sync_bool_compare_and_swap\(reinterpret_cast<uint(32|64)_t\*>'
                    r'\(REX_RAW_ADDR\(ea\)\), ctx\.reserved\.s(32|64), __builtin_bswap(32|64)\(ctx\.r\d+\.s(32|64)\)\);$')
RE_CR0 = re.compile(r'^ctx\.cr0\.(lt|gt) = 0;$|^ctx\.cr0\.so = ctx\.xer\.so;$')
RE_CMP = re.compile(r'^ctx\.cr\d\.compare<u?int(32|64)_t>\(ctx\.r\d+\.[su](32|64), (ctx\.r\d+\.[su](32|64)|-?\d+), ctx\.xer\);$')
RE_GOTO = re.compile(r'^if \(!?ctx\.cr\d\.(eq|lt|gt|so)\) goto (loc_[0-9A-F]+);$')
RE_REGOP = re.compile(r'^ctx\.r\d+\.[su](32|64) = [^;]*;$')
RE_LABEL = re.compile(r'^(loc_[0-9A-F]+):$')


def bodies():
    out = {}
    for path in sorted(GEN.glob('legodimensions_recomp.*.cpp')):
        text = path.read_text()
        for m in re.finditer(r'^DEFINE_REX_FUNC\((sub_[0-9A-F]+)\) \{\n(.*?)^\}\n', text, re.M | re.S):
            if ENTER in m.group(2):
                out[m.group(1)] = (path, m.group(2))
    return out


def regop_ok(line):
    return RE_REGOP.match(line) and 'REX_' not in line and 'base' not in line and '*(' not in line \
        and 'reserved' not in line


def validate(body):
    """Returns None when every locked region is a single CAS, else the reason."""
    lines = [l.strip() for l in body.split('\n')]
    code = [(i, l) for i, l in enumerate(lines) if l and not l.startswith('//')]
    gotos = {}
    for k, (i, l) in enumerate(code):
        m = re.search(r'goto (loc_[0-9A-F]+);', l)
        if m: gotos.setdefault(m.group(1), []).append(k)
    inside = set()      # code indices inside ENTER segments
    store_labels = set()
    enters = [k for k, (_, l) in enumerate(code) if l == ENTER]
    leaves = [k for k, (_, l) in enumerate(code) if l == LEAVE]
    for k in enters:
        if k < 4 or code[k - 4][1] != FENCE or not RE_CHECK.match(code[k - 3][1]) \
                or code[k - 2][1] != FENCE or not RE_MSR.match(code[k - 1][1]):
            return 'enter at %d lacks the mfmsr/mtmsrd prologue' % k
        j = k + 1
        if not (RE_EA.match(code[j][1]) and RE_RES.match(code[j + 1][1]) and RE_SWAP.match(code[j + 2][1])):
            return 'enter at %d does not start with a reservation load' % k
        j += 3
        stores = 0
        while code[j][1] != LEAVE:
            l = code[j][1]
            if l in (ENTER,) or RE_LABEL.match(l) or 'return' in l or 'REX_CHECK' in l:
                return 'region at %d has %s' % (k, l)
            m = RE_GOTO.match(l)
            if m:
                store_labels.add(m.group(2))
            elif RE_CAS.match(l):
                stores += 1
            elif not (RE_EA.match(l) or RE_CR0.match(l) or RE_CMP.match(l) or l == FENCE or RE_MSR.match(l)
                      or regop_ok(l)):
                return 'region at %d has %s' % (k, l)
            inside.add(j)
            j += 1
        if stores != 1 or not RE_MSR.match(code[j - 1][1]) or code[j - 2][1] != FENCE or not RE_CAS.match(code[j - 4][1]):
            return 'region at %d does not end in one conditional store' % k
        inside.add(j)
    labels = {l[:-1]: k for k, (_, l) in enumerate(code) if RE_LABEL.match(l)}
    for name in store_labels:
        k = labels.get(name)
        if k is None: return 'missing label %s' % name
        if any(g not in inside for g in gotos.get(name, [])): return '%s reached from outside a region' % name
        prev = code[k - 1][1]
        if not (prev.startswith('goto ') or prev == 'return;'): return '%s is reached by fall-through' % name
        seq = [l for _, l in code[k + 1:k + 10]]
        if not (RE_EA.match(seq[0]) and RE_CR0.match(seq[1]) and RE_CR0.match(seq[2]) and RE_CAS.match(seq[3])
                and RE_CR0.match(seq[4]) and seq[5] == FENCE and RE_MSR.match(seq[6]) and seq[7] == LEAVE):
            return '%s is not a bare conditional store' % name
        inside.add(k + 8)
    for k in leaves:
        if k not in inside: return 'leave at %d outside a checked region' % k
    if body.count('REX_CHECK_GLOBAL_LOCK') != len(enters): return 'unpaired mfmsr'
    return None


def elide(body):
    out = []
    for line in body.split('\n'):
        s = line.strip()
        if s in (ENTER, LEAVE): continue
        out.append(line.replace('REX_CHECK_GLOBAL_LOCK()', 'LockElidedMsr()'))
    return '\n'.join(out)


def generate(all_bodies):
    parts = ['// Generated by tools/native-cpu/gen_lock_elide.py from the codegen output; do not edit.',
             '// Each body is the generated function with only the global-lock enter/leave removed.',
             '#include "legodimensions_pch.h"', '#include <rex/hook.h>', '', '#include <cstdlib>', '#include <cstring>', '']
    headers = sorted({p.name.replace('recomp', 'funcs').replace('.cpp', '.h') for p, _ in
                      (all_bodies['sub_' + f] for f in FUNCS)})
    parts += ['#include "%s"' % h for h in headers]
    parts += [''] + ['extern "C" void __imp__sub_%s(PPCContext& __restrict ctx, uint8_t* base);' % f for f in FUNCS]
    parts += ['', 'namespace {', 'bool Enabled() {', '  static const bool enabled = [] {',
              '    const char* value = std::getenv("LEGO_NATIVE_LOCK_ELIDE");',
              '    return !value || std::strcmp(value, "0") != 0;', '  }();', '  return enabled;', '}', '',
              '// The value mfmsr would report, without taking the mutex.',
              'u64 LockElidedMsr() { return ppc_global_lock_count_().load() ? 0 : 0x8000; }', '}', '']
    for f in FUNCS:
        name = 'sub_' + f
        path, body = all_bodies[name]
        digest = hashlib.sha256(body.encode()).hexdigest()[:16]
        parts += ['// %s from %s, body sha256 %s.' % (name, path.name, digest),
                  'static void Elided_%s(PPCContext& __restrict ctx, uint8_t* base) {' % f,
                  elide(body).rstrip('\n'), '}', '',
                  'REX_HOOK_RAW(%s) {' % name,
                  '  if (!Enabled()) { __imp__%s(ctx, base); return; }' % name,
                  '  Elided_%s(ctx, base);' % f, '}', '']
    return '\n'.join(parts)


def main():
    all_bodies = bodies()
    if '--scan' in sys.argv:
        ok = [n for n, (_, b) in sorted(all_bodies.items()) if validate(b) is None]
        print('%d of %d lock users qualify' % (len(ok), len(all_bodies)))
        for n in ok: print(' ', n)
        return 0
    for f in FUNCS:
        name = 'sub_' + f
        if name not in all_bodies: sys.exit('%s has no global lock in the codegen output' % name)
        why = validate(all_bodies[name][1])
        if why: sys.exit('%s rejected: %s' % (name, why))
    text = generate(all_bodies)
    if '--check' in sys.argv:
        if not OUT.exists() or OUT.read_text() != text: sys.exit('native_lock_elide.cpp is stale; rerun gen_lock_elide.py')
        print('native_lock_elide.cpp is current'); return 0
    OUT.write_text(text)
    print('wrote %s (%d functions)' % (OUT, len(FUNCS)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
