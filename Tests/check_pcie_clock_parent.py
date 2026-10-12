#!/usr/bin/env python3
"""Check the SM8450 PCIe PHY rate-change clock against Linux GCC data.

The 100 MHz ``phy_rchng`` rate is GPLL0_OUT_MAIN / 6.  A source enum and its
RCG CFG encoding are separate fields; confusing them selects BI_TCXO instead
of GPLL0 while still producing a syntactically valid clock vote.
"""
from pathlib import Path
import re

crane = Path(__file__).resolve().parents[1]
target = (crane / "Library/Sm8450ClockLib/Sm8450Clock.c").read_text()
linux = (crane.parents[1] / "linux/drivers/clk/qcom/gcc-sm8450.c").read_text()

table_match = re.search(
    r"STATIC\s+ClockRcgFreqTable\s+mPcieRchngFreq\[\]\s*=\s*\{(.*?)\n\};",
    target,
    re.S,
)
assert table_match, "SM8450 PCIe rate-change table is missing"
entries = re.findall(
    r"CLOCK_NODE_RCG_FREQ_TABLE_ELEMENT\(\s*([^,]+),\s*([^,]+),\s*"
    r"(\d+),\s*(\d+),\s*0,\s*0\s*\)",
    table_match.group(1),
)
assert entries == [
    ("SM8450_CLOCK_RCG2_SRC_BI_TCXO", "0", "19200000", "1"),
    ("SM8450_CLOCK_RCG2_SRC_GPLL0_MAIN", "1", "100000000", "6"),
], f"unexpected PCIe rate-change table: {entries!r}"

parent_map = re.search(
    r"static const struct parent_map gcc_parent_map_0\[\].*?=\s*\{(.*?)\n\};",
    linux,
    re.S,
)
assert parent_map and re.search(
    r"\{\s*P_GCC_GPLL0_OUT_MAIN\s*,\s*1\s*\}", parent_map.group(1)
), "Linux GCC parent-map does not encode GPLL0_OUT_MAIN as 1"
assert re.search(
    r"F\(100000000,\s*P_GCC_GPLL0_OUT_MAIN,\s*6,\s*0,\s*0\)", linux
), "Linux PCIe phy_rchng table no longer uses GPLL0_OUT_MAIN / 6"

for node, register in (
    ("GccPcie0PhyRchngClkSrc", "0x7b048"),
    ("GccPcie1PhyRchngClkSrc", "0x9d04c"),
):
    node_match = re.search(
        rf"STATIC ClockNode {node}\s*=\s*\{{(.*?)\n\}};", target, re.S
    )
    assert node_match, f"{node} is missing"
    body = node_match.group(1)
    assert f".CmdRegister      = {register}" in body, f"{node}: wrong RCG register"
    assert ".FreqTable        = mPcieRchngFreq" in body, f"{node}: wrong rate table"

print("SM8450 PCIe phy_rchng: GPLL0_OUT_MAIN (parent 1) / 6 at 100 MHz: passed")
