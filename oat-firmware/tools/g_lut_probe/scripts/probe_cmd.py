# usage: probe_cmd.py PORT "command" [max_seconds]  -- send one line, print output up to the "## done" marker
import sys, time, serial
port, cmd = sys.argv[1], sys.argv[2]
limit = float(sys.argv[3]) if len(sys.argv) > 3 else 90.0
s = None; t_open = time.time()
while s is None:
    try: s = serial.Serial(port, 115200, timeout=0.2)
    except Exception as e:
        if time.time() - t_open > 15: print("cannot open port:", e); sys.exit(1)
        time.sleep(0.5)
with s:
    time.sleep(0.2); s.reset_input_buffer()
    s.write((cmd + "\n").encode()); s.flush()
    t0 = time.time(); buf = b""
    while time.time() - t0 < limit:
        d = s.read(4096)
        if d:
            buf += d
            if b"## done" in buf: break
    txt = buf.decode(errors="replace").replace("## done", "").rstrip()
    print(txt); print("[%.1f s]" % (time.time() - t0))
