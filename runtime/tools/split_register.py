#!/usr/bin/env python3
"""Splits the generated function registration into small functions so it compiles quickly.

`rexglue codegen` writes generated/default/<project>_register.cpp as one function holding one
`registrar->SetFunction(...)` call per recompiled function (about 57,000 for this game). Clang's
compile time for a single function grows faster than linearly with its size: 2,000 calls take
about 1.6 s, 8,000 about 7 s, and the whole file close to ten minutes. Codegen rewrites the file
every time a function hint is added, so that cost would hit every iteration.

This script rewrites the file in place as chunks of CHUNK_SIZE calls, each in its own function,
and keeps the original entry point, which calls the chunks in their original order. The calls
themselves are copied unchanged, so the registered functions and their order are identical.

It is idempotent: a file that is already split is left alone. Exit status is 0 on success and 1
when the file does not have the expected shape (codegen output changed), so the build stops
instead of compiling something unexpected.

Usage: split_register.py <path to *_register.cpp>
"""

import re
import sys
from pathlib import Path

# Calls per chunk; at this size one chunk compiles in about a second and a half.
CHUNK_SIZE = 2000

# Marker written into split files, to recognise them on later runs.
SPLIT_MARKER = "// Split into chunks by runtime/tools/split_register.py."

# The single generated entry point: "void <name>(rex::runtime::IModuleRegistrar* registrar) {".
ENTRY_RE = re.compile(r"^void (\w+)\(rex::runtime::IModuleRegistrar\* registrar\) \{$")
CALL_RE = re.compile(r"^\s*registrar->SetFunction\(0x[0-9A-Fa-f]+, \w+\);$")


def split(text: str) -> str:
    """Returns the split source, or raises ValueError if `text` is not the expected shape."""
    lines = text.splitlines()
    entry_index = next((i for i, line in enumerate(lines) if ENTRY_RE.match(line)), None)
    if entry_index is None:
        raise ValueError("registration entry point not found")
    entry_name = ENTRY_RE.match(lines[entry_index]).group(1)

    try:
        close_index = lines.index("}", entry_index)
    except ValueError as error:
        raise ValueError("end of the registration function not found") from error

    body = [line for line in lines[entry_index + 1 : close_index] if line.strip()]
    if not body or not all(CALL_RE.match(line) for line in body):
        raise ValueError("registration body holds something other than SetFunction calls")

    chunks = [body[i : i + CHUNK_SIZE] for i in range(0, len(body), CHUNK_SIZE)]
    out = lines[:entry_index] + [SPLIT_MARKER, ""]
    for index, chunk in enumerate(chunks):
        out.append(f"static void {entry_name}_{index}(rex::runtime::IModuleRegistrar* registrar) {{")
        out.extend(chunk)
        out.extend(["}", ""])
    out.append(lines[entry_index])
    out.extend(f"  {entry_name}_{index}(registrar);" for index in range(len(chunks)))
    out.append("}")
    out.extend(lines[close_index + 1 :])
    return "\n".join(out) + "\n"


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__.strip().splitlines()[-1], file=sys.stderr)
        return 1
    path = Path(sys.argv[1])
    text = path.read_text(encoding="utf-8")
    if SPLIT_MARKER in text:
        return 0
    try:
        result = split(text)
    except ValueError as error:
        print(f"split_register.py: {path.name}: {error}", file=sys.stderr)
        return 1
    path.write_text(result, encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
