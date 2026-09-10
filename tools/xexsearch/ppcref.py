"""Find PowerPC code that materialises a guest address.

A 32-bit constant never appears as a word in PPC code: it is built with
lis rD,hi then addi/ori rD,rD,lo. This walks the image as instructions and
reports every rD that ends up holding the target address.
"""
import struct, sys, collections

BASE = 0x82000000
DUMP = r"E:\Claude\LEGO Dimensions\xexdump\dump\default.bin"
data = open(DUMP, "rb").read()

def halves(addr):
    """(hi, lo) as the assembler would emit them."""
    lo = addr & 0xFFFF
    hi = (addr >> 16) & 0xFFFF
    if lo & 0x8000:            # addi sign-extends, so hi is pre-incremented
        hi = (hi + 1) & 0xFFFF
    return hi, lo

def scan(target):
    hi_addi, lo = halves(target)
    hi_ori = (target >> 16) & 0xFFFF     # ori does not sign-extend
    hits = []
    pending = {}                          # rD -> (hi, addr_of_lis)
    n = len(data) // 4 * 4
    for off in range(0, n, 4):
        w = struct.unpack_from(">I", data, off)[0]
        op = w >> 26
        if op == 15:                                  # addis / lis
            rD = (w >> 21) & 31
            rA = (w >> 16) & 31
            imm = w & 0xFFFF
            if rA == 0:
                pending[rD] = (imm, BASE + off)
            else:
                pending.pop(rD, None)
        elif op == 14:                                # addi
            rD = (w >> 21) & 31
            rA = (w >> 16) & 31
            imm = w & 0xFFFF
            p = pending.get(rA)
            if p and rA != 0 and p[0] == hi_addi and imm == lo:
                hits.append((p[1], BASE + off))
            if rD != rA:
                pending.pop(rD, None)
        elif op == 24:                                # ori
            rS = (w >> 21) & 31
            rA = (w >> 16) & 31
            imm = w & 0xFFFF
            p = pending.get(rS)
            if p and p[0] == hi_ori and imm == lo:
                hits.append((p[1], BASE + off))
            if rA != rS:
                pending.pop(rA, None)
    return hits

for a in sys.argv[1:]:
    t = int(a, 16)
    hits = scan(t)
    print(f"=== code building 0x{t:08X}: {len(hits)} site(s)")
    for lis_at, lo_at in hits[:40]:
        print(f"    lis @ 0x{lis_at:08X}   lo @ 0x{lo_at:08X}")
