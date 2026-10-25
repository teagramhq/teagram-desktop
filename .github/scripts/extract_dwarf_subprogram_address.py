#!/usr/bin/env python3
import re
import sys
from pathlib import Path


DIE_HEADER = re.compile(
    r"^0x[0-9a-fA-F]+:\s+(DW_TAG_[A-Za-z0-9_]+)\b",
    re.MULTILINE,
)
DIE_NAME = re.compile(r'DW_AT_name\s+\("([^"]+)"\)')
LOW_PC = re.compile(r"DW_AT_low_pc\s+\((0x[0-9a-fA-F]+)\)")


def find_subprogram_address(name, source_file, dwarf_info):
    headers = list(DIE_HEADER.finditer(dwarf_info))
    current_source = ""
    for index, header in enumerate(headers):
        end = (
            headers[index + 1].start()
            if index + 1 < len(headers)
            else len(dwarf_info)
        )
        block = dwarf_info[header.start():end]
        tag = header.group(1)
        if tag == "DW_TAG_compile_unit":
            current_source = "\n".join(DIE_NAME.findall(block))
        elif tag == "DW_TAG_subprogram" and source_file in current_source:
            if any(name in die_name for die_name in DIE_NAME.findall(block)):
                match = LOW_PC.search(block)
                if match:
                    return match.group(1)
    raise ValueError(
        f"no DW_TAG_subprogram named {name} in {source_file} has DW_AT_low_pc"
    )


def main():
    if len(sys.argv) != 4:
        print(
            "usage: extract_dwarf_subprogram_address.py "
            "<name> <source-file> <dwarf-info>",
            file=sys.stderr,
        )
        return 2
    name, source_file, info_path = sys.argv[1:]
    try:
        dwarf_info = Path(info_path).read_text(encoding="utf-8")
        print(find_subprogram_address(name, source_file, dwarf_info))
    except (OSError, ValueError) as error:
        print(error, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
