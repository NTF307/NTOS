#!/usr/bin/env python3
"""mkinitrd.py - pack user programs into a ustar archive (NTOS initrd).

Each FILE is stored as bin/<name without extension>.
"""
import argparse
import io
import os
import tarfile


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', required=True)
    ap.add_argument('files', nargs='+')
    args = ap.parse_args()

    with tarfile.open(args.out, 'w', format=tarfile.USTAR_FORMAT) as tar:
        for path in sorted(args.files):
            data = open(path, 'rb').read()
            info = tarfile.TarInfo('bin/' + os.path.splitext(os.path.basename(path))[0])
            info.size = len(data)
            info.mode = 0o755
            info.mtime = 0
            tar.addfile(info, io.BytesIO(data))
    print(f'mkinitrd: {len(args.files)} file(s), {os.path.getsize(args.out)} bytes -> {args.out}')


if __name__ == '__main__':
    main()
