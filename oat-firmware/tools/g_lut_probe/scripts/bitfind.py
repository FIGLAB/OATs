# usage: bitfind.py mtp.bin "hex pattern" [min_bits]  -- find the bit pattern at any bit offset in the waveform area
import sys
buf = open(sys.argv[1], "rb").read()[:0xF00]
pat = bytes.fromhex(sys.argv[2].replace(" ", ""))
nb = len(pat) * 8
big = int.from_bytes(buf, "big"); total = len(buf) * 8
pv = int.from_bytes(pat, "big"); mask = (1 << nb) - 1
hits = []
for bit in range(0, total - nb + 1):
    if (big >> (total - nb - bit)) & mask == pv:
        hits.append(bit)
print("pattern %s (%d bits): %d hits" % (sys.argv[2], nb, len(hits)))
for h in hits[:60]:
    print("  byte 0x%03X bit %d" % (h // 8, h % 8), end="")
print()
