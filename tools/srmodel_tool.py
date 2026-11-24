#!/usr/bin/env python3
"""Extract model directories from an srmodels.bin pack, or build a new pack.

Format (from esp-sr/model/pack_model.py):
    u32 model_num
    per model:  char[32] name, u32 file_count
                per file: char[32] name, u32 start (absolute), u32 len
    then the raw data blobs.

Usage:
    python srmodel_tool.py extract <pack.bin> <out_dir>
    python srmodel_tool.py pack    <models_dir> <out.bin>
    python srmodel_tool.py list    <pack.bin>
"""
import os
import struct
import sys

NAME_LEN = 32


def _read_name(buf, off):
    raw = buf[off:off + NAME_LEN]
    return raw.split(b"\0", 1)[0].decode("utf-8", "replace"), off + NAME_LEN


def parse(path):
    with open(path, "rb") as f:
        data = f.read()
    (model_num,) = struct.unpack_from("<I", data, 0)
    off = 4
    models = []
    for _ in range(model_num):
        name, off = _read_name(data, off)
        (file_count,) = struct.unpack_from("<I", data, off)
        off += 4
        files = []
        for _ in range(file_count):
            fname, off = _read_name(data, off)
            start, length = struct.unpack_from("<II", data, off)
            off += 8
            files.append((fname, start, length))
        models.append((name, files))
    return data, models


def cmd_list(path):
    data, models = parse(path)
    print("pack: %d bytes, %d models" % (len(data), len(models)))
    for name, files in models:
        total = sum(f[2] for f in files)
        print("  %-28s %2d files, %8d bytes" % (name, len(files), total))
        for fname, start, length in files:
            print("      %-24s %8d @ 0x%06X" % (fname, length, start))


def cmd_extract(path, out_dir):
    data, models = parse(path)
    os.makedirs(out_dir, exist_ok=True)
    for name, files in models:
        d = os.path.join(out_dir, name)
        os.makedirs(d, exist_ok=True)
        for fname, start, length in files:
            with open(os.path.join(d, fname), "wb") as f:
                f.write(data[start:start + length])
        print("extracted %s (%d files)" % (name, len(files)))


def _pad_name(s):
    raw = s.encode("utf-8")
    assert len(raw) <= NAME_LEN, s
    return raw + b"\0" * (NAME_LEN - len(raw))


def cmd_pack(models_dir, out_path):
    models = []
    for entry in sorted(os.listdir(models_dir)):
        d = os.path.join(models_dir, entry)
        if not os.path.isdir(d):
            continue
        files = []
        for fname in sorted(os.listdir(d)):
            p = os.path.join(d, fname)
            if os.path.isfile(p):
                with open(p, "rb") as f:
                    files.append((fname, f.read()))
        if files:
            models.append((entry, files))

    file_num = sum(len(f) for _, f in models)
    header_len = 4 + len(models) * (NAME_LEN + 4) + file_num * (NAME_LEN + 8)
    print("models: %d, files: %d, header: %d bytes" % (len(models), file_num, header_len))

    out = bytearray()
    out += struct.pack("<I", len(models))
    blob = bytearray()
    for name, files in models:
        out += _pad_name(name)
        out += struct.pack("<I", len(files))
        for fname, content in files:
            out += _pad_name(fname)
            out += struct.pack("<II", header_len + len(blob), len(content))
            blob += content
    out += blob

    with open(out_path, "wb") as f:
        f.write(out)
    print("wrote %s: %d bytes" % (out_path, len(out)))
    print("partition cap 0x3E0000 = %d bytes, spare %d" % (0x3E0000, 0x3E0000 - len(out)))


if __name__ == "__main__":
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(1)
    cmd = sys.argv[1]
    if cmd == "list":
        cmd_list(sys.argv[2])
    elif cmd == "extract":
        cmd_extract(sys.argv[2], sys.argv[3])
    elif cmd == "pack":
        cmd_pack(sys.argv[2], sys.argv[3])
    else:
        print(__doc__)
        sys.exit(1)
