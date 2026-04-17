#!/usr/bin/env python3
"""Generate wavetables/factory/default.wav: 4-frame 16-bit mono WAV.
Frame 0: sine, Frame 1: triangle, Frame 2: sawtooth, Frame 3: square.
Each frame is 1024 samples at 44100 Hz."""

import struct
import math
import os

FRAME_SIZE = 1024
NUM_FRAMES = 4
SAMPLE_RATE = 44100
NUM_SAMPLES = FRAME_SIZE * NUM_FRAMES

samples = []

for frame in range(NUM_FRAMES):
    for i in range(FRAME_SIZE):
        t = i / FRAME_SIZE
        if frame == 0:
            v = math.sin(2.0 * math.pi * t)
        elif frame == 1:
            # Triangle: 0→1 in first half, 1→-1 in second half
            if t < 0.5:
                v = 4.0 * t - 1.0
            else:
                v = 3.0 - 4.0 * t
        elif frame == 2:
            v = 2.0 * t - 1.0
        else:
            v = 1.0 if t < 0.5 else -1.0
        samples.append(max(-1.0, min(1.0, v)))

pcm = bytes()
for v in samples:
    s = int(v * 32767)
    pcm += struct.pack('<h', s)

data_size = len(pcm)
fmt_size = 16
chunk_size = 4 + (8 + fmt_size) + (8 + data_size)

out_path = os.path.join(os.path.dirname(__file__), '..', 'wavetables', 'factory', 'default.wav')
with open(out_path, 'wb') as f:
    f.write(b'RIFF')
    f.write(struct.pack('<I', chunk_size))
    f.write(b'WAVE')
    f.write(b'fmt ')
    f.write(struct.pack('<I', fmt_size))
    f.write(struct.pack('<H', 1))           # PCM
    f.write(struct.pack('<H', 1))           # mono
    f.write(struct.pack('<I', SAMPLE_RATE))
    f.write(struct.pack('<I', SAMPLE_RATE * 2))  # byte rate
    f.write(struct.pack('<H', 2))           # block align
    f.write(struct.pack('<H', 16))          # bits per sample
    f.write(b'data')
    f.write(struct.pack('<I', data_size))
    f.write(pcm)

print(f"Wrote {NUM_FRAMES} frames × {FRAME_SIZE} samples → {out_path}")
print(f"File size: {os.path.getsize(out_path)} bytes")
