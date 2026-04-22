#!/usr/bin/env python3
"""Portable `xxd -i` replacement for embedding binary files as C arrays.

Usage: embed_file.py <input> <output.h>

The C symbol name is derived from the input basename by replacing
non-alphanumeric characters with '_' (same convention as `xxd -i`).
"""
import sys
import os


def main():
    if len(sys.argv) != 3:
        sys.stderr.write("usage: embed_file.py <input> <output.h>\n")
        sys.exit(2)

    in_path, out_path = sys.argv[1], sys.argv[2]
    base = os.path.basename(in_path)
    symbol = "".join(c if c.isalnum() else "_" for c in base)

    with open(in_path, "rb") as f:
        data = f.read()

    lines = ["unsigned char %s[] = {" % symbol]
    for i in range(0, len(data), 12):
        chunk = data[i:i + 12]
        lines.append("  " + ", ".join("0x%02x" % b for b in chunk) + ",")
    if lines[-1].endswith(","):
        lines[-1] = lines[-1][:-1]
    lines.append("};")
    lines.append("unsigned int %s_len = %d;" % (symbol, len(data)))

    with open(out_path, "w", newline="\n") as f:
        f.write("\n".join(lines) + "\n")


if __name__ == "__main__":
    main()
