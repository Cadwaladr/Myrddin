#!/usr/bin/env python3
"""ppmdiff.py A.ppm B.ppm: mean absolute difference and the number of pixels that differ by more than 8."""
import sys, numpy as np
def read(path):
    with open(path, 'rb') as f:
        data = f.read()
    parts, pos = [], 0
    while len(parts) < 4:
        while data[pos:pos+1].isspace(): pos += 1
        if data[pos:pos+1] == b'#':
            pos = data.index(b'\n', pos) + 1; continue
        end = pos
        while not data[end:end+1].isspace(): end += 1
        parts.append(data[pos:end]); pos = end
    pos += 1
    w, h = int(parts[1]), int(parts[2])
    return np.frombuffer(data, dtype=np.uint8, count=w*h*3, offset=pos).reshape(h, w, 3).astype(np.int16)
a, b = read(sys.argv[1]), read(sys.argv[2])
d = np.abs(a - b)
print('mean %.4f, %d px > 8, max %d' % (d.mean(), (d.max(axis=2) > 8).sum(), d.max()))
