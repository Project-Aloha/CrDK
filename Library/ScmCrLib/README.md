# SCM ownership core

Canonical portable relocation of the held Windows ScmCr core. Uses OSKAL
status/mutex/atomic operations; Windows supplies the existing ARM64 transport,
while ScmUefi.c is a native ArmSmcLib adapter. No platform currently selects
this library. The fixed PAS1 HDK8450 authority and GPU CP aperture policy are
still the first target profile, not general Qualcomm support.

ScmPas.c privately wraps unchanged ../PILCrLib/PilSmccc.c with its original MIT
notices and exact frame checks. Private names avoid duplicate symbols in SDK
checks that link all libraries. Only the resident provider may publish a secure
route. The independent original PAS helper must have no remaining live caller
in that image after the coherent Common/PEP migration.

Source history, tests and next Windows integration are under CrWDDK/ScmCr.
The Windows header/source baseline remains archived in Build/scm72-held;
remove its now-superseded copy only after managed SDK registration/export and
shared apply are completed together. No official MU source is modified.
