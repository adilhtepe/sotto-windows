# Counts VBAN packets on a UDP port for a few seconds and prints the first header (CI audio test).
import socket, struct, sys, time
port, secs = int(sys.argv[1]), float(sys.argv[2])
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind(("0.0.0.0", port)); s.settimeout(0.5)
n, first, last_fc, gaps, end = 0, None, None, 0, time.time() + secs
while time.time() < end:
    try:
        d, a = s.recvfrom(2048)
    except socket.timeout:
        continue
    if len(d) < 28 or d[:4] != b"VBAN":
        continue
    fc = struct.unpack("<I", d[24:28])[0]
    if last_fc is not None and fc != last_fc + 1:
        gaps += 1
    last_fc = fc; n += 1
    if first is None:
        first = (a[0], d[4], d[5] + 1, d[6] + 1, d[7], d[8:24].rstrip(b"\0").decode(errors="replace"), len(d))
print("packets", n, "gaps", gaps)
if first:
    print("from %s sr_index %d frames %d channels %d datatype %d stream %r size %d" % first)
