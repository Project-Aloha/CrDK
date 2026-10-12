#!/usr/bin/env python3
"""Check native target DMA policy and hardware capability against pinned SM8450 DT.

This tests metadata and compilation, not SMMU hardware, ACPI PCI enumeration,
ExitBootServices handoff, or the coherency behavior of a physical endpoint.
"""
import argparse
import hashlib
import json
import re
import shlex
import shutil
import subprocess
from pathlib import Path


CRANE = Path(__file__).resolve().parents[1]
WORKSPACE = CRANE.parents[1]
DT_COMMIT = "877203a7cf9b79973d44a9db716490f95d820110"
LINUX_COMMIT = "a53261b60834ce617cc4f3c7723dda8285c59789"
ANDROID_KERNEL_COMMIT = "e93f69dc83e69e5139e6a7911041f357b5e2b304"


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(command, log):
    result = subprocess.run(command, capture_output=True, text=True, timeout=60)
    log.write_text(result.stdout + result.stderr)
    if result.returncode:
        raise RuntimeError(f"{command[0]} failed: {log}\n{result.stderr}")
    return result.stdout


def node(text, label):
    match = re.search(r"\b" + re.escape(label) + r"\s*:\s*[^{};]+\{", text)
    if not match:
        raise ValueError(f"missing source node {label}")
    start = match.end()
    depth = 1
    for index in range(start, len(text)):
        depth += (text[index] == "{") - (text[index] == "}")
        if depth == 0:
            return text[start:index]
    raise ValueError(f"unterminated source node {label}")


def property_text(text, name):
    match = re.search(r"\b" + re.escape(name) + r"\s*=\s*([^;]+);", text)
    if not match:
        raise ValueError(f"missing source property {name}")
    return match.group(1)


def pinned_blob(repository, commit, path, destination):
    data = subprocess.run(
        ["git", "-C", str(repository), "show", f"{commit}:{path}"],
        capture_output=True, check=True,
    ).stdout
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_bytes(data)
    return data.decode(), {
        "repository": str(repository), "commit": commit, "path": path,
        "snapshot": str(destination), "sha256": sha(destination),
    }


def source_qualification(output, dt_repository, linux_repository, kernel_repository):
    snapshots = output / "sources"
    dt_prefix = "kernel_platform/qcom/proprietary/devicetree/qcom/"
    blobs = {}
    provenance = []
    selections = [
        ("android-pcie", dt_repository, DT_COMMIT, dt_prefix + "waipio-pcie.dtsi"),
        ("android-soc", dt_repository, DT_COMMIT, dt_prefix + "waipio.dtsi"),
        ("android-smmu", dt_repository, DT_COMMIT,
         dt_prefix + "msm-arm-smmu-waipio.dtsi"),
        ("linux-soc", linux_repository, LINUX_COMMIT,
         "arch/arm64/boot/dts/qcom/sm8450.dtsi"),
        ("linux-pcie", linux_repository, LINUX_COMMIT,
         "drivers/pci/controller/dwc/pcie-qcom.c"),
        ("android-pgtable", kernel_repository, ANDROID_KERNEL_COMMIT,
         "drivers/iommu/io-pgtable-arm.c"),
        ("android-smmu-driver", kernel_repository, ANDROID_KERNEL_COMMIT,
         "drivers/iommu/arm/arm-smmu/arm-smmu.c"),
    ]
    for name, repository, commit, path in selections:
        blobs[name], source = pinned_blob(
            repository, commit, path, snapshots / (name + Path(path).suffix))
        provenance.append(source)

    expected = ((0x1c00, 0x1c01), (0x1c80, 0x1c81))
    for index, ids in enumerate(expected):
        android = node(blobs["android-pcie"], f"pcie{index}")
        if not re.search(r"\bdma-coherent\s*;", android):
            raise ValueError("Android target does not qualify coherent PCIe")
        assigned = [int(value, 16) for value in re.findall(
            r"&apps_smmu\s+(0x[0-9a-fA-F]+)",
            property_text(android, "iommu-map"))]
        if assigned != list(ids):
            raise ValueError("Android PCIe exact SID pair changed")
        mainline = node(blobs["linux-soc"], f"pcie{index}")
        if re.search(r"\bdma-coherent\s*;", mainline):
            raise ValueError("review mainline coherent policy change")
        assigned = [int(value, 16) for value in re.findall(
            r"&apps_smmu\s+(0x[0-9a-fA-F]+)",
            property_text(mainline, "iommu-map"))]
        if assigned != list(ids):
            raise ValueError("mainline PCIe exact SID pair changed")
    ufs = node(blobs["android-soc"], "ufshc_mem")
    if not re.search(r"\bdma-coherent\s*;", ufs):
        raise ValueError("Android UFS is not coherent")
    if not re.fullmatch(r"\s*<&apps_smmu\s+0x[Ee]0\s+0x0>\s*",
                        property_text(ufs, "iommus")):
        raise ValueError("Android UFS exact SID changed")
    android_its = node(blobs["android-soc"], "gic_its")
    mainline_its = node(blobs["linux-soc"], "gic_its")
    if "0x17140000 0x20000" not in property_text(android_its, "reg"):
        raise ValueError("Android ITS geometry changed")
    if "0x17140000 0x0 0x40000" not in property_text(mainline_its, "reg"):
        raise ValueError("mainline ITS geometry changed")
    result = {
        "status": "PASS", "sources": provenance,
        "hardware_coherent_sids": ["1c00", "1c01", "1c80", "1c81", "e0"],
        "native_coherent_sids": ["1c00", "1c01", "1c80", "1c81", "e0"],
        "native_ufs_policy": "E0 uses coherent WB translation with CPU WB "
                             "common buffers from UEFI initialization; EBS "
                             "revokes mappings then retains coherent physical DMA.",
        "basis": "Android client dma-coherent plus exact iommu-map/iommus; "
                 "SMMU table-walker dma-coherent is a separate property.",
        "msi": {
            "android_pcie0": property_text(node(blobs["android-pcie"], "pcie0"),
                                            "msi-map"),
            "android_pcie1": property_text(node(blobs["android-pcie"], "pcie1"),
                                            "msi-parent"),
            "mainline_pcie0": property_text(node(blobs["linux-soc"], "pcie0"),
                                             "msi-map"),
            "mainline_pcie1": property_text(node(blobs["linux-soc"], "pcie1"),
                                             "msi-map"),
        },
        "its": {"base": "17140000", "android_bytes": "20000",
                "mainline_bytes": "40000", "live_hypervisor_access": "unqualified"},
        "limits": "Mainline omits PCIe dma-coherent. No SM8450 no-snoop "
                  "override write, PCIe ACPI root, EBS unblock, or ITS/MSI "
                  "current-world readiness is established by this test.",
    }
    path = output / "source-qualification.json"
    path.write_text(json.dumps(result, indent=2) + "\n")
    return path, provenance


def dependencies(path):
    text = path.read_text().replace("\\\n", " ")
    return [Path(item).resolve() for item in shlex.split(text.split(":", 1)[1])]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--cc", default="clang")
    parser.add_argument("--wdk-tool", type=Path, default=Path(
        "/home/sunf/droidvm-repos/out/artifacts/dxvk/"
        "llvm-mingw-20260908-ucrt-ubuntu-22.04-x86_64/bin/clang"))
    parser.add_argument("--windows", type=Path, default=WORKSPACE.parent / "CrWDDK")
    parser.add_argument("--android-dt", type=Path,
                        default=WORKSPACE / ".vscode/android_kernel_modules_and_devicetree_oneplus_sm8450")
    parser.add_argument("--linux", type=Path, default=WORKSPACE / ".vscode/linux")
    parser.add_argument("--android-kernel", type=Path,
                        default=WORKSPACE.parent / "droidvm-repos/qcrosvm/reference/codes/waipio-msm-5.10")
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    qualification, sources = source_qualification(
        output, args.android_dt, args.linux, args.android_kernel)

    targets = [CRANE / f"Target/Sm{soc}/CrSmmuTarget.c" for soc in (8450, 8250, 8350)]
    inputs = [*targets, Path(__file__).resolve(), CRANE / "Tests/SmmuTargetCoherentTest.c",
              CRANE / "Include/Library/CrTargetSmmuLib.h", CRANE / "Include/Library/smmu.h",
              CRANE / "Include/oskal/common.h", CRANE / "Tests/KernelMocks/ntddk.h"]
    frozen = {str(path): sha(path) for path in inputs}
    include = "-I" + str(CRANE / "Include")
    common = ["-std=c17", "-Wall", "-Wextra", "-Werror", include]
    edk = WORKSPACE / "MU_BASECORE/MdePkg/Include"
    runs = []
    objects = []
    compiler_inputs = set(inputs)
    for soc, target in zip((8450, 8250, 8350), targets):
        for kernel in (False, True):
            name = f"sm{soc}-" + ("nt" if kernel else "native")
            binary = output / name
            flags = [args.cc, *common, "-g", "-O1", "-fsanitize=address,undefined",
                     "-fno-sanitize-recover=all", f"-DTEST_SOC={soc}"]
            if kernel:
                flags += ["-D_KERNEL_MODE", "-pthread", "-I" + str(CRANE / "Tests/KernelMocks"),
                          "-include", str(CRANE / "Include/oskal/common.h")]
            else:
                flags += ["-fshort-wchar", "-I" + str(edk), "-I" + str(edk / "X64")]
            for source in (target, CRANE / "Tests/SmmuTargetCoherentTest.c"):
                dep = output / (name + "-" + source.stem + ".d")
                run([*flags, "-M", "-MT", name, str(source), "-MF", str(dep)],
                    output / (dep.stem + "-deps.log"))
                compiler_inputs.update(dependencies(dep))
            command = [*flags, str(target), str(CRANE / "Tests/SmmuTargetCoherentTest.c"),
                       "-o", str(binary)]
            run(command, output / (name + "-compile.log"))
            message = run([str(binary)], output / (name + ".log"))
            print(message, end="")
            runs.append({"name": name, "command": command, "binary_sha256": sha(binary)})

    kit = "10.0.26100.0"
    wdk = args.windows / "Build/serial-kit/microsoft.windows.wdk.arm64/c"
    sdk = args.windows / "Build/serial-kit/microsoft.windows.sdk.cpp/c"
    directories = [wdk / f"Include/{kit}/km", wdk / f"Include/{kit}/km/crt",
                   wdk / f"Include/{kit}/shared", sdk / f"Include/{kit}/shared"]
    if not all(path.is_dir() for path in directories):
        raise RuntimeError("actual ARM64 WDK/SDK include tree is required")
    overlay = output / "case.json"
    overlay.write_text(json.dumps({"version": 0, "case-sensitive": False, "roots": [
        {"type": "directory", "name": str(directory), "contents": [
            {"type": "file", "name": file.name, "external-contents": str(file)}
            for file in sorted(directory.iterdir()) if file.is_file()]}
        for directory in directories]}))
    for soc, target in zip((8450, 8250, 8350), targets):
        for kernel in (False, True):
            name = f"sm{soc}-arm64-" + ("wdk" if kernel else "efi")
            obj = output / (name + ".obj")
            dep = output / (name + ".d")
            flags = [str(args.wdk_tool), "--target=aarch64-pc-windows-msvc",
                     "-ffreestanding", "-fms-extensions", "-Wno-microsoft-anon-tag", *common]
            if kernel:
                flags += ["-D_ARM64_", "-D_KERNEL_MODE", "-D_WIN32_WINNT=0x0A00",
                          "-DNTDDI_VERSION=0x0A00000A", "-ivfsoverlay", str(overlay),
                          "-I" + str(args.windows / "PepCr"),
                          "-include", str(CRANE / "Include/oskal/common.h")]
                for directory in directories:
                    flags += ["-isystem", str(directory)]
            else:
                flags += ["-fshort-wchar", "-I" + str(edk), "-I" + str(edk / "AArch64")]
            command = [*flags, "-MD", "-MF", str(dep), "-c", str(target), "-o", str(obj)]
            run(command, output / (name + ".log"))
            compiler_inputs.update(dependencies(dep))
            objects.append({"name": name, "command": command, "sha256": sha(obj)})
    if frozen != {str(path): sha(path) for path in inputs}:
        raise RuntimeError("input drift during target test")
    tool = Path(shutil.which(args.cc)).resolve()
    artifact = {
        "status": "PASS", "scope": __doc__, "inputs": frozen,
        "compiler_inputs": {str(path): sha(path) for path in sorted(compiler_inputs)},
        "source_qualification": {"path": str(qualification), "sha256": sha(qualification),
                                 "sources": sources},
        "tools": {str(tool): sha(tool), str(args.wdk_tool): sha(args.wdk_tool)},
        "runs": runs, "arm64_objects": objects,
        "limits": "Six actual-target ASan/UBSan programs and six actual ARM64 "
                  "EFI/WDK target objects; no SMMU adapter/core execution, full "
                  "firmware packaging, OS PCIe enumeration, or physical coherency claim.",
    }
    (output / "artifacts.json").write_text(json.dumps(artifact, indent=2) + "\n")
    print("Actual three-target coherent metadata and six ARM64 objects PASS")


if __name__ == "__main__":
    main()
