#!/usr/bin/env python3
"""Decode the actual Kodiak LPCC literal fixture and exercise the portable core.

This checks the shared policy, not Windows PoFx registration or hardware votes.
The restricted ASL reader accepts literal packages only, never executes AML.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
import tempfile

crane = Path(__file__).resolve().parents[1]
workspace = crane.parents[1]
source = crane / "Tests/Fixtures/PepKodiakLpcc.asl"
text = source.read_text()
start = text.index("Name (LPCC")
brace = text.index("{", start)
depth, end = 1, brace + 1
while depth:
    if text[end] == "{": depth += 1
    elif text[end] == "}": depth -= 1
    end += 1
literal = text[start:end]
literal = literal[literal.index("Package"):]
tokens = re.findall(r'"(?:\\.|[^"\\])*"|0x[\da-fA-F]+|\b[A-Za-z_]\w*\b|\d+|[(){}]', literal)
position = 0


def read_value():
    global position
    token = tokens[position]
    position += 1
    if token == "Package":
        assert tokens[position] == "("
        position += 1
        count = None
        if tokens[position] != ")":
            count = int(tokens[position], 0)
            position += 1
        assert tokens[position] == ")" and tokens[position + 1] == "{"
        position += 2
        result = []
        while tokens[position] != "}": result.append(read_value())
        position += 1
        assert count is None or count == len(result)
        return result
    if token.startswith('"'): return json.loads(token)
    if token == "Zero": return 0
    if token == "One": return 1
    return int(token, 0)


tree = read_value()
assert position == len(tokens) and len(tree) == 5
def source_collection(name):
    """Read an additional original literal collection, with no AML execution."""
    global tokens, position
    start = text.index('Name (' + name + ', Package')
    brace = text.index('{', start)
    depth, end = 1, brace + 1
    while depth:
        if text[end] == '{': depth += 1
        elif text[end] == '}': depth -= 1
        end += 1
    literal = text[start:end]
    tokens = re.findall(r'"(?:\\.|[^"\\])*"|0x[\da-fA-F]+|\b[A-Za-z_]\w*\b|\d+|[(){}]',
                        literal[literal.index('Package'):])
    position = 0
    value = read_value()
    assert position == len(tokens)
    return value

apcc = source_collection('APCC')
touch = source_collection('LPXC')
assert len(apcc) == len(touch) == 1
definitions = []


def emit(value):
    if isinstance(value, list):
        items = [emit(v) for v in value]
        name = "PepNodes" + str(len(definitions))
        definitions.append("static const CR_PEP_VALUE " + name + "[] = {\n" +
                           ",\n".join(items) + "\n};")
        return '{.Type = CrPepPackage, .Package = {' + name + ', ' + str(len(items)) + '}}'
    if isinstance(value, str):
        return '{.Type = CrPepString, .String = {' + json.dumps(value) + ', ' + str(len(value)) + '}}'
    return '{.Type = CrPepInteger, .Integer = ' + str(value) + 'ULL}'


root = emit(tree)
apcc_root = emit(apcc)
touch_root = emit(touch)


def wire(value, version, package_type=3):
    if isinstance(value, list):
        kind, data = package_type, b"".join(wire(v, version) for v in value)
    elif isinstance(value, bytes):
        kind, data = 2, value
    elif isinstance(value, str):
        kind, data = 1, value.encode("ascii") + b"\0"
    else:
        kind, data = 0, value.to_bytes(4 if value <= 0xffffffff else 8, "little")
    header = struct.pack("<HH", kind, len(data)) if version == 1 else struct.pack("<H2xI", kind, len(data))
    return header + data + bytes(max(0, 4 - len(data)))


def wire_output(value, version, package_type=3):
    data = wire(value, version, package_type)
    signature = 0x426f6541 if version == 1 else 0x4b6f6541
    return struct.pack("<III", signature, 12 + len(data), 1) + data


def byte_array(name, data):
    return "static const UINT8 " + name + "[] = {" + ",".join(map(str, data)) + "};\n"


deep = 0
for _ in range(20): deep = [deep]
encoded = byte_array("KodiakWireV1", wire_output(tree, 1))
encoded += byte_array("KodiakWireV2", wire_output(tree, 2, 4))
encoded += byte_array("DeepWire", wire_output(deep, 2))
for version in (1,2):
    encoded += byte_array('ApccWireV'+str(version),wire_output(apcc,version))
    encoded += byte_array('TouchWireV'+str(version),wire_output(touch,version))
    encoded += byte_array('ScalarZeroWireV'+str(version),wire_output(0,version))
    encoded += byte_array("BufferWireV"+str(version), wire_output(bytes([2,0,13,0,0,3,2,2,0]),version))
    encoded += byte_array("EmptyBufferWireV"+str(version), wire_output(b"",version))
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--emit-fixture',type=Path,help='Write the actual immutable Kodiak fixture without running tests')
parser.add_argument('--output',type=Path)
parser.add_argument('--core-source',type=Path,default=crane/'Library/PepCrLib/Pep.c')
args = parser.parse_args()
fixture = "\n".join(definitions) + "\nstatic const CR_PEP_VALUE KodiakRoot = " + root + ";\n"
fixture += "static const CR_PEP_VALUE ApccRoot = " + apcc_root + ";\n"
fixture += "static const CR_PEP_VALUE TouchRoot = " + touch_root + ";\n" + encoded
if args.emit_fixture is not None:
    args.emit_fixture.write_text(fixture)
    raise SystemExit(0)
with tempfile.TemporaryDirectory(prefix="crane-pep-test-") as name:
    tmp = args.output.resolve() if args.output else Path(name)
    tmp.mkdir(parents=True,exist_ok=True)
    (tmp/'artifacts.json').unlink(missing_ok=True)
    runs=[]
    (tmp / "PepFixture.h").write_text(fixture)
    for kernel in (False, True):
        flags = ["clang", "-std=c11", "-g", "-O1", "-Wall", "-Wextra", "-Werror",
                 "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                 "-I" + str(tmp), "-I" + str(crane / "Include")]
        if kernel:
            flags += ["-D_KERNEL_MODE", "-pthread", "-I" + str(crane / "Tests/KernelMocks")]
        else:
            flags += ["-fshort-wchar", "-I" + str(workspace / "MU_BASECORE/MdePkg/Include"),
                      "-I" + str(workspace / "MU_BASECORE/MdePkg/Include/X64")]
        executable = tmp / ("pep-kernel" if kernel else "pep-native")
        subprocess.run([*flags, str(crane / "Tests/PepCrTest.c"),
                        str(args.core_source),
                        str(crane / "Library/PepCrLib/PepAcpi.c"),
                        "-o", str(executable)], check=True)
        result=subprocess.run([str(executable)],text=True,capture_output=True,timeout=30)
        (tmp/(executable.name+'.log')).write_text(result.stdout+result.stderr)
        result.check_returncode();print(result.stdout.strip())
        runs.append({'kernel':kernel,'binary_sha256':hashlib.sha256(executable.read_bytes()).hexdigest(),'output':result.stdout})
    inputs=[Path(__file__),source,args.core_source,crane/'Library/PepCrLib/PepAcpi.c',crane/'Tests/PepCrTest.c',crane/'Include/Library/PepCrLib.h']
    (tmp/'artifacts.json').write_text(json.dumps({'scope':'actual portable PEP policy/ACPI parser, native and NT ABI ASan/UBSan, original Kodiak fixture','runs':runs,'inputs':{str(f.resolve()):hashlib.sha256(f.read_bytes()).hexdigest() for f in inputs}},indent=2)+'\n')
print("Kodiak DSDT fixture SHA256", hashlib.sha256(source.read_bytes()).hexdigest())
