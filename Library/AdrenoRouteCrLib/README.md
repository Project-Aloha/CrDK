# SM8450 GPU boot admission (private, unselected)

This unit supplies the missing early native-to-Windows route boundary. It does
not supply PAS13, an allocated hardware context-bank number, aperture SCM
authority, an operational GPU adapter, or RenderReady.

`CrAdrenoRouteParseIort` accepts one bounded standard revision-5 IORT profile:
the installed GPU0 and GMU0 named components must resolve to the qualified
Adreno SMMUv2 at 0x03da0000, including its complete IRQ arrays. GPU input mappings
resolve to physical stream IDs 0, 1, 0x400, 0x401; GMU mappings resolve to 5,
0x405. The per-PDO InputMappingId namespace is checked independently and is
never interpreted as a physical SID or a context-bank number. The schema is
deliberately narrower than a general IORT validator.

`CrAdrenoBootRetire` inventories every visible stream and context before any GPU
stop or SMMU mutation. It accepts exact mask-0 or mask-0x400 routes and rejects
foreign active routes, overlapping/wildcard routes, incomplete aliases,
enabled unreferenced contexts, non-AArch64/non-stage-1 contexts and unsupported
SMMU capabilities. It never accesses a secure register window. The retained
native provider must expose the same qualified non-secure register instance.

With actual retained clock/access ownership, the transaction invokes the real
AdrenoStop fence, checks a second identical route inventory, faults only the
owned stream slots, completes own-ASID maintenance, disables only the owned
contexts, completes maintenance again, and verifies all original global,
foreign, SMR, CBAR, CBA2R and TTBR-ASID values. It does not change SCR0, global
translation policy, context-bank assignment or any page-table contents. Fault
write emulation quirks are refused rather than worked around. Each attempted
effect is journaled before dispatch; an uncertain result is terminal.

GPUH is a proposed private ACPI transport, not a standard hardware attestation.
Its 64-byte locator identifies one reserved 4-KiB page containing a bounded,
pointer-free, versioned record with CRC, exact IORT facts, native incarnation,
full retired alias mask and actual fresh stop observations. Checksums detect
corruption, not malicious firmware. The native owner commits before publishing
GPUH and then IORT. Failure has no fallback installation of the execution IORT.
This ordering precedes Windows loader/HAL enumeration; EBS alone is too late.

The WDM importer reads the actually installed tables through AuxKlib, rejects
duplicate tables, checks exact PDO BiosDeviceName and _CCA, verifies that the
record page is excluded from OS usable RAM, takes two read-only copies, and
compares the actual HAL QueryInputMappings sets. Imported state and exact PDO
references belong to permanent nonpaged owner storage. There is no unload or
reinitialize operation. One execution incarnation consumes the boot loan;
actual child domains must retire under a fresh stopped parent before the loan
is consumed permanently.

All four CP and two GMU input mappings receive real HAL fault registrations.
Every nonexact HAL effect result, including a negative destructive result,
retains remaining handles, pages and callback storage terminally. This avoids
inventing a retry guarantee for opaque handles that HAL has not provided.
The code still requires the separate secure lease before any CP activation.

Current evidence is host sanitizer composition and actual ARM64 compilation,
with OS, HAL, secure endpoint and hardware modeled. No current ACPI profile,
native image, SDK, DiagnosticStart or driver installation selects this unit.
See the private handoff and provenance manifest for exact source and receipts.
