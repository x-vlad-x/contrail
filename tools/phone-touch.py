#!/usr/bin/env python3
"""Inject one touchscreen drag for UI regression testing through an accessible evdev device.

Usage: python3 phone-touch.py /dev/input/event3 X Y DX DY [duration]
Coordinates are raw device coordinates (inspect the printed bounds first).
Refuses to run while a physical finger is already down; never grabs the device.
"""
import array
import fcntl
import os
import struct
import sys
import time

EV_SYN, EV_KEY, EV_ABS = 0, 1, 3
SLOT, X, Y, TRACKING = 47, 53, 54, 57
BTN_TOUCH = 330

def ioctl_read(fd, number, size, initial=None):
    data = bytearray(initial or bytes(size))
    fcntl.ioctl(fd, (2 << 30) | (size << 16) | (ord('E') << 8) | number, data, True)
    return data

def abs_info(fd, code):
    return struct.unpack('6i', ioctl_read(fd, 0x40 + code, 24))

def emit(fd, events):
    now = time.monotonic()
    seconds, micros = int(now), int((now % 1) * 1_000_000)
    payload = b''.join(struct.pack('llHHi', seconds, micros, kind, code, value)
                       for kind, code, value in [*events, (EV_SYN, 0, 0)])
    os.write(fd, payload)

def main():
    fd = os.open(sys.argv[1], os.O_RDWR | os.O_NONBLOCK)
    pressed = False
    try:
        name = ioctl_read(fd, 0x06, 256).split(b'\0', 1)[0].decode()
        if 'fts' not in name.lower() and 'touch' not in name.lower():
            raise RuntimeError('Not a touchscreen: ' + name)
        _, xmin, xmax, *_ = abs_info(fd, X)
        _, ymin, ymax, *_ = abs_info(fd, Y)
        print(f'{name}: x={xmin}..{xmax}, y={ymin}..{ymax}', flush=True)
        if len(sys.argv) == 2:
            return
        _, slot_min, slot_max, *_ = abs_info(fd, SLOT)
        slots = array.array('i', [TRACKING] + [-1] * (slot_max + 1))
        states = array.array('i')
        states.frombytes(ioctl_read(fd, 0x0a, len(slots.tobytes()), slots.tobytes()))
        if any(value >= 0 for value in states[1:]):
            raise RuntimeError('A physical touch is active; refusing to inject')
        x, y, dx, dy = map(int, sys.argv[2:6])
        duration = float(sys.argv[6]) if len(sys.argv) > 6 else 0.4
        if not (xmin <= x <= xmax and xmin <= x + dx <= xmax
                and ymin <= y <= ymax and ymin <= y + dy <= ymax):
            raise RuntimeError('Drag outside touchscreen bounds')
        emit(fd, [(EV_ABS, SLOT, slot_min), (EV_ABS, TRACKING, 1234),
                  (EV_ABS, X, x), (EV_ABS, Y, y), (EV_ABS, 48, 8), (EV_KEY, BTN_TOUCH, 1)])
        pressed = True
        for step in range(1, 25):
            time.sleep(duration / 24)
            emit(fd, [(EV_ABS, SLOT, slot_min), (EV_ABS, X, x + dx * step // 24),
                      (EV_ABS, Y, y + dy * step // 24)])
    finally:
        if pressed:
            emit(fd, [(EV_ABS, SLOT, 0), (EV_ABS, TRACKING, -1), (EV_KEY, BTN_TOUCH, 0)])
        os.close(fd)

if __name__ == '__main__':
    main()
