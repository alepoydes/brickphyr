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
    shared_imu = layout["IMU_SHARED_FIELDS"]
    if any(left[0] is not None or right[0] is not None or left[2] != right[2]
           for left, right in zip(kinds[0][2][:shared_imu], kinds[1][2][:shared_imu], strict=True)):
        raise ValueError("Shared IMU metadata must have identical mandatory wire fields")
    lines = [
        "/* Generated from brickphyrdev/capture_layout.py. */",
        "#pragma once",
        f"enum {{ SOURCE_COUNT = {len(sources)}, CONFIG_SIZE = {layout['CONFIG_SIZE']}, "
        f"CAPTURE_MAX_FIELDS = {4 + sum(len(kinds[k][2]) for k in source_kinds)}, "
        f"CAPTURE_IMU_SHARED_FIELDS = {shared_imu} }};",
        "#ifdef CAPTURE_FIELDS_DESCRIPTORS",
        "static const char *const sources[SOURCE_COUNT] = {",
        *(f"\t{json.dumps(name)}," for name in sources),
        "};",
        "static const uint8_t source_kinds[] = {" + ", ".join(map(str, source_kinds)) + "};",
        "struct capture_field { const char *name; uint8_t size; };",
        "struct capture_wire_field { uint8_t size; int8_t selected, delta; };",
        "struct capture_fields { const struct capture_field *items; "
        "const struct capture_wire_field *wire; uint8_t count, wire_count, base; };",
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
        lines.append(f"static const struct capture_wire_field {stem}_wire[] = {{")
        bit = 0
        for selector, decoded, fmt, _expr in fields:
            delta = (fmt == "q" and decoded.endswith(("_us", "_ms")) or
                     fmt == "I" and decoded in layout["DELTA_COUNTERS"])
            lines.append(f"\t{{{struct.calcsize('<' + fmt)}, "
                         f"{bit if selector is not None else -1}, {int(delta)}}},")
            bit += selector is not None
        lines.append("};")
        descriptors.append(f"\t{{{stem}_fields, {stem}_wire, {len(optional)}, {len(fields)}, {base}}},")
    lines.extend(("static const struct capture_fields field_kinds[] = {",
                  *descriptors, "};", "#endif"))
    for kind, c_type, fields in kinds:
        stem = kind.replace(".", "_")
        signature = (f"uint8_t *capture_pack_{stem}(uint8_t *out, uint8_t selected, "
                     f"const {c_type} *value)")
        lines.append(signature + ";")
        lines.append("#ifdef CAPTURE_FIELDS_IMPLEMENTATION")
        lines.append(signature)
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
        lines.extend(("\treturn out;", "}", "#endif"))
    return "\n".join(lines) + "\n"


if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit("usage: capture_layout.py INPUT_MODULE OUTPUT_HEADER")
    Path(sys.argv[2]).write_text(generate(sys.argv[1]))
