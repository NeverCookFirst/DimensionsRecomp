import sys, re
BASE=0x82000000
data=open(r"E:\Claude\LEGO Dimensions\xexdump\dump\default.bin","rb").read()
lo=int(sys.argv[1],16); hi=int(sys.argv[2],16)
seg=data[lo-BASE:hi-BASE]
for m in re.finditer(rb"[ -~]{5,}", seg):
    print(f"0x{lo+m.start():08X}  {m.group().decode()}")
