#!/usr/bin/env python3
"""Generate ctypes structure declarations from the public C header.

The generator uses Clang's JSON AST so array extents, typedef expansion, field
order, native alignment, and packed compact structures all come from the C
declarations rather than a second hand-maintained schema.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import subprocess


PRIMITIVES = {
    "char": "C.c_char",
    "double": "C.c_double",
    "float": "C.c_float",
    "int8_t": "C.c_int8",
    "int16_t": "C.c_int16",
    "int32_t": "C.c_int32",
    "int64_t": "C.c_int64",
    "uint8_t": "C.c_uint8",
    "uint16_t": "C.c_uint16",
    "uint32_t": "C.c_uint32",
    "uint64_t": "C.c_uint64",
}


def python_name(c_name: str) -> str:
    return c_name.removeprefix("Balatro")


def type_expression(qualified: str) -> str:
    dimensions = [int(value) for value in re.findall(r"\[(\d+)\]", qualified)]
    base = re.sub(r"\[\d+\]", "", qualified)
    expression = PRIMITIVES.get(base, python_name(base))
    for dimension in reversed(dimensions):
        expression = f"({expression} * {dimension})"
    return expression


def render(header: Path, clang: str) -> str:
    command = [
        clang,
        "-Xclang",
        "-ast-dump=json",
        "-fsyntax-only",
        f"-I{header.parent}",
        str(header),
    ]
    ast = json.loads(subprocess.check_output(command, text=True))
    records = [
        node
        for node in ast.get("inner", [])
        if node.get("kind") == "RecordDecl"
        and node.get("completeDefinition")
        and node.get("name", "").startswith("Balatro")
    ]

    lines = [
        '"""Generated from include/balatro.h by tools/generate_ctypes.py."""',
        "",
        "from __future__ import annotations",
        "",
        "import ctypes as C",
        "",
    ]
    for record in records:
        name = python_name(record["name"])
        packed = any(child.get("kind") == "MaxFieldAlignmentAttr" for child in record.get("inner", []))
        lines.append(f"class {name}(C.Structure):")
        if packed:
            lines.append("    _pack_ = 1")
        fields = [child for child in record.get("inner", []) if child.get("kind") == "FieldDecl"]
        lines.append("    _fields_ = [")
        for field in fields:
            lines.append(f'        ("{field["name"]}", {type_expression(field["type"]["qualType"])}),')
        lines.extend(["    ]", ""])

    lines.extend([
        "LegalMasks = ObservedLegality",
        "",
        "__all__ = [name for name, value in globals().items() if isinstance(value, type) and issubclass(value, C.Structure)]",
        "__all__.append(\"LegalMasks\")",
        "",
    ])
    return "\n".join(lines)


def main() -> None:
    parser = argparse.ArgumentParser()
    root = Path(__file__).resolve().parents[1]
    parser.add_argument("--header", type=Path, default=root / "include" / "balatro.h")
    parser.add_argument("--output", type=Path, default=root / "python" / "simulatro" / "_structs.py")
    parser.add_argument("--clang", default="clang")
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    rendered = render(args.header.resolve(), args.clang)
    output = args.output.resolve()
    if args.check:
        if not output.exists() or output.read_text() != rendered:
            raise SystemExit(f"generated ctypes are stale: run {Path(__file__).name}")
    else:
        output.write_text(rendered)


if __name__ == "__main__":
    main()
