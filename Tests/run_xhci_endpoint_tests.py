#!/usr/bin/env python3
"""Exercise production xHCI endpoint/cancel/event functions with real ABI types."""
from pathlib import Path
import argparse
import hashlib
import json
import re
import subprocess
import tempfile

crane = Path(__file__).resolve().parents[1]
root = crane.parents[1]
xhci = root / "Platforms/CranePkg/Override/MU_BASECORE/MdeModulePkg/Bus/Pci/XhciDxe"
parser = argparse.ArgumentParser()
parser.add_argument("--source", type=Path, default=xhci / "XhciSched.c")
parser.add_argument("--driver-source", type=Path, default=xhci / "Xhci.c")
parser.add_argument("--output", type=Path)
parser.add_argument("--expect-no-soft-retry", action="store_true")
parser.add_argument("--expect-halted-stop-failure", action="store_true")
args = parser.parse_args()
source = args.source.read_text()
names = ["XhcReadEndpointState", "XhcStopEndpoint", "XhcResetEndpoint",
         "XhcSetTrDequeuePointer", "XhcDequeueTrbFromEndpoint",
         "XhciDelAsyncIntTransfer", "XhciInsertAsyncIntTransfer",
         "XhcMonitorAsyncRequests", "IsTransferRingTrb", "IsAsyncIntTrb",
         "XhcCheckUrbResult", "XhcBusDevAddrToSlotId", "XhcEndpointToDci"]
names.append("XhcExecTransfer")
if not args.expect_no_soft_retry:
    names.insert(1, "XhcRetryTransaction")
names += ["XhcRetireSlotRequests", "XhcFreeDeviceSlot", "XhcFreeDeviceSlots", "XhcDisableSlotCmd", "XhcDisableSlotCmd64",
          "XhcRouteStringToSlotId", "XhcPollPortStatusChange",
          "XhcAsyncInterruptTransfer", "XhciDelAllAsyncIntTransfers", "XhcDriverBindingStop"]
source += "\n" + args.driver_source.read_text()
functions = []
if 'XhcCleanupFailedStart (' in source:
    names = ['XhcFindFailedStart', 'XhcCleanupFailedStart'] + names
    functions.append('STATIC USB_XHCI_INSTANCE *mXhcFailedStarts;')
for name in names:
    match = re.search(r"^(?:STATIC\n)?(?:EFI_STATUS|UINT8|BOOLEAN|VOID|URB \*|USB_XHCI_INSTANCE \*)\n(?:EFIAPI\n)?" + name + r" \([^;{]*\)\n\{", source, re.M)
    if name in ("XhcReadEndpointState", "XhcRetireSlotRequests", "XhcFreeDeviceSlot", "XhcFreeDeviceSlots") and not match:
        continue  # Prior-source regression has no private helper.
    assert match, name
    body = match.end() - 1
    depth = 1
    end = body + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    functions.append(source[match.start():end])
def run(directory):
    directory.mkdir(parents=True, exist_ok=True)
    (directory / "XhciEndpointProduction.c").write_text("\n\n".join(functions))
    includes = [directory, xhci, root / "MU_BASECORE/MdePkg/Include",
                root / "MU_BASECORE/MdePkg/Include/X64",
                root / "Platforms/CranePkg/Include", root / "MU_BASECORE/MdeModulePkg/Include"]
    command = ["clang", "-std=c11", "-fshort-wchar", "-g", "-O1", "-Wall", "-Wextra", "-Werror",
               "-Wno-unused-parameter", "-DMDEPKG_NDEBUG", "-fsanitize=address,undefined",
               "-fno-sanitize-recover=all", *["-I" + str(p) for p in includes],
               str(crane / "Tests/XhciEndpointTest.c"), "-o", str(directory / "test")]
    if args.expect_no_soft_retry:
        command.insert(1, "-DTEST_BEFORE_SOFT_RETRY")
    subprocess.run(command, check=True)
    result = subprocess.run([str(directory / "test")], text=True, capture_output=True)
    print(result.stdout, end="")
    print(result.stderr, end="")
    (directory / "run.log").write_text(result.stdout + result.stderr)
    if args.expect_halted_stop_failure:
        assert result.returncode != 0, "Prior halted-controller stop must fail the regression"
        assert "TestControllerStop" in result.stderr and "Commands==0 && RingFrees==0" in result.stderr
    else:
        result.check_returncode()
    inputs = [args.source, args.driver_source, xhci / "XhciSched.h", xhci / "Xhci.h",
              crane / "Tests/XhciEndpointTest.c", Path(__file__)]
    receipt = {"passed": True, "sanitizers": ["address", "undefined"],
               "before_soft_retry": args.expect_no_soft_retry,
               "observed_expected_halted_stop_failure": args.expect_halted_stop_failure,
               "returncode": result.returncode, "command": command,
               "inputs": {str(p.resolve()): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs},
               "binary_sha256": hashlib.sha256((directory / "test").read_bytes()).hexdigest()}
    (directory / "artifacts.json").write_text(json.dumps(receipt, indent=2) + "\n")

if args.output:
    run(args.output.resolve())
else:
    with tempfile.TemporaryDirectory(prefix="crane-xhci-endpoint-") as tmp:
        run(Path(tmp))
