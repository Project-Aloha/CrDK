#!/usr/bin/env python3
"""Validate and export the catalogued CRDK cores without replacing a Git tree.

By default this only validates the source catalogue. --destination compares an
export; --write updates it. --adopt-existing is required for the first export
over a legacy SDK and backs up every overwritten file. Subsequent updates reject
local changes rather than silently overwrite them. Unmanaged files stay intact.
"""
import argparse
import datetime
import hashlib
import json
import pathlib
import re
import shutil
import xml.etree.ElementTree as ET

ROOT = pathlib.Path(__file__).resolve().parents[1]
NS = {"m": "http://schemas.microsoft.com/developer/msbuild/2003"}
INCLUDES = re.compile(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]', re.M)
MACROS = re.compile(r'\$\(([^)]+)\)')


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def child(root, relative):
    path = pathlib.PurePosixPath(relative)
    if path.is_absolute() or ".." in path.parts or ".git" in path.parts:
        raise ValueError(f"Unsafe catalogue path: {relative}")
    result = root.joinpath(*path.parts)
    if not result.resolve().is_relative_to(root.resolve()):
        raise ValueError(f"Path escapes root: {relative}")
    return result


def sheet_sources(root, relative, item_type="ClCompile"):
    path = child(root, relative)
    tree = ET.parse(path)
    variables = {"MSBuildThisFileDirectory": str(path.parent) + "/"}
    def expand(value):
        # Only source-path macros are evaluated; this is not an MSBuild runner.
        for _ in range(16):
            updated = MACROS.sub(lambda m: variables.get(m[1], m[0]), value)
            if updated == value:
                break
            value = updated
        return value.replace("\\", "/")
    for group in tree.findall("m:PropertyGroup", NS):
        for item in group:
            variables[item.tag.rsplit("}", 1)[-1]] = expand(item.text or "")
    result = []
    for item in tree.findall(f".//m:{item_type}[@Include]", NS):
        value = expand(item.attrib["Include"])
        if "$" in value:
            raise ValueError(f"Unresolved source macro in {relative}: {value}")
        source = (path.parent / value).resolve()
        result.append(str(source.relative_to(root.resolve())))
    return result


def inventory(root=ROOT):
    catalog = json.loads((root / "CrDK.json").read_text())
    if catalog["schema"] != 1:
        raise ValueError("Unsupported CRDK catalogue schema")
    libraries = catalog["libraries"]
    visited, active = set(), set()
    def visit(name):
        if name in active:
            raise ValueError(f"Library dependency cycle at {name}")
        if name in visited:
            return
        active.add(name)
        for dependency in libraries[name]["dependencies"]:
            if dependency not in libraries:
                raise ValueError(f"Unknown dependency of {name}: {dependency}")
            visit(dependency)
        active.remove(name)
        visited.add(name)
    for name in libraries:
        visit(name)
    files = set(catalog["export_files"] + catalog["header_roots"])
    sources = []
    for name, library in libraries.items():
        listed = library["sources"]
        compiled = sheet_sources(root, library["property_sheet"])
        if sorted(compiled) != sorted(listed) or len(set(compiled)) != len(compiled):
            raise ValueError(f"{name}: property-sheet sources disagree with catalogue")
        files.update(listed)
        # Two checked assembler dialects implement the same ARM64 ABI. Export
        # both, but compile only the declared dialect for each build tool.
        assembly = library.get("assembly_sources", {})
        if assembly:
            if set(assembly) != {"clang", "msvc"}:
                raise ValueError(f"{name}: assembly_sources needs clang and msvc paths")
            if child(root, assembly["clang"]).suffix != ".S" or child(root, assembly["msvc"]).suffix != ".asm":
                raise ValueError(f"{name}: unexpected assembler input suffix")
            custom = sheet_sources(root, library["property_sheet"], "CustomBuild")
            if custom != [assembly["msvc"]]:
                raise ValueError(f"{name}: CustomBuild input disagrees with assembler catalogue")
            files.update(assembly.values())
            sources.append(assembly["clang"])
        elif sheet_sources(root, library["property_sheet"], "CustomBuild"):
            raise ValueError(f"{name}: unqualified CustomBuild input")
        files.add(library["property_sheet"])
        files.update(str(p.relative_to(root))
                     for p in (root / "Library" / name).glob("*.h"))
        sources.extend(listed)
    if len(sources) != len(set(sources)):
        raise ValueError("Duplicate shared compile unit")
    for relative in catalog["header_directories"]:
        files.update(str(p.relative_to(root)) for p in child(root, relative).rglob("*.h"))
    # Include closure catches target-data and interface dependencies omitted from
    # the explicit public roots, without copying all native protocol headers.
    pending = list(files)
    while pending:
        relative = pending.pop()
        path = child(root, relative)
        if not path.is_file():
            raise ValueError(f"Missing export input: {relative}")
        if path.suffix not in (".c", ".h"):
            continue
        for include in INCLUDES.findall(path.read_text()):
            candidates = (root / "Include" / include, path.parent / include)
            for candidate in candidates:
                candidate = candidate.resolve()
                if candidate.is_file():
                    if not candidate.is_relative_to(root.resolve()):
                        raise ValueError(f"Include escapes source package: {relative}: {include}")
                    nested = str(candidate.relative_to(root.resolve()))
                    if nested not in files:
                        files.add(nested)
                        pending.append(nested)
                    break
    return catalog, {f: digest(child(root, f)) for f in sorted(files)}


def export(destination, files, write=False, adopt=False):
    destination = destination.resolve()
    if destination == ROOT or destination.is_relative_to(ROOT):
        raise ValueError("Export destination must be outside the source package")
    manifest = destination / "crdk-export.json"
    old = json.loads(manifest.read_text())["files"] if manifest.exists() else {}
    changes, conflicts = [], []
    for relative, checksum in files.items():
        target = child(destination, relative)
        if target.exists() and (not target.is_file() or target.is_symlink()):
            raise ValueError(f"Non-file destination: {target}")
        actual = digest(target) if target.exists() else None
        if actual == checksum:
            continue
        changes.append(relative)
        if actual is not None and actual != old.get(relative) and not (adopt and not old):
            conflicts.append(relative)
    if conflicts and write:
        raise ValueError("Locally modified/unmanaged export files: " + ", ".join(conflicts))
    if write:
        timestamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ")
        backup = destination.parent / "Build/crdk-export-backups" / timestamp
        # Preflight every path/conflict before mutating any destination file.
        for relative in changes:
            source, target = child(ROOT, relative), child(destination, relative)
            if target.exists():
                saved = child(backup, relative)
                saved.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(target, saved)
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, target)
        destination.mkdir(parents=True, exist_ok=True)
        payload = {"schema": 1, "scope": "Managed portable cores and WDF OSKAL backend only",
                   "files": files}
        temporary = manifest.with_suffix(".json.tmp")
        temporary.write_text(json.dumps(payload, indent=2) + "\n")
        temporary.replace(manifest)
        print(f"Exported {len(files)} files; updated {len(changes)}; backup {backup}")
        return True
    for relative in changes:
        print(f"Outdated/missing: {relative}")
    return not changes and not conflicts


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--destination", type=pathlib.Path)
    parser.add_argument("--write", action="store_true")
    parser.add_argument("--adopt-existing", action="store_true")
    args = parser.parse_args()
    if args.write and not args.destination:
        parser.error("--write needs --destination")
    try:
        catalog, files = inventory()
        print(f"Validated {len(catalog['libraries'])} libraries, {len(files)} export inputs")
        if args.destination and not export(args.destination, files, args.write, args.adopt_existing):
            return 1
    except (ValueError, KeyError, OSError, ET.ParseError) as error:
        parser.exit(2, str(error) + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
