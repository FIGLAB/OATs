# usage: mtp_parse.py capture.txt out.bin  -- turns "MTP xxx: .." lines into a binary file and prints a summary
import sys, re, collections
lines = open(sys.argv[1], errors="replace").read().splitlines()
data = {}
for ln in lines:
    m = re.match(r"MTP ([0-9A-Fa-f]{3,4}):((?: [0-9A-Fa-f]{2})+)", ln.strip())
    if m:
        addr = int(m.group(1), 16)
        for i, h in enumerate(m.group(2).split()):
            data[addr + i] = int(h, 16)
if not data:
    print("no MTP lines found"); sys.exit(1)
lo, hi = min(data), max(data)
buf = bytes(data.get(a, 0) for a in range(lo, hi + 1))
open(sys.argv[2], "wb").write(buf)
print("bytes %d (0x%03X..0x%03X), missing %d" % (len(buf), lo, hi, (hi - lo + 1) - len(data)))
c = collections.Counter(buf)
print("most common:", ", ".join("%02X x%d" % kv for kv in c.most_common(8)))
# settings area per datasheet 9.3
if hi >= 0xF84:
    g = lambda a: data.get(a, -1)
    print("F59 enable=%02X  PSR=%02X %02X  PWR=%02X %02X %02X %02X %02X %02X  BTST=%s  PLL=%02X CDI=%02X  TRES=%02X %02X %02X %02X  VDCS=%02X" % (
        g(0xF59), g(0xF5C), g(0xF5D), g(0xF5E), g(0xF5F), g(0xF60), g(0xF61), g(0xF62), g(0xF63),
        " ".join("%02X" % g(a) for a in range(0xF68, 0xF6F)), g(0xF70), g(0xF71), g(0xF74), g(0xF75), g(0xF76), g(0xF77), g(0xF7C)))
# where does the waveform area stop being non-FF?
last = max((a for a in range(lo, min(hi, 0xEFF) + 1) if data.get(a, 0xFF) != 0xFF), default=None)
print("last non-FF byte in the waveform area: %s" % (("0x%03X" % last) if last is not None else "none"))
