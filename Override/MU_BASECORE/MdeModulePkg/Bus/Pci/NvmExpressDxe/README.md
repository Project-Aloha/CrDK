# NVMe platform fork

This is the complete source closure of MU's NvmExpressDxe at the revision in
`PROVENANCE.json`, with the previously developed Crane recovery and DMA ownership
corrections. Its sources are byte-for-byte copies of the pre-migration working
version. Microsoft MU submodules remain unmodified.

The changes bound recovery recursion, negotiate queue counts, retain DMA backing
until a confirmed stop/unmap and serialize lifecycle/callback reentry. Existing
read-only timeout diagnostics are preserved. `MU_BASECORE.patch` records the
upstream delta, and the host recovery/lifecycle suites exercise these paths.

Select this module instead of the upstream module; they share a FILE_GUID.
Host and DEBUG build checks do not establish endpoint or board operation.
