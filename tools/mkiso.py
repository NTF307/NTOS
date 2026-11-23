#!/usr/bin/env python3
"""
mkiso.py - build a UEFI-bootable NTOS ISO with no external tools.

The image contains:
  * an ISO 9660 file system with every file from --esp (so tools like Rufus can
    copy them onto a FAT32 USB stick) plus BOOT/EFIBOOT.IMG
  * an El Torito boot catalog with one EFI (platform 0xEF) no-emulation entry
    pointing at EFIBOOT.IMG, a FAT16 image holding the same files
  * an MBR in the system area with a type 0xEF partition covering EFIBOOT.IMG,
    so the raw .iso written to a USB stick (dd, Rufus "DD mode") also boots

Usage: python mkiso.py --esp build/esp --out build/ntos.iso [--label NTOS]
"""
import argparse
import os
import struct
import sys
import time

ISO_SECTOR = 2048
FAT_SECTOR = 512


# --------------------------------------------------------------------- tree
class Node:
    def __init__(self, name, parent=None, data=None):
        self.name = name
        self.parent = parent
        self.data = data                      # None for directories
        self.children = {} if data is None else None
        self.cluster = 0
        self.lba = 0
        self.size = 0

    @property
    def is_dir(self):
        return self.data is None


def check_83(name, is_dir):
    base, _, ext = name.partition('.')
    ok = 1 <= len(base) <= 8 and len(ext) <= 3 and all(c.isalnum() or c == '_' for c in base + ext)
    if not ok or (is_dir and ext):
        sys.exit(f"mkiso: '{name}' is not a valid 8.3 name")


def build_tree(files):
    root = Node('')
    for path in sorted(files):
        parts = path.split('/')
        node = root
        for p in parts[:-1]:
            check_83(p, True)
            if p not in node.children:
                node.children[p] = Node(p, node)
            node = node.children[p]
        check_83(parts[-1], False)
        node.children[parts[-1]] = Node(parts[-1], node, files[path])
    return root


def collect(esp_dir):
    files = {}
    for dirpath, _, names in os.walk(esp_dir):
        for n in names:
            full = os.path.join(dirpath, n)
            rel = os.path.relpath(full, esp_dir).replace(os.sep, '/').upper()
            with open(full, 'rb') as f:
                files[rel] = f.read()
    if not files:
        sys.exit(f"mkiso: no files found in {esp_dir}")
    return files


# -------------------------------------------------------------------- FAT16
def build_fat16(root, size_mb, label):
    total = size_mb * 1024 * 1024 // FAT_SECTOR
    spc, reserved, nfats, root_entries = 4, 4, 2, 512
    root_secs = root_entries * 32 // FAT_SECTOR
    fat_secs = 1
    while True:
        clusters = (total - reserved - nfats * fat_secs - root_secs) // spc
        need = ((clusters + 2) * 2 + FAT_SECTOR - 1) // FAT_SECTOR
        if need <= fat_secs:
            break
        fat_secs = need
    if not 4085 <= clusters < 65525:
        sys.exit("mkiso: FAT16 geometry out of range; adjust --fat-mb")

    csize = spc * FAT_SECTOR
    fat = [0] * (clusters + 2)
    fat[0], fat[1] = 0xFFF8, 0xFFFF
    nxt = [2]

    def alloc(nbytes):
        n = max(1, (nbytes + csize - 1) // csize)
        first = nxt[0]
        if first + n > clusters + 2:
            sys.exit("mkiso: FAT image is full; increase --fat-mb")
        for k in range(first, first + n - 1):
            fat[k] = k + 1
        fat[first + n - 1] = 0xFFFF
        nxt[0] += n
        return first

    def assign(d):
        for name in sorted(d.children):
            c = d.children[name]
            if c.is_dir:
                c.cluster = alloc((len(c.children) + 2) * 32)
                assign(c)
            else:
                c.cluster = alloc(len(c.data)) if c.data else 0

    if len(root.children) + 1 > root_entries:
        sys.exit("mkiso: too many files in the FAT root directory")
    root.cluster = 0
    assign(root)

    t = time.localtime()
    fdate = ((t.tm_year - 1980) << 9) | (t.tm_mon << 5) | t.tm_mday
    ftime = (t.tm_hour << 11) | (t.tm_min << 5) | (t.tm_sec // 2)

    def raw_entry(name11, attr, cluster, size):
        return struct.pack('<11sBBBHHHHHHHI', name11, attr, 0, 0, ftime, fdate, fdate, 0,
                           ftime, fdate, cluster, size)

    def entry(node):
        base, _, ext = node.name.partition('.')
        name11 = (base.ljust(8) + ext.ljust(3)).encode('ascii')
        return raw_entry(name11, 0x10 if node.is_dir else 0x20, node.cluster,
                         0 if node.is_dir else len(node.data))

    img = bytearray(total * FAT_SECTOR)
    root_off = (reserved + nfats * fat_secs) * FAT_SECTOR
    data_start = reserved + nfats * fat_secs + root_secs

    def cluster_off(c):
        return (data_start + (c - 2) * spc) * FAT_SECTOR

    def write_dir(d):
        ents = []
        if d is root:
            ents.append(raw_entry(label.upper().ljust(11)[:11].encode('ascii'), 0x08, 0, 0))
        else:
            ents.append(raw_entry(b'.          ', 0x10, d.cluster, 0))
            ents.append(raw_entry(b'..         ', 0x10, d.parent.cluster if d.parent is not root else 0, 0))
        for name in sorted(d.children):
            ents.append(entry(d.children[name]))
        blob = b''.join(ents)
        off = root_off if d is root else cluster_off(d.cluster)
        img[off:off + len(blob)] = blob
        for name in sorted(d.children):
            c = d.children[name]
            if c.is_dir:
                write_dir(c)
            elif c.data:
                o = cluster_off(c.cluster)
                img[o:o + len(c.data)] = c.data

    write_dir(root)

    fat_bytes = struct.pack(f'<{len(fat)}H', *fat)
    for i in range(nfats):
        off = (reserved + i * fat_secs) * FAT_SECTOR
        img[off:off + len(fat_bytes)] = fat_bytes

    bs = bytearray(FAT_SECTOR)
    bs[0:3] = b'\xEB\x3C\x90'
    bs[3:11] = b'MSWIN4.1'
    struct.pack_into('<HBHBHHBHHHII', bs, 11, FAT_SECTOR, spc, reserved, nfats, root_entries,
                     total if total < 65536 else 0, 0xF8, fat_secs, 32, 64, 0,
                     total if total >= 65536 else 0)
    struct.pack_into('<BBBI11s8s', bs, 36, 0x80, 0, 0x29, int(time.time()) & 0xFFFFFFFF,
                     label.upper().ljust(11)[:11].encode('ascii'), b'FAT16   ')
    bs[510:512] = b'\x55\xAA'
    img[0:FAT_SECTOR] = bs
    return bytes(img)


# ------------------------------------------------------------------ ISO9660
def both16(v):
    return struct.pack('<H', v) + struct.pack('>H', v)


def both32(v):
    return struct.pack('<I', v) + struct.pack('>I', v)


def iso_name(node):
    return b'\x00' if node.parent is None else (node.name if node.is_dir else node.name + ';1').encode('ascii')


def dir_record(ident, lba, size, is_dir, d7):
    rl = 33 + len(ident)
    rl += rl & 1
    rec = (struct.pack('<BB', rl, 0) + both32(lba) + both32(size) + d7 +
           bytes([2 if is_dir else 0, 0, 0]) + both16(1) + bytes([len(ident)]) + ident)
    return rec.ljust(rl, b'\x00')


def pack_records(recs):
    out = bytearray()
    for r in recs:
        if len(out) % ISO_SECTOR + len(r) > ISO_SECTOR:
            out += b'\x00' * (ISO_SECTOR - len(out) % ISO_SECTOR)
        out += r
    if len(out) % ISO_SECTOR:
        out += b'\x00' * (ISO_SECTOR - len(out) % ISO_SECTOR)
    return bytes(out)


def walk_dirs(root):
    out, i = [root], 0
    while i < len(out):
        d = out[i]
        i += 1
        for name in sorted(d.children):
            if d.children[name].is_dir:
                out.append(d.children[name])
    return out


def all_files(root):
    out = []
    for d in walk_dirs(root):
        for name in sorted(d.children):
            if not d.children[name].is_dir:
                out.append(d.children[name])
    return out


def build_iso(root, efi_node, label):
    t = time.gmtime()
    d7 = bytes([t.tm_year - 1900, t.tm_mon, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec, 0])
    d17 = time.strftime('%Y%m%d%H%M%S00', t).encode('ascii') + b'\x00'
    dirs = walk_dirs(root)

    def records(d):
        parent = d.parent or d
        recs = [dir_record(b'\x00', d.lba, d.size, True, d7),
                dir_record(b'\x01', parent.lba, parent.size, True, d7)]
        for c in sorted(d.children.values(), key=iso_name):
            recs.append(dir_record(iso_name(c), c.lba, c.size if c.is_dir else len(c.data), c.is_dir, d7))
        return recs

    # pass 1: sizes (independent of LBAs)
    for d in dirs:
        d.size = len(pack_records(records(d)))

    def path_table(big):
        e = '>' if big else '<'
        out = bytearray()
        for d in dirs:
            ident = b'\x00' if d.parent is None else d.name.encode('ascii')
            parent_no = 1 if d.parent is None else dirs.index(d.parent) + 1
            out += struct.pack(e + 'BBIH', len(ident), 0, d.lba, parent_no) + ident
            if len(ident) & 1:
                out += b'\x00'
        return bytes(out)

    pt_size = len(path_table(False))
    pt_secs = (pt_size + ISO_SECTOR - 1) // ISO_SECTOR
    catalog_lba = 19
    l_pt_lba = 20
    m_pt_lba = l_pt_lba + pt_secs
    lba = m_pt_lba + pt_secs
    for d in dirs:
        d.lba = lba
        lba += d.size // ISO_SECTOR
    for f in all_files(root):
        f.lba = lba if f.data else 0
        lba += (len(f.data) + ISO_SECTOR - 1) // ISO_SECTOR
    total = lba

    img = bytearray(total * ISO_SECTOR)

    def put(sector, blob):
        img[sector * ISO_SECTOR:sector * ISO_SECTOR + len(blob)] = blob

    # Primary volume descriptor
    pvd = bytearray(ISO_SECTOR)
    pvd[0] = 1
    pvd[1:6] = b'CD001'
    pvd[6] = 1
    pvd[8:40] = b' ' * 32
    pvd[40:72] = label.upper().encode('ascii').ljust(32)[:32]
    pvd[80:88] = both32(total)
    pvd[120:124] = both16(1)
    pvd[124:128] = both16(1)
    pvd[128:132] = both16(ISO_SECTOR)
    pvd[132:140] = both32(pt_size)
    struct.pack_into('<I', pvd, 140, l_pt_lba)
    struct.pack_into('>I', pvd, 148, m_pt_lba)
    pvd[156:190] = dir_record(b'\x00', root.lba, root.size, True, d7)
    pvd[190:813] = b' ' * (813 - 190)
    pvd[574:574 + 12] = b'NTOS MKISO  '
    pvd[813:830] = d17
    pvd[830:847] = d17
    pvd[847:864] = b'0' * 16 + b'\x00'
    pvd[864:881] = d17
    pvd[881] = 1
    put(16, pvd)

    # El Torito boot record
    br = bytearray(ISO_SECTOR)
    br[1:6] = b'CD001'
    br[6] = 1
    br[7:39] = b'EL TORITO SPECIFICATION'.ljust(32, b'\x00')
    struct.pack_into('<I', br, 0x47, catalog_lba)
    put(17, br)

    # Terminator
    term = bytearray(ISO_SECTOR)
    term[0] = 255
    term[1:6] = b'CD001'
    term[6] = 1
    put(18, term)

    # Boot catalog: validation entry (platform EFI) + default entry
    efi_sectors_512 = len(efi_node.data) // 512
    val = bytearray(32)
    val[0] = 1
    val[1] = 0xEF
    val[4:28] = b'NTOS'.ljust(24, b'\x00')
    val[30], val[31] = 0x55, 0xAA
    csum = (-sum(struct.unpack('<16H', bytes(val)))) & 0xFFFF
    struct.pack_into('<H', val, 28, csum)
    ent = bytearray(32)
    ent[0] = 0x88                                  # bootable
    ent[1] = 0                                     # no emulation
    struct.pack_into('<H', ent, 6, min(efi_sectors_512, 0xFFFF))
    struct.pack_into('<I', ent, 8, efi_node.lba)
    put(catalog_lba, bytes(val) + bytes(ent))

    put(l_pt_lba, path_table(False))
    put(m_pt_lba, path_table(True))
    for d in dirs:
        put(d.lba, pack_records(records(d)))
    for f in all_files(root):
        if f.data:
            put(f.lba, f.data)

    # Hybrid MBR: one EFI System Partition over EFIBOOT.IMG (512-byte LBAs)
    mbr = bytearray(512)
    struct.pack_into('<B3sB3sII', mbr, 446, 0x00, b'\xFE\xFF\xFF', 0xEF, b'\xFE\xFF\xFF',
                     efi_node.lba * 4, efi_sectors_512)
    mbr[510:512] = b'\x55\xAA'
    img[0:512] = mbr
    return bytes(img)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--esp', required=True, help='directory with EFI/BOOT/BOOTX64.EFI and friends')
    ap.add_argument('--out', required=True, help='output .iso path')
    ap.add_argument('--label', default='NTOS')
    ap.add_argument('--fat-mb', type=int, default=0, help='EFI image size in MiB (default: auto)')
    args = ap.parse_args()

    files = collect(args.esp)
    payload = sum(len(v) for v in files.values())
    fat_mb = args.fat_mb or max(16, (payload * 5 // 4) // (1024 * 1024) + 4)
    efi_img = build_fat16(build_tree(files), fat_mb, args.label)

    iso_files = dict(files)
    iso_files['BOOT/EFIBOOT.IMG'] = efi_img
    root = build_tree(iso_files)
    iso = build_iso(root, root.children['BOOT'].children['EFIBOOT.IMG'], args.label)

    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, 'wb') as f:
        f.write(iso)
    print(f"mkiso: wrote {args.out} ({len(iso) // 1024} KiB, EFI image {fat_mb} MiB, {len(files)} files)")


if __name__ == '__main__':
    main()
