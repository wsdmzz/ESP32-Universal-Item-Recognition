#!/usr/bin/env python3
"""devlog.py — 把板子串口日志持续抄到文件，方便边调 HTTP 边看端侧发生了什么。

注意：打开 /dev/ttyACM0 会让 S3 复位一次（实测过），所以别在采集跑到一半时启动它。
用法: python3 tools/devlog.py /tmp/devlog.txt 1800
"""
import sys
import time
from pathlib import Path

import serial

port = sys.argv[1] if len(sys.argv) > 1 else '/tmp/devlog.txt'
secs = float(sys.argv[2]) if len(sys.argv) > 2 else 600.0

s = serial.Serial()
s.port = '/dev/ttyACM0'
s.baudrate = 115200
s.dtr = False
s.rts = False
s.open()

t0 = time.time()
with open(port, 'ab', buffering=0) as f:
    f.write(f'\n===== devlog 开始 {time.strftime("%H:%M:%S")} =====\n'.encode())
    while time.time() - t0 < secs:
        n = s.in_waiting
        if n:
            f.write(s.read(n))
        else:
            time.sleep(0.05)
s.close()
print('devlog done ->', port)
