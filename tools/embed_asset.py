#!/usr/bin/env python3
"""Turns a file into a C++ source defining its bytes: `embed_asset.py <input> <output.cc> <symbol>`.

The output defines `extern const char piAsset_<symbol>[]` and `extern const unsigned long piAsset_<symbol>_size`; the matching
declarations are written by hand next to the BUILD file that uses it (see assets/export_html).
"""
import sys


def main() -> int:
    source, target, symbol = sys.argv[1:4]
    with open(source, "rb") as handle:
        data = handle.read()
    lines = []
    for start in range(0, len(data), 32):
        lines.append(",".join(str(byte - 256 if byte > 127 else byte) for byte in data[start : start + 32]))
    body = ",\n".join(lines)
    with open(target, "w", encoding="ascii") as out:
        out.write(f"extern const char piAsset_{symbol}[];\nextern const unsigned long piAsset_{symbol}_size;\n")
        out.write(f"const char piAsset_{symbol}[] = {{\n{body}\n}};\n")
        out.write(f"const unsigned long piAsset_{symbol}_size = {len(data)};\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
