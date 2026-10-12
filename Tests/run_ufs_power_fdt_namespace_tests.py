#!/usr/bin/env python3
"""Real namespace validator/libfdt, hostile trees and deterministic rescan guard.

No production or official MU files are changed. The test includes the actual
parser translation unit so that its private Namespace can be called directly.
An optional --before-source compiles a saved pre-fix source and requires the
same performance guard to reject it; this is a negative control, not a fixture
containing a second implementation of the parser.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shlex
import shutil
import struct
import subprocess


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def node(name="", children=(), properties=()):
    return name, list(children), list(properties)


def dtb(tree):
    """Emit valid structure tokens, allowing duplicate names unlike dtc.

    Each property name gets its own string offset, including duplicates.
    These fixtures exercise ambiguity checks that libfdt intentionally allows.
    """
    strings, tokens = bytearray(), bytearray()

    def word(value):
        tokens.extend(struct.pack(">I", value))

    def pad():
        tokens.extend(b"\0" * (-len(tokens) % 4))

    def add(item):
        name, children, properties = item
        word(1)
        tokens.extend(name.encode() + b"\0")
        pad()
        for key, value in properties:
            offset = len(strings)
            strings.extend(key.encode() + b"\0")
            word(3)
            word(len(value))
            word(offset)
            tokens.extend(value)
            pad()
        for child in children:
            add(child)
        word(2)

    add(tree)
    word(9)
    header = struct.pack(">10I", 0xd00dfeed, 56 + len(tokens) + len(strings),
                         56, 56 + len(tokens), 40, 17, 16, 0,
                         len(strings), len(tokens))
    return header + b"\0" * 16 + tokens + strings


def fixtures(out):
    cases = []

    def case(name, tree, status="success", performance=False):
        path = out / (name + ".dtb")
        path.write_bytes(dtb(tree))
        cases.append((path, status, "performance" if performance else "semantic"))

    # The 18 topology/ambiguity cases were also independently exercised by the
    # Native74 peer review. Retain them with the deterministic cost regression.
    case("root", node(), performance=True)
    case("cousins", node(children=[node("a", [node("same")]),
                                  node("b", [node("same")])]))
    case("duplicate-after-subtree", node(children=[
        node("same", [node("child", [node("grandchild")])]), node("same")]), "bad")
    case("multi-level-ascent", node(children=[
        node("a", [node("x", [node("y", [node("same")])])]),
        node("b", [node("same"), node("x", [node("same")])])]))
    case("duplicate-after-ascent", node(children=[
        node("a", [node("x"), node("z", [node("q")]), node("x")])]), "bad")
    # Distinct strings with identical folded FNV1a-16 values (41200). Exact
    # identity, not hash equality, must decide duplicates for nodes/properties.
    a, b = "collision31", "collision113"
    case("sibling-hash-collision", node(children=[node(a), node(b)]))
    case("property-hash-collision", node(properties=[(a, b"x"), (b, b"y")]))
    case("duplicate-with-collision-between", node(children=[node(a), node(b), node(a)]), "bad")
    case("duplicate-property-distinct-string-offsets", node(properties=[
        ("same", b"x"), ("same", b"y")]), "bad")
    case("property-name-different-parents", node(children=[
        node("a", properties=[("same", b"1")]),
        node("b", properties=[("same", b"2")])]))
    for levels, status in [(31, "success"), (32, "unsupported")]:
        tree = node("bottom")
        for i in range(levels - 1):
            tree = node(f"level{i}", [tree])
        case(f"depth-{levels}-plus-root", node(children=[tree]), status)
    for count, status in [(8192, "success"), (8193, "unsupported")]:
        children, left = [], count - 65
        for i in range(64):
            leaves = min(127, left)
            left -= leaves
            children.append(node(f"parent{i}", [node(f"leaf{j}") for j in range(leaves)]))
        assert left == 0
        case(f"nodes-{count}", node(children=children), status)
    case("properties-work-limit", node(properties=[(f"p{i}", b"") for i in range(4097)]),
         "unsupported")
    case("phandle-hash-collision", node(children=[
        node("a", properties=[("phandle", struct.pack(">I", 1))]),
        node("b", properties=[("phandle", struct.pack(">I", 4097))])]))
    case("duplicate-phandle", node(children=[
        node("a", properties=[("phandle", struct.pack(">I", 1))]),
        node("b", properties=[("linux,phandle", struct.pack(">I", 1))])]), "bad")
    case("large-chain-cousins", node(children=[
        node(f"parent{i}", [node("same"), node("branch", [node("same")])])
        for i in range(180)]))
    # Thousands of nodes plus nontrivial subtrees expose parent/root rescans;
    # a property-heavy node independently guards repeated property iteration.
    case("large-topology", node(children=[
        node(f"parent{i}", [node(f"leaf{j}", properties=[("value", b"x")])
                           for j in range(32)]) for i in range(96)]), performance=True)
    case("large-properties", node(properties=[(f"p{i}", b"x") for i in range(1024)]),
         performance=True)
    return cases


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--before-source", type=Path)
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    receipt = out / "artifacts.json"
    receipt.unlink(missing_ok=True)  # A failed rerun must never retain PASS.
    crane = Path(__file__).resolve().parents[1]
    root = crane.parents[1]
    base = root / "MU_BASECORE/MdePkg/Library/BaseFdtLib"
    fdt = base / "libfdt/libfdt"
    source = crane / "Library/UfsPowerFdtCrLib/UfsPowerFdt.c"
    held = sha(source)
    cases = fixtures(out)
    sources = [crane / "Tests/UfsPowerFdtNamespaceTest.c", base / "FdtLib.c",
               *sorted(fdt.glob("*.c"))]
    compiler = Path(shutil.which("clang")).resolve()
    flags = ["-std=c17", "-D_GNU_SOURCE", "-include", "Uefi.h", "-g", "-O1",
             "-fshort-wchar", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
             "-Wno-unused-function", "-Wno-sign-compare", "-Wno-incompatible-pointer-types",
             "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
             "-I" + str(crane / "Include"),
             "-I" + str(root / "MU_BASECORE/MdePkg/Include"),
             "-I" + str(root / "MU_BASECORE/MdePkg/Include/X64"),
             "-idirafter", str(base), "-I" + str(fdt)]
    inputs = {Path(__file__).resolve(): sha(Path(__file__).resolve())}
    builds = []

    def dependencies(text):
        return {Path(p).resolve() for p in shlex.split(
            text.replace("\\\n", " ").split(":", 1)[1])}

    def run(command, log):
        result = subprocess.run(command, capture_output=True, text=True)
        log.write_text(result.stdout + result.stderr)
        return result

    def build(name, defines, selected_cases, negative=False):
        directory = out / name
        directory.mkdir(exist_ok=True)
        commands, objects, actual_deps = [], [], set()
        for index, src in enumerate(sources):
            # Snapshot the same source/header closure before compiling, then
            # compare it with the .d emitted by the real compiler invocation.
            closure = dependencies(subprocess.check_output(
                [str(compiler), *flags, *defines, "-MM", str(src)], text=True))
            for path in closure:
                inputs.setdefault(path, sha(path))
            obj, dep = directory / f"{index}.o", directory / f"{index}.d"
            command = [str(compiler), *flags, *defines, "-MMD", "-MF", str(dep),
                       "-c", str(src), "-o", str(obj)]
            result = run(command, directory / f"{index}-compile.log")
            if result.returncode:
                raise RuntimeError(result.stderr)
            actual = dependencies(dep.read_text())
            if actual != closure:
                raise RuntimeError("Compiler header closure changed")
            actual_deps.update(actual)
            commands.append(command)
            objects.append(obj)
        executable = directory / "namespace-test"
        command = [str(compiler), "-fsanitize=address,undefined", *map(str, objects),
                   "-o", str(executable)]
        result = run(command, directory / "link.log")
        if result.returncode:
            raise RuntimeError(result.stderr)
        commands.append(command)
        invocation = [str(executable), *[str(value) for case in selected_cases for value in case]]
        result = run(invocation, directory / "run.log")
        if negative:
            if result.returncode != 1 or "PERF_REGRESSION:" not in result.stderr:
                raise RuntimeError("Saved before-source did not fail the performance guard")
        elif result.returncode:
            raise RuntimeError(result.stdout + result.stderr)
        builds.append({"name": name, "compile_and_link": commands, "run": invocation,
                       "expected_failure": negative, "returncode": result.returncode,
                       "binary_sha256": sha(executable), "log_sha256": sha(directory / "run.log"),
                       "dependency_files": {str(p.relative_to(out)): sha(p)
                                            for p in sorted(directory.glob("*.d"))},
                       "actual_dependencies": sorted(map(str, actual_deps))})
        print(name, "PASS (saved before-source rejected)" if negative else result.stdout.splitlines()[-1])

    for mode in ["debug", "release"]:
        build(mode, ["-DMDEPKG_NDEBUG"] if mode == "release" else [], cases)
    if args.before_source:
        before = args.before_source.resolve()
        performance = [case for case in cases if case[0].stem.startswith("large-") and
                       case[2] == "performance"]
        build("before-negative-control", ['-DNAMESPACE_SOURCE="' + str(before) + '"'],
              performance, negative=True)
    if sha(source) != held or any(sha(path) != digest for path, digest in inputs.items()):
        raise RuntimeError("Input changed during validation")
    receipt.write_text(json.dumps({
        "status": "PASS", "scope": "Actual namespace + real repository libfdt; host ASan/UBSan, no hardware claim",
        "parser_sha256": held, "compiler": str(compiler), "compiler_sha256": sha(compiler),
        "compiler_version": subprocess.check_output([str(compiler), "--version"], text=True),
        "inputs": {str(p.relative_to(root)) if p.is_relative_to(root) else str(p): h
                   for p, h in sorted(inputs.items())},
        "fixtures": {p.name: {"sha256": sha(p), "status": status, "check": check}
                     for p, status, check in cases}, "builds": builds,
    }, indent=2) + "\n")
    print("receipt", sha(receipt))


if __name__ == "__main__":
    main()
