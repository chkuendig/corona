# Reads the frame tap's FIFO for the whole test and writes one line per frame:
#   <seq> <width> <height> <capture_ns> <centre colour>
# then "EOF" if the writer ever closes the stream. The fd stays open across
# the reload: reopening would hide whether the stream survives it.
import struct
import sys

fifo, out_path = sys.argv[1], sys.argv[2]
out = open(out_path, "w", buffering=1)
f = open(fifo, "rb", buffering=0)


def read_exact(n):
    buf = bytearray()
    while len(buf) < n:
        chunk = f.read(n - len(buf))
        if not chunk:
            return None
        buf += chunk
    return bytes(buf)


def colour(b, g, r):
    if r > 200 and g < 60 and b < 60:
        return "red"
    if b > 200 and r < 60 and g < 60:
        return "blue"
    return "other"


while True:
    header = read_exact(64)
    if header is None:
        out.write("EOF\n")
        break
    if header[:4] != b"S2VT":
        out.write("BADMAGIC\n")
        break
    width, height, stride = struct.unpack_from("<III", header, 8)
    seq, capture_ns = struct.unpack_from("<QQ", header, 32)
    payload = read_exact(stride * height)
    if payload is None:
        out.write("EOF\n")
        break
    # BGRA; rows are bottom-up, which does not move the centre.
    at = (height // 2) * stride + (width // 2) * 4
    out.write("%d %d %d %d %s\n" % (seq, width, height, capture_ns,
                                     colour(payload[at], payload[at + 1], payload[at + 2])))
