#!/usr/bin/env python3
"""Extract flanterm's built-in 8x16 font into a C header."""

from __future__ import annotations

import re
import sys
from pathlib import Path


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: extract_flanterm_font.py <flanterm fb.c> <output header>", file=sys.stderr)
        return 2

    source = Path(sys.argv[1])
    output = Path(sys.argv[2])
    text = source.read_text(encoding="utf-8")

    match = re.search(r"static const uint8_t builtin_font\[\]\s*=\s*\{(.*?)\};", text, re.S)
    if not match:
        print(f"{source} does not contain flanterm's builtin_font array", file=sys.stderr)
        return 1

    values = match.group(1).strip()
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(
        "/* Generated from flanterm/src/flanterm_backends/fb.c. */\n"
        "/* SPDX-License-Identifier: BSD-2-Clause */\n"
        "#pragma once\n\n"
        "#include <stdint.h>\n\n"
        "static const uint8_t flanterm_builtin_font[256u * 16u] = {\n"
        f"{values}\n"
        "};\n",
        encoding="utf-8",
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
