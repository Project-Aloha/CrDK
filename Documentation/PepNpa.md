# Qualified Kodiak NPA resources

The original Kodiak AML contains 21 `NPARESOURCE` records on each of the three
boards. These records use scalar opcode 1 and two resource names:

| AML name | Levels and record counts | Backend |
|---|---|---|
| `/arc/client/rail_cx` | 0:11, 256:4 | ARC `cx.lvl` virtual performance level |
| `/arc/client/rail_xo` | 0:4, 128:2 | ARC `xo.lvl` clock state level |

The reduced LPMD/LPCC fixture contains 15 CX records. XO also occurs in the
original APMD/APCC audio and EWMD/WBRC Wi-Fi programs; the inventory test reads
every NPA record from the original AML rather than assuming LPCC is complete.
QRD7325 AML SHA256 is
`e24683a3f2cb495fbf855b1ebab9aac3573c68327b8a14ff764603875aea44cb`.
Both Samsung tables have SHA256
`f4e2efda79fec4f47f1c082f550b5dc9774b368a4138049277a1bfc92527f03e`.

Linux's [RPMh power-domain driver](https://github.com/torvalds/linux/blob/master/drivers/pmdomain/qcom/rpmhpd.c)
uses command database auxiliary levels to convert a virtual level into a
hardware corner index. Level 256 is NOM in the
[power bindings](https://github.com/torvalds/linux/blob/master/include/dt-bindings/power/qcom,rpmhpd.h).
It is not a VRM voltage or mode selector. The independent
[clock driver](https://github.com/torvalds/linux/blob/master/drivers/clk/qcom/clk-rpmh.c)
uses raw XO hardware values 0 and 3. That alone does not prove AML128 means3.
The Windows adapter accepts XO128 only when real cached `xo.lvl` metadata maps
it to index3 and declares level128 at that index. Missing or mismatched XO aux
data declines before reservation or hardware voting. XO0 may be qualified
without an auxiliary level table. No other scalar value is admitted by the
Kodiak profiles.

`PepNpaLib` matches the complete name/opcode/full64-bit scalar against immutable
profiles. Describe is pure. A separate metadata callback produces the backend
corner, and capture/restore stores that corner and presence at full width.
Unknown names, tuples, qualifiers and unavailable metadata decline. The
provider never guesses millivolts, boolean enable semantics, or mode rankings.
`Common/CrPepNpa` checks the narrower ARC ABI range before conversion and holds
a referenced provider interface plus fixed stable owner records. Failed reserve
or release retains the record and reference; uninit refuses retained owners.

Reserve is metadata and fixed-storage bookkeeping only. Fresh ARC Get remains
Pending until the broker establishes an explicitly qualified hardware handoff
with Set. Unused reservation cleanup does not clear existing boot votes.
Journals must close before ReleaseOwner; provider removal or leaving D0 is an
unavailable backend and failed close remains retryable with the original owner.
The permanent broker must maintain D0 dependencies, drain callbacks and retain
code/storage through recovery. Active votes do not qualify sleep/wake ARC
programming. Do not enable platform readiness from this adapter alone.

`run_pep_npa_tests.py` decompiles all three original AML inputs and tests actual
decode/provider/journal sources with native and NT sanitizers. The Windows
`run_pep_npa_adapter_tests.py` tests the actual adapter and actual ARC vote bank,
including metadata, initial handoff, foreign votes, write/close/removal failures
and full-width guards. `check_pep_npa_wdk.py` compiles and links their actual
sources against ARM64 WDK26100 with a trace/WPP substitute; its probe is build evidence, not an installed
PEP, package, signature or hardware test. Whole-policy qualification and the
permanent PEP integration remain separate work.
