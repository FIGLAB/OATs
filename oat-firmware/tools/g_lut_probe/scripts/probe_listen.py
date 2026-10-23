# usage: probe_listen.py PORT max_seconds [stop_substring]  -- print what the probe says until stop text or timeout
import sys, time, serial
port, limit = sys.argv[1], float(sys.argv[2])
stop = sys.argv[3] if len(sys.argv) > 3 else None
with serial.Serial(port, 115200, timeout=0.2) as s:
    t0 = time.time(); buf = b""
    while time.time() - t0 < limit:
        d = s.read(4096)
        if d:
            buf += d
            if stop and stop.encode() in buf: break
    sys.stdout.write(buf.decode(errors="replace"))
    print("\n[probe_listen: %.1f s]" % (time.time() - t0))
