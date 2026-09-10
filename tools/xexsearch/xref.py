"""Find big-endian 32-bit references to a guest address inside the mapped image."""
import struct, sys

BASE = 0x82000000
DUMP = r"E:\Claude\LEGO Dimensions\xexdump\dump\default.bin"
data = open(DUMP, "rb").read()

def find(addr):
    needle = struct.pack(">I", addr)
    out, i = [], 0
    while True:
        i = data.find(needle, i)
        if i < 0: break
        out.append(BASE + i)
        i += 1
    return out

for a in sys.argv[1:]:
    addr = int(a, 16)
    hits = find(addr)
    print(f"=== refs to 0x{addr:08X}: {len(hits)}")
    for h in hits[:40]:
        print(f"    0x{h:08X}   (aligned)" if h % 4 == 0 else f"    0x{h:08X}   unaligned")
