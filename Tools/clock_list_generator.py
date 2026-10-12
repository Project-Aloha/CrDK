#!/usr/bin/env python3
"""Generate a complete SM8450 GCC clock inventory from Linux sources.

ADPK advertises a clock parser, but its current ``tools/clk-parser.py`` is an
empty placeholder.  This small, dependency-free parser keeps the target's
clock ID/name inventory tied to the Linux DT binding and provider array.  It
does not invent register semantics for clocks that Crane does not own yet;
those entries remain inventory-only and are reported as such.

Usage::

    python3 Platforms/CranePkg/Tools/clock_list_generator.py \
      --linux-root linux \
      --output Platforms/CranePkg/Include/Library/CrSm8450ClockIds.h

The default ``--check`` mode validates the binding IDs, provider references,
and generated inventory without writing a file.
"""

from __future__ import annotations

import argparse
import re
import subprocess
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Tuple


CLOCK_HEADER = Path("include/dt-bindings/clock/qcom,gcc-sm8450.h")
CLOCK_SOURCE = Path("drivers/clk/qcom/gcc-sm8450.c")


@dataclass(frozen=True)
class Clock:
    macro: str
    ident: int
    name: str
    symbol: Optional[str]
    linux_type: Optional[str]


def read_linux_revision(root: Path) -> str:
    try:
        return subprocess.check_output(
            ["git", "-C", str(root), "rev-parse", "HEAD"],
            text=True,
            stderr=subprocess.DEVNULL,
        ).strip()
    except (OSError, subprocess.CalledProcessError):
        return "unknown"


def parse_binding(path: Path) -> Dict[str, int]:
    text = path.read_text(encoding="utf-8")
    text = text.split("/* GCC resets */", 1)[0]
    definitions = {
        macro: int(value)
        for macro, value in re.findall(
            r"^#define\s+(\w+)\s+(\d+)\s*$", text, re.MULTILINE
        )
        if macro.startswith(("GCC_", "PCIE_", "UFS_", "USB3_"))
    }
    if len(definitions) != 184 or set(definitions.values()) != set(range(1, 185)):
        raise ValueError(
            "SM8450 binding must contain exactly IDs 1..184 "
            f"(found {len(definitions)} definitions)"
        )
    return definitions


def extract_array(text: str) -> Dict[str, str]:
    start = text.index("static struct clk_regmap *gcc_sm8450_clocks[]")
    end = text.index("static const struct qcom_reset_map", start)
    body = text[start:end]
    return {
        macro: symbol
        for macro, symbol in re.findall(
            r"^\s*\[(\w+)\]\s*=\s*&([A-Za-z0-9_]+)\.clkr\s*,?",
            body,
            re.MULTILINE,
        )
    }


def extract_object_metadata(text: str) -> Dict[str, Tuple[str, str]]:
    """Return Linux symbol -> (struct type, clk name) for clock objects.

    The initializer can contain nested structures, so locate its matching
    brace before searching for the first ``.name`` field.
    """

    result: Dict[str, Tuple[str, str]] = {}
    init_names = {
        symbol: name
        for symbol, name in re.findall(
            r"static struct clk_init_data\s+(\w+)\s*=\s*\{.*?"
            r"\.name\s*=\s*\"([^\"]+)\"",
            text,
            re.DOTALL,
        )
    }
    declaration = re.compile(
        r"^static struct\s+(clk_[A-Za-z0-9_]+)\s+([A-Za-z0-9_]+)\s*=\s*\{",
        re.MULTILINE,
    )
    for match in declaration.finditer(text):
        depth = 0
        end = None
        for index in range(match.end() - 1, len(text)):
            char = text[index]
            if char == "{":
                depth += 1
            elif char == "}":
                depth -= 1
                if depth == 0:
                    end = index + 1
                    break
        if end is None:
            raise ValueError(f"unterminated initializer for {match.group(2)}")
        initializer = text[match.end() : end]
        name = re.search(r"\.name\s*=\s*\"([^\"]+)\"", initializer)
        if name is None:
            init = re.search(r"\.hw\.init\s*=\s*&([A-Za-z0-9_]+)", initializer)
            if init is not None:
                init_name = init_names.get(init.group(1))
                if init_name is not None:
                    name = (init_name,)
        if name is not None:
            clock_name = name.group(1) if hasattr(name, "group") else name[0]
            result[match.group(2)] = (match.group(1), clock_name)
    return result


def fallback_name(macro: str) -> str:
    return macro.lower()


def parse_clocks(root: Path) -> List[Clock]:
    header = root / CLOCK_HEADER
    source = root / CLOCK_SOURCE
    definitions = parse_binding(header)
    text = source.read_text(encoding="utf-8")
    array = extract_array(text)
    metadata = extract_object_metadata(text)
    clocks = []
    for macro, ident in sorted(definitions.items(), key=lambda item: item[1]):
        symbol = array.get(macro)
        object_data = metadata.get(symbol) if symbol is not None else None
        linux_type = object_data[0] if object_data is not None else None
        name = object_data[1] if object_data is not None else fallback_name(macro)
        clocks.append(Clock(macro, ident, name, symbol, linux_type))
    return clocks


def validate(clocks: Iterable[Clock]) -> None:
    clocks = list(clocks)
    ids = [clock.ident for clock in clocks]
    if ids != list(range(1, 185)):
        raise ValueError("generated clock IDs are not contiguous 1..184")
    if len({clock.name for clock in clocks}) != len(clocks):
        raise ValueError("generated clock names are not unique")
    for clock in clocks:
        if clock.symbol is not None and clock.linux_type is None:
            raise ValueError(
                f"provider references {clock.symbol}, but its initializer was not found"
            )


def render(root: Path, clocks: List[Clock]) -> str:
    revision = read_linux_revision(root)
    lines = [
        "/* Auto-generated by Platforms/CranePkg/Tools/clock_list_generator.py. */",
        "/* Source: Linux drivers/clk/qcom/gcc-sm8450.c and its DT binding. */",
        f"/* Linux revision: {revision} */",
        "#pragma once",
        "#include <oskal/common.h>",
        "",
        "/* Names and IDs copied from qcom,gcc-sm8450.h. */",
        "enum {",
    ]
    for clock in clocks:
        lines.append(f"  {clock.macro} = {clock.ident},")
    lines.extend([
        "};",
        "",
        "/* SM8450 GCC has public clock IDs 1..184; ID zero is unused. */",
        "#define SM8450_CLOCK_GCC_CLOCK_COUNT 185U",
        "#define SM8450_CLOCK_GCC_MAX_ID      184U",
        "",
        "typedef struct {",
        "  UINT16       Id;",
        "  CONST CHAR8 *Name;",
        "  CONST CHAR8 *LinuxSymbol;",
        "  CONST CHAR8 *LinuxType;",
        "} SM8450_CLOCK_DESCRIPTOR;",
        "",
        "/* Complete source inventory. A NULL LinuxSymbol means the Linux",
        " * provider exposes the binding ID without a gcc_sm8450_clocks[]",
        " * object (for example a PHY-owned hardware clock). */",
        "STATIC CONST SM8450_CLOCK_DESCRIPTOR mSm8450GccClockList[SM8450_CLOCK_GCC_CLOCK_COUNT] = {",
        "  [0] = { 0, NULL, NULL, NULL },",
    ])
    for clock in clocks:
        symbol = f'"{clock.symbol}"' if clock.symbol else "NULL"
        linux_type = f'"{clock.linux_type}"' if clock.linux_type else "NULL"
        lines.append(
            f'  [{clock.ident}] = {{ {clock.ident}, "{clock.name}", '
            f"{symbol}, {linux_type} }},"
        )
    lines.extend(["};", ""])
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--linux-root", type=Path, required=True)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()

    clocks = parse_clocks(args.linux_root)
    validate(clocks)
    if args.output is not None and not args.check:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(render(args.linux_root, clocks), encoding="utf-8")
    print(
        f"validated {len(clocks)} SM8450 GCC clocks (IDs "
        f"{clocks[0].ident}..{clocks[-1].ident}), "
        f"provider objects={sum(clock.symbol is not None for clock in clocks)}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
