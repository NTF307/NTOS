#!/usr/bin/env python3
"""
qemu_test.py - boot an NTOS ISO headless in QEMU (UEFI), wait for the shell,
type commands, and save a screenshot (PNG) plus the serial log.

Exit code 0 = kernel reached the shell, 1 = it did not.
"""
import argparse
import json
import os
import shutil
import socket
import struct
import subprocess
import sys
import time
import zlib

QEMU_DIR = r'C:\Program Files\qemu'
READY = b'NTOS shell ready'

QCODES = {' ': 'spc', '\n': 'ret', '-': 'minus', '=': 'equal', '.': 'dot', ',': 'comma',
          '/': 'slash', ';': 'semicolon', '\'': 'apostrophe', '[': 'bracket_left',
          ']': 'bracket_right', '\\': 'backslash', '`': 'grave_accent'}
SHIFTED = {'_': '-', '+': '=', ':': ';', '"': '\'', '{': '[', '}': ']', '|': '\\', '~': '`',
           '!': '1', '@': '2', '#': '3', '$': '4', '%': '5', '^': '6', '&': '7', '*': '8',
           '(': '9', ')': '0', '<': ',', '>': '.', '?': '/'}


class QMP:
    def __init__(self, port):
        for _ in range(100):
            try:
                self.sock = socket.create_connection(('127.0.0.1', port), timeout=5)
                break
            except OSError:
                time.sleep(0.1)
        else:
            raise RuntimeError('cannot connect to QMP')
        self.f = self.sock.makefile('rwb')
        self._read()                                   # greeting
        self.cmd('qmp_capabilities')

    def _read(self):
        while True:
            msg = json.loads(self.f.readline())
            if 'event' not in msg:
                return msg

    def cmd(self, name, **args):
        self.f.write(json.dumps({'execute': name, 'arguments': args}).encode() + b'\n')
        self.f.flush()
        return self._read()

    def key(self, ch):
        keys = []
        if ch in SHIFTED:
            keys = [{'type': 'qcode', 'data': 'shift'}]
            ch = SHIFTED[ch]
        elif ch.isupper():
            keys = [{'type': 'qcode', 'data': 'shift'}]
            ch = ch.lower()
        keys.append({'type': 'qcode', 'data': QCODES.get(ch, ch)})
        self.cmd('send-key', keys=keys)
        time.sleep(0.03)


def ppm_to_png(ppm_path, png_path):
    with open(ppm_path, 'rb') as f:
        data = f.read()
    parts, pos = [], 0
    while len(parts) < 4:                              # P6 width height maxval
        while data[pos:pos + 1].isspace():
            pos += 1
        start = pos
        while not data[pos:pos + 1].isspace():
            pos += 1
        parts.append(data[start:pos])
    pos += 1
    w, h = int(parts[1]), int(parts[2])
    pixels = data[pos:pos + w * h * 3]
    raw = b''.join(b'\x00' + pixels[y * w * 3:(y + 1) * w * 3] for y in range(h))

    def chunk(tag, body):
        return struct.pack('>I', len(body)) + tag + body + struct.pack('>I', zlib.crc32(tag + body) & 0xFFFFFFFF)

    png = (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) +
           chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))
    with open(png_path, 'wb') as f:
        f.write(png)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('iso')
    ap.add_argument('--out', default='build/test', help='directory for serial.log and screen.png')
    ap.add_argument('--type', action='append', default=[], help='command to type at the shell (repeatable)')
    ap.add_argument('--timeout', type=float, default=60)
    ap.add_argument('--port', type=int, default=4447)
    args = ap.parse_args()

    os.makedirs(args.out, exist_ok=True)
    serial = os.path.abspath(os.path.join(args.out, 'serial.log'))
    shot_ppm = os.path.abspath(os.path.join(args.out, 'screen.ppm'))
    shot_png = os.path.abspath(os.path.join(args.out, 'screen.png'))
    vars_fd = os.path.abspath(os.path.join(args.out, 'vars.fd'))
    shutil.copyfile(os.path.join(QEMU_DIR, 'share', 'edk2-i386-vars.fd'), vars_fd)
    if os.path.exists(serial):
        os.remove(serial)

    cmd = [os.path.join(QEMU_DIR, 'qemu-system-x86_64.exe'),
           '-machine', 'q35', '-m', '512M', '-no-reboot',
           '-drive', 'if=pflash,format=raw,unit=0,readonly=on,file=' + os.path.join(QEMU_DIR, 'share', 'edk2-x86_64-code.fd'),
           '-drive', 'if=pflash,format=raw,unit=1,file=' + vars_fd,
           '-cdrom', os.path.abspath(args.iso),
           '-serial', 'file:' + serial,
           '-display', 'none',
           '-qmp', f'tcp:127.0.0.1:{args.port},server,wait=off']
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    ok = False
    try:
        qmp = QMP(args.port)
        deadline = time.time() + args.timeout
        while time.time() < deadline:
            if proc.poll() is not None:
                break
            if os.path.exists(serial) and READY in open(serial, 'rb').read():
                ok = True
                break
            time.sleep(0.25)
        if ok:
            time.sleep(0.5)
            for line in args.type:
                for ch in line:
                    qmp.key(ch)
                qmp.key('\n')
                time.sleep(0.6)
        time.sleep(0.5)
        if proc.poll() is None:
            qmp.cmd('screendump', filename=shot_ppm)
            time.sleep(0.3)
            ppm_to_png(shot_ppm, shot_png)
            os.remove(shot_ppm)
            qmp.cmd('quit')
    finally:
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
        out = proc.stdout.read().decode(errors='replace').strip()
        if out:
            print('qemu:', out)

    log = open(serial, 'rb').read().decode(errors='replace') if os.path.exists(serial) else ''
    print('---- serial log ----')
    print(log.rstrip())
    print('--------------------')
    print(f'screenshot: {shot_png}' if os.path.exists(shot_png) else 'screenshot: (none)')
    print('RESULT:', 'PASS - kernel reached the shell' if ok else 'FAIL - shell not reached')
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
