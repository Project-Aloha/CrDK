#!/usr/bin/env python3
"""Verify export dependency/source closure and preservation on update conflicts."""
import importlib.util
import copy
import json
from pathlib import Path
import tempfile

root = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("crdk_export", root / "Tools/export_crdk.py")
exporter = importlib.util.module_from_spec(spec)
spec.loader.exec_module(exporter)
catalog, complete = exporter.inventory()
assert len(catalog["libraries"]) >= 16
for name, entry in catalog["libraries"].items():
    assert all(path in complete for path in entry["sources"])
    assert all(path not in complete for path in entry["native_only_sources"])
    assert all(path in complete for path in entry.get("assembly_sources", {}).values())
assert "Include/Library/CrTargetPcieLib.h" in complete
assert "Include/ucsi/TypeC.h" in complete
assert not any(path.startswith("Driver/") for path in complete)
with tempfile.TemporaryDirectory(prefix="crane-export-") as name:
    base = Path(name)
    target = base / "CrDK"
    target.mkdir()
    (target / ".git").write_text("gitdir: ../.git/modules/CrDK\n")
    original = b"legacy property sheet\n"
    (target / "CrDK.props").write_bytes(original)
    files = {"CrDK.props": complete["CrDK.props"]}
    try:
        exporter.export(target, files, write=True)
        raise AssertionError("An unmanaged destination was overwritten")
    except ValueError:
        pass
    assert (target / "CrDK.props").read_bytes() == original
    exporter.export(target, files, write=True, adopt=True)
    backups = list((base / "Build/crdk-export-backups").glob("*/CrDK.props"))
    assert len(backups) == 1 and backups[0].read_bytes() == original
    assert (target / ".git").read_text() == "gitdir: ../.git/modules/CrDK\n"
    assert exporter.export(target, files)
    prior_manifest = (target / "crdk-export.json").read_bytes()
    (target / "CrDK.props").write_text("user edited SDK\n")
    files["CrDK.json"] = complete["CrDK.json"]
    try:
        exporter.export(target, files, write=True)
        raise AssertionError("A local edit was overwritten")
    except ValueError:
        pass
    assert (target / "CrDK.props").read_text() == "user edited SDK\n"
    assert not (target / "CrDK.json").exists()  # Full preflight before any writes.
    assert (target / "crdk-export.json").read_bytes() == prior_manifest
    (target / "Include").symlink_to(base / "outside")
    for path in ("../outside", ".git/config", "Include/escaped.h"):
        try:
            exporter.child(target, path)
            raise AssertionError(f"Unsafe export path accepted: {path}")
        except ValueError:
            pass
# A missing/unqualified assembler must fail before an SDK can pass catalogue
# checks with an unresolved secure-call symbol. CustomBuild commands are data.
with tempfile.TemporaryDirectory(prefix="crane-asm-catalog-") as name:
    base = Path(name)
    lib = base / "Library/Probe"
    lib.mkdir(parents=True)
    for source in ("Probe.c", "Probe.S", "Probe.asm"):
        (lib/source).write_text("/* source fixture */\n")
    sheet = lib / "Probe.props"
    sheet.write_text('''<Project xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
<ItemGroup><ClCompile Include="Probe.c"/><CustomBuild Include="Probe.asm">
<Command>unqualified_command_must_never_execute</Command></CustomBuild></ItemGroup></Project>''')
    entry = {"property_sheet":"Library/Probe/Probe.props", "sources":["Library/Probe/Probe.c"],
             "dependencies":[], "native_only_sources":[],
             "assembly_sources":{"clang":"Library/Probe/Probe.S", "msvc":"Library/Probe/Probe.asm"}}
    valid = {"schema":1,"libraries":{"Probe":entry},"export_files":[],
             "header_roots":[],"header_directories":[]}
    def check(payload):
        (base/"CrDK.json").write_text(json.dumps(payload))
        return exporter.inventory(base)
    _, files = check(valid)
    assert set(entry["assembly_sources"].values()).issubset(files)
    # Split portable libraries can share an internal header through a relative
    # include. Export its canonical in-package path, never an escaping target.
    sibling = base / "Library/Sibling"
    sibling.mkdir()
    (sibling/"Internal.h").write_text("/* shared internal declaration */\n")
    (lib/"Probe.c").write_text('#include "../Sibling/Internal.h"\n')
    _, files = check(valid)
    assert "Library/Sibling/Internal.h" in files
    assert not any(".." in Path(path).parts for path in files)
    with tempfile.TemporaryDirectory(prefix="crane-outside-header-") as external:
        outside = Path(external)/"Foreign.h"
        outside.write_text("/* outside the package */\n")
        (lib/"Escaped.h").symlink_to(outside)
        (lib/"Probe.c").write_text('#include "Escaped.h"\n')
        try:
            check(valid)
            raise AssertionError("An external header entered the SDK")
        except ValueError:
            pass
        (lib/"Escaped.h").unlink()
    (lib/"Probe.c").write_text('#include "../Sibling/Internal.h"\n')
    malformed = []
    for changes in ({}, {"clang":"Library/Probe/Probe.S"},
                    {"clang":"Library/Probe/Probe.S","msvc":"Library/Probe/Absent.asm"},
                    {"clang":"../escape.S","msvc":"Library/Probe/Probe.asm"}):
        payload=copy.deepcopy(valid);payload["libraries"]["Probe"]["assembly_sources"]=changes
        malformed.append(payload)
    duplicate=copy.deepcopy(valid);duplicate["libraries"]["Duplicate"]=copy.deepcopy(entry)
    malformed.append(duplicate)
    for payload in malformed:
        try:
            check(payload)
            raise AssertionError("Incomplete or unqualified assembler catalogue accepted")
        except ValueError:
            pass
    (lib/"Probe.S").unlink()
    try:
        check(valid)
        raise AssertionError("Missing assembler was accepted")
    except ValueError:
        pass
print("CRDK export: source/assembler closure, backups, local edits, Git and path containment passed")
