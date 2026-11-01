#!/usr/bin/env python3
"""Generate portable C++ uint32 SPIR-V arrays. No runtime shader files needed."""
import argparse
from pathlib import Path
import struct


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("files", type=Path, nargs="+")
    args = parser.parse_args()
    out = ["// Generated from validated SPIR-V; do not edit.", "#pragma once",
           "#include <cstdint>", "#include <cstddef>", "#include <string>",
           "namespace lv { namespace embedded {"]
    names = []
    for file in args.files:
        name = file.stem
        if not name.isidentifier() or name in names:
            raise ValueError(f"Invalid/duplicate shader name: {name}")
        data = file.read_bytes()
        if len(data) < 20 or len(data) % 4:
            raise ValueError(f"Invalid SPIR-V length: {file}")
        words = struct.unpack("<" + "I" * (len(data) // 4), data)
        if words[0] != 0x07230203:
            raise ValueError(f"Invalid SPIR-V magic: {file}")
        names.append(name)
        out.append(f"inline constexpr uint32_t {name}_data[] = {{")
        for i in range(0, len(words), 8):
            out.append("  " + ",".join(f"0x{v:08x}u" for v in words[i:i + 8]) + ",")
        out.append("};")
    out.append("inline const uint32_t* shader(const std::string& name, size_t& words) {")
    for name in names:
        out.append(f'  if (name == "{name}") {{ words = sizeof({name}_data)/4; return {name}_data; }}')
    out.extend(["  words = 0; return nullptr;", "}", "}}"])
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text("\n".join(out) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
