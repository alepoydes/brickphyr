# SPDX-License-Identifier: Apache-2.0
"""Generate the firmware's compact capture layout from the host definition."""

import json
import runpy
import struct
import sys
from pathlib import Path


def generate(source):
    layout = runpy.run_path(str(source))
    kinds = layout["KINDS"]
    sources = layout["SOURCES"]
    source_kinds = layout["SOURCE_KINDS"]
    lines = [
        "/* Generated from brickphyrdev/capture_layout.py. */",
        "#pragma once",
        f"enum {{ SOURCE_COUNT = {len(sources)}, CONFIG_SIZE = {layout['CONFIG_SIZE']} }};",
        "static const char *const sources[SOURCE_COUNT] = {",
        *(f"\t{json.dumps(name)}," for name in sources),
        "};",
        "static const uint8_t source_kinds[] = {" + ", ".join(map(str, source_kinds)) + "};",
        "struct capture_field { const char *name; uint8_t size; };",
        "struct capture_fields { const struct capture_field *items; uint8_t count, base; };",
    ]
    descriptors = []
    for kind, c_type, fields in kinds:
        stem = kind.replace(".", "_")
        optional = [(name, fmt) for name, _decoded, fmt, _expr in fields if name is not None]
        base = sum(struct.calcsize("<" + fmt) for name, _decoded, fmt, _expr in fields
                   if name is None)
        lines.extend((
            f"static const struct capture_field {stem}_fields[] = {{",
            *(f"\t{{{json.dumps(name)}, {struct.calcsize('<' + fmt)}}},"
              for name, fmt in optional),
            "};",
        ))
        descriptors.append(f"\t{{{stem}_fields, {len(optional)}, {base}}},")
    lines.extend(("static const struct capture_fields field_kinds[] = {",
                  *descriptors, "};"))
    for kind, c_type, fields in kinds:
        stem = kind.replace(".", "_")
        lines.append(f"static uint8_t *capture_pack_{stem}(uint8_t *out, uint8_t selected, "
                     f"const {c_type} *value)")
        lines.append("{")
        bit = 0
        for name, _decoded, fmt, expr in fields:
            if fmt == "B":
                write = f"*out++ = {expr};"
            else:
                width = struct.calcsize("<" + fmt)
                write = f"sys_put_le{width * 8}((uint{width * 8}_t)({expr}), out); out += {width};"
            if name is None:
                lines.append(f"\t{write}")
            else:
                lines.append(f"\tif (selected & BIT({bit})) {{ {write} }}")
                bit += 1
        lines.extend(("\treturn out;", "}"))
    return "\n".join(lines) + "\n"


if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit("usage: capture_layout.py INPUT_MODULE OUTPUT_HEADER")
    Path(sys.argv[2]).write_text(generate(sys.argv[1]))
