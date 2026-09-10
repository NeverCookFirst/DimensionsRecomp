"""Find every load/store that touches a guest address via lis + d-form.

ppcref.py catches lis+addi (taking an address). This catches lis+lwz/stw/lbz/stb
(using one), which is how a global is actually read and written.
"""
import struct, sys
BASE = 0x82000000
data = open(r"E:\Claude\LEGO Dimensions\xexdump\dump\default.bin", "rb").read()
OPS = {32:"lwz", 33:"lwzu", 34:"lbz", 36:"stw", 37:"stwu", 38:"stb", 40:"lhz", 42:"lha", 44:"sth"}
STORES = {36, 37, 38, 44}

def scan(target):
    hits = []
    pend = {}
    n = len(data) // 4 * 4
    for off in range(0, n, 4):
        w = struct.unpack_from(">I", data, off)[0]
        op = w >> 26
        if op == 15:                                   # lis
            rD, rA = (w >> 21) & 31, (w >> 16) & 31
            if rA == 0:
                pend[rD] = ((w & 0xFFFF) << 16, BASE + off)
            else:
                pend.pop(rD, None)
        elif op in OPS:
            rA = (w >> 16) & 31
            d = w & 0xFFFF
            if d & 0x8000: d -= 0x10000
            p = pend.get(rA)
            if p and (p[0] + d) & 0xFFFFFFFF == target:
                hits.append((BASE + off, OPS[op], op in STORES))
            rD = (w >> 21) & 31
            if op in STORES: pass
            elif rD != rA: pend.pop(rD, None)
    return hits

for a in sys.argv[1:]:
    t = int(a, 16)
    hits = scan(t)
    print(f"=== 0x{t:08X}: {len(hits)} access site(s)")
    for at, name, is_store in hits:
        print(f"    0x{at:08X}  {name:5} {'WRITE' if is_store else 'read'}")
