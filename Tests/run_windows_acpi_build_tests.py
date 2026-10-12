#!/usr/bin/env python3
"""Exercise the actual HDK table selection and pre-packaging rejection paths."""
from pathlib import Path
import argparse
import importlib.util
import json
import shutil
import struct
import tempfile
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parents[3]
SOURCE = ROOT / "Platforms/WaipioCrPkg/WindowsAcpiBuild.py"
spec = importlib.util.spec_from_file_location("windows_acpi_build", SOURCE)
acpi = importlib.util.module_from_spec(spec)
spec.loader.exec_module(acpi)


class Env(dict):
    def GetValue(self, key, default=None):
        return self.get(key, default)

    def SetValue(self, key, value, reason):
        self[key] = value


class Builder:
    def __init__(self, output, pair=None, device="qcom-hdk8450"):
        self.env = Env(TARGET_DEVICE=device)
        self.output = output
        if pair is not None:
            self.env.update(CR_HDK_WINDOWS_ACPI="1", CR_WINDOWS_ACPI_DIR=str(pair))

    def GetWorkspaceRoot(self):
        return str(ROOT)

    def GetOutputBinDirectory(self):
        return str(self.output)

    def GetOutputDirectory(self):
        return str(self.output)


def fail(call, text):
    try:
        call()
    except ValueError as error:
        assert text in str(error), str(error)
    else:
        raise AssertionError("accepted invalid input: " + text)


def fixture_fv(raw):
    """Minimal standards-layout container; payloads are real compiled tables."""
    sections = bytearray()
    for blob in raw:
        sections.extend((len(blob) + 4).to_bytes(3, "little") + bytes([0x19]) + blob)
        sections.extend(b"\0" * (-len(sections) % 4))
    ffs = bytearray(24)
    ffs[:16] = acpi.ACPI_FILE_GUID.bytes_le
    ffs[18] = 2
    ffs[20:23] = (len(ffs) + len(sections)).to_bytes(3, "little")
    result = bytearray(56) + ffs + sections
    result.extend(b"\xff" * (-len(result) % 8))
    struct.pack_into("<Q", result, 32, len(result))
    result[40:44] = b"_FVH"
    struct.pack_into("<H", result, 48, 56)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pair", required=True, type=Path)
    parser.add_argument("--default-fv", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    directory, original_manifest, loaded = acpi.load_pair(args.pair)
    table_names = tuple(loaded)
    default_fv = args.default_fv.read_bytes()
    default_raw = [p.read_bytes() for p in acpi.expected_paths(ROOT, "qcom-hdk8450")]
    assert acpi.acpi_raw_sections(default_fv) == default_raw
    cases = []
    with tempfile.TemporaryDirectory(prefix="cr-acpi-") as tmp:
        work = Path(tmp)
        pair = work / "pair"
        pair.mkdir()
        for name in (*table_names, "manifest.json"):
            shutil.copy2(directory / name, pair / name)
        original = {p.name: p.read_bytes() for p in pair.iterdir()}

        def restore():
            for name, blob in original.items():
                (pair / name).write_bytes(blob)

        def builder(pair_dir=pair, device="qcom-hdk8450"):
            b = Builder(work / "build", pair_dir, device)
            acpi.configure(b)
            acpi.prepare(b)
            return b

        b = builder(None)
        assert b.env["BLD_*_CR_HDK_WINDOWS_ACPI"] == "0"
        fv_path = Path(b.GetOutputBinDirectory()) / "FV/FVMAIN.Fv"
        fv_path.parent.mkdir(parents=True)
        fv_path.write_bytes(default_fv)
        acpi.verify(b)
        image = SimpleNamespace(output_path="keep.img", bootpayload_path="keep.bin")
        acpi.select_image(b, image)
        assert image.output_path == "keep.img" and image.bootpayload_path == "keep.bin"
        cases.append("real default FV and unchanged output path")

        fail(lambda: builder(device="qcom-mtp8450"), "only HDK8450")
        bad = Builder(work, pair)
        bad.env["CR_HDK_WINDOWS_ACPI"] = "2"
        fail(lambda: acpi.configure(bad), "must be 0 or 1")
        bad.env["CR_HDK_WINDOWS_ACPI"] = "0"
        fail(lambda: acpi.configure(bad), "without enabling")
        cases.append("board and option rejection")

        manifest = json.loads(original["manifest.json"])
        manifest["board"] = "other"
        (pair / "manifest.json").write_text(json.dumps(manifest))
        fail(lambda: acpi.load_pair(pair), "wrong board/profile")
        restore()
        for name in table_names:
            bad = bytearray(original[name])
            bad[-1] ^= 1
            (pair / name).write_bytes(bad)
            fail(lambda: acpi.load_pair(pair), "paired manifest" if name == "FACS.aml" else "invalid length/checksum")
            # A self-consistent ACPI checksum still must match the chosen pair.
            if name != "FACS.aml":
                bad[9] = (bad[9] - sum(bad)) & 0xff
            (pair / name).write_bytes(bad)
            fail(lambda: acpi.load_pair(pair), "paired manifest")
            restore()
        cases.append("wrong manifest and corrupted/mismatched compiled tables")

        if json.loads(original_manifest)["schema"] >= 2:
            # Mutate valid compiled tables and update hashes too: the structural
            # cross-table contract must reject them independently of integrity.
            def install_mutation(name, data):
                data = bytearray(data)
                struct.pack_into("<I", data, 4, len(data))
                data[9] = (data[9] - sum(data)) & 0xff
                (pair / name).write_bytes(data)
                manifest = json.loads(original["manifest.json"])
                manifest["tables"][name] = {"bytes": len(data), "sha256": acpi.digest(data)}
                (pair / "manifest.json").write_text(json.dumps(manifest))

            bad = bytearray(original["IORT.aml"])
            count = struct.unpack_from("<I", bad, 36)[0]
            struct.pack_into("<I", bad, 36, count + 1)
            bad.extend(struct.pack("<BHBIIIII", 0, 24, 1, 0, 0, 0, 1, 0xFFFFFFFF))
            install_mutation("IORT.aml", bad)
            fail(lambda: acpi.load_pair(pair), "IORT references absent MADT ITS")
            restore()
            bad = bytearray(original["MADT.aml"])
            bad[45] = 0
            install_mutation("MADT.aml", bad)
            fail(lambda: acpi.load_pair(pair), "invalid MADT entry bounds")
            restore()
            manifest = json.loads(original["manifest.json"])
            del manifest["tables"]["MADT.aml"]
            (pair / "manifest.json").write_text(json.dumps(manifest))
            fail(lambda: acpi.load_pair(pair), "table set")
            restore()
            cases.append("schema2 complete set and checksummed dangling ITS/malformed MADT rejection")

            iort = original["IORT.aml"]
            node_count, node_at = struct.unpack_from("<II", iort, 36)
            nodes = []
            for _ in range(node_count):
                kind, size, _, _, maps, offset = struct.unpack_from("<BHBIII", iort, node_at)
                nodes.append((node_at, kind, size, maps, offset))
                node_at += size
            smmu = next(node for node in nodes if node[1] == 3)
            named = [node for node in nodes if node[1] == 1]
            assert len(named) >= 2 and named[0][3] > 0

            # These are actual node-relative IRQ arrays, not the separate ID
            # mapping array. Checksums/manifests are updated for every fault.
            for offset, value, message in (
                    (44, 0xFFFFFFFF, "context interrupt array bounds"),
                    (48, smmu[2] + 8, "context interrupt array bounds"),
                    (52, 0xFFFFFFFF, "PMU interrupt array bounds"),
                    (40, smmu[2] + 8, "global interrupt array bounds"),
                    (56, smmu[2] + 8, "empty interrupt array offset"),
                    (48, 60, "overlapping IORT SMMU interrupt arrays")):
                bad = bytearray(iort)
                struct.pack_into("<I", bad, smmu[0] + offset, value)
                install_mutation("IORT.aml", bad)
                fail(lambda: acpi.load_pair(pair), message)
                restore()
            # The native table legitimately uses end-of-node for an empty PMU
            # array; accepting the unchanged real pair protects that boundary.
            acpi.load_pair(pair)

            last = nodes[-1]
            assert last[1] == 1
            bad = bytearray(iort[:last[0] + 16])
            struct.pack_into("<H", bad, last[0] + 1, 16)
            struct.pack_into("<II", bad, last[0] + 8, 0, 0)
            install_mutation("IORT.aml", bad)
            fail(lambda: acpi.load_pair(pair), "truncated IORT named component")
            restore()
            bad = bytearray(iort)
            start, _, _, _, offset = named[0]
            bad[start + 29:start + offset] = b"X" * (offset - 29)
            install_mutation("IORT.aml", bad)
            fail(lambda: acpi.load_pair(pair), "name bounds")
            restore()
            # A valid node offset can still describe an invalid route. Test a
            # cycle separately from an acyclic named-to-named wrong-type edge.
            for output, message in ((start, "cyclic IORT node route"),
                                    (named[1][0], "invalid IORT output node type"),
                                    (0xFFFFFFFF, "invalid IORT node reference")):
                bad = bytearray(iort)
                struct.pack_into("<I", bad, start + offset + 12, output)
                install_mutation("IORT.aml", bad)
                fail(lambda: acpi.load_pair(pair), message)
                restore()
            bad = bytearray(iort)
            bad[start] = 4
            install_mutation("IORT.aml", bad)
            fail(lambda: acpi.load_pair(pair), "unsupported IORT node type")
            restore()
            bad = bytearray(original["MADT.aml"])
            madt_at = 44
            while bad[madt_at] != 15:
                madt_at += bad[madt_at + 1]
            bad[madt_at] = 11  # A 20-byte ITS body cannot stand in for GICC80.
            install_mutation("MADT.aml", bad)
            fail(lambda: acpi.load_pair(pair), "truncated MADT interrupt controller")
            restore()
            # Positive ITS reference control: this profile's real MADT has ITS0.
            bad = bytearray(iort)
            struct.pack_into("<I", bad, 36, node_count + 1)
            bad.extend(struct.pack("<BHBIIIII", 0, 24, 1, 99, 0, 0, 1, 0))
            install_mutation("IORT.aml", bad)
            acpi.load_pair(pair)
            restore()
            cases.append("schema2 typed bodies, IRQ arrays, names, cycles and route types; valid ITS and empty-array controls")

        if json.loads(original_manifest)["schema"] == 3:
            b = builder()
            assert Path(b.env["BLD_*_CR_WINDOWS_ACPI_BUILTIN_DIR"]) == pair
            selected = acpi.expected_paths(ROOT, "qcom-hdk8450", pair)
            assert len(selected) == 14 and all(p.parent == pair for p in selected)
            assert [p.name for p in selected[-3:]] == list(acpi.BUILTIN_TABLES)
            assert [p.read_bytes()[:4] for p in selected[-3:]] == [b"SSDT", b"SSDT", b"TPM2"]
            for name in acpi.BUILTIN_TABLES:
                manifest = json.loads(original["manifest.json"])
                del manifest["tables"][name]
                (pair / "manifest.json").write_text(json.dumps(manifest))
                fail(lambda: acpi.load_pair(pair), "table set")
                restore()
                bad = bytearray(original[name])
                bad[:4] = b"TPM2" if bad[:4] == b"SSDT" else b"SSDT"
                install_mutation(name, bad)
                fail(lambda: acpi.load_pair(pair), "invalid ACPI header")
                restore()
            # Each legacy schema still selects the inherited builtin directory,
            # without silently accepting schema3-only manifest entries.
            for schema in (1, 2):
                manifest = json.loads(original["manifest.json"])
                manifest["schema"] = schema
                keep = acpi.TABLES if schema == 1 else acpi.TABLES + acpi.AUX_TABLES
                manifest["tables"] = {name: manifest["tables"][name] for name in keep}
                (pair / "manifest.json").write_text(json.dumps(manifest))
                legacy = builder()
                inherited = ROOT / "Platforms/MUCommonCrPkg/AcpiTables/builtin"
                assert Path(legacy.env["BLD_*_CR_WINDOWS_ACPI_BUILTIN_DIR"]) == inherited
                assert all(p.parent == inherited for p in acpi.expected_paths(ROOT, "qcom-hdk8450", pair)[-3:])
                restore()
            cases.append("schema3 all14 generated inputs, typed TPM signatures and schema1/2 builtin compatibility")

        manifest = json.loads(original["manifest.json"])
        manifest["native_inputs"][acpi.NATIVE_INPUTS[0]] = "0" * 64
        (pair / "manifest.json").write_text(json.dumps(manifest))
        fail(lambda: builder(), "different native inputs")
        del manifest["native_inputs"]
        (pair / "manifest.json").write_text(json.dumps(manifest))
        fail(lambda: builder(), "incomplete native reservation/baseline witnesses")
        restore()
        cases.append("mismatched or absent native memory/reservation witnesses")

        b = builder()
        raw = [Path(p).read_bytes() for p in b.cr_acpi_snapshot["inputs"]]
        candidate_fv = fixture_fv(raw)
        fv_path.write_bytes(candidate_fv)
        acpi.verify(b)
        acpi.select_image(b, image)
        assert image.output_path.endswith("qcom-hdk8450-windows-cr.img")
        assert image.bootpayload_path.endswith("bootpayload-windows-cr.bin")
        receipt = json.loads((fv_path.parent / "acpi-selection.json").read_text())
        assert receipt["profile"] == "windows-cr" and receipt["raw_table_count"] == 14
        cases.append("paired real table bytes and separate image selection")

        (pair / "manifest.json").write_bytes(original["manifest.json"] + b"\n")
        fail(lambda: acpi.verify(b), "manifest changed during build")
        restore()
        fv_path.write_bytes(default_fv)
        fail(lambda: acpi.verify(b), "generated FV ACPI tables differ")
        for bad_raw in (raw + [raw[6]], raw[:6] + raw[7:], raw[::-1]):
            fv_path.write_bytes(fixture_fv(bad_raw))
            fail(lambda: acpi.verify(b), "generated FV ACPI tables differ")
        cases.append("post-build drift, stale/default FV, duplicate/missing/reordered tables")

        for data, message in ((candidate_fv[:-1], "invalid FV bounds"),
                              (candidate_fv[:24], "invalid FV header")):
            fail(lambda: acpi.acpi_raw_sections(data), message)
        malformed = bytearray(candidate_fv)
        malformed[76:79] = b"\x01\0\0"
        fail(lambda: acpi.acpi_raw_sections(malformed), "invalid FFS bounds")
        malformed = bytearray(candidate_fv)
        malformed[80:83] = b"\x01\0\0"
        fail(lambda: acpi.acpi_raw_sections(malformed), "invalid ACPI section bounds")
        cases.append("truncated FV and invalid nested FFS/section bounds")
    receipt = {"cases": cases, "source_sha256": acpi.digest(SOURCE.read_bytes()),
               "schema": json.loads(original_manifest)["schema"],
               "test_sha256": acpi.digest(Path(__file__).read_bytes()),
               "default_fv_sha256": acpi.digest(default_fv),
               "pair_manifest_sha256": acpi.digest((directory / "manifest.json").read_bytes())}
    (args.output / "artifacts.json").write_text(json.dumps(receipt, indent=2) + "\n")
    print("HDK ACPI build selection/rejection paths PASS:", len(cases), "groups")


if __name__ == "__main__":
    main()
