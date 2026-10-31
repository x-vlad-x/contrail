#!/usr/bin/env python3
"""Finds breaks in a recorded 440 Hz tone, the clicks an ear would hear (see tone-probe.cpp).

  tone-gaps.py FILE [SECONDS_SKIPPED_AT_START]

FILE is raw s16le stereo at 48 kHz, as from `parec --device=SINK.monitor`. A break is a run of at least 2 ms of
silence after the tone started. Prints each one and the total.
"""
import array
import sys

RATE = 48000
path = sys.argv[1]
skip = float(sys.argv[2]) if len(sys.argv) > 2 else 0.0
data = array.array('h')
with open(path, 'rb') as f:
    data.frombytes(f.read())
left = data[::2]
peak = max((abs(v) for v in left), default=0)
print(f'{len(left) / RATE:.2f} s, peak {peak}')
if peak < 8:
    print('NO SIGNAL')
    sys.exit(2)

limit = max(1, peak // 50)
minimum = RATE * 2 // 1000
# One period of 440 Hz is 109 samples, so the tone itself is never silent for 2 ms... except near the top of a
# coarse rounding at a low level, which is why the limit is relative to the peak.
window = 120
loud = [False] * len(left)
last = -window
for i, value in enumerate(left):
    if abs(value) > limit:
        last = i
    loud[i] = (i - last) < window

first = next((i for i, value in enumerate(loud) if value), 0)
end = len(loud) - 1
while end > first and not loud[end]:
    end -= 1
start = max(first, int(skip * RATE))
gaps = []
run = 0
for i in range(start, end):
    if loud[i]:
        if run >= minimum:
            gaps.append((i - run, run))
        run = 0
    else:
        run += 1
for at, length in gaps:
    print(f'  break at {at / RATE:7.3f} s, {length * 1000 / RATE:.1f} ms')
seconds = (end - start) / RATE
print(f'RESULT {len(gaps)} breaks in {seconds:.1f} s of tone, {len(gaps) / max(seconds, 0.001):.2f}/s')
