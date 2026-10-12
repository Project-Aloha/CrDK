#!/usr/bin/env python3
"""Assemble the real Windows ARM64 wrapper and emulate its calling convention.

Requires an ARM64-COFF LLVM toolchain, real SDK shared headers, and Unicorn.
The SMC instruction is intercepted: this proves wrapper ABI/metadata, not that
Windows firmware grants PAS access. Microsoft assembler execution is separate.
"""
import argparse
import hashlib
import json
from pathlib import Path
import random
import re
import struct
import subprocess
import sys


def run(args):
    return subprocess.run(args, check=True, text=True, capture_output=True).stdout


def section_bytes(obj, name):
    data = obj.read_bytes()
    machine, count, _, _, _, optional, _ = struct.unpack_from('<HHIIIHH', data)
    assert machine == 0xaa64, 'not ARM64 COFF'
    for index in range(count):
        offset = 20 + optional + index * 40
        section = data[offset:offset + 8].split(b'\0')[0].decode()
        size, pointer = struct.unpack_from('<II', data, offset + 16)
        relocations = struct.unpack_from('<H', data, offset + 32)[0]
        if section == name:
            assert relocations == 0, 'wrapper text unexpectedly has relocations'
            assert pointer + size <= len(data)
            return data[pointer:pointer + size]
    raise AssertionError('missing ' + name)


def instructions(path, microsoft=False):
    result = []
    for line in path.read_text().splitlines():
        line = line.split(';' if microsoft else '//')[0].strip().lower()
        if microsoft:
            match = re.fullmatch(r'prolog_save_reg_pair(?:_no_fp)? (\w+), (\w+), #(-?\d+)(!)?', line)
            if match:
                a, b, offset, writeback = match.groups()
                line = f'stp {a}, {b}, [sp, #{offset}]' + ('!' if writeback else '')
            match = re.fullmatch(r'epilog_restore_reg_pair (\w+), (\w+), #(\d+)(!)?', line)
            if match:
                a, b, offset, writeback = match.groups()
                line = f'ldp {a}, {b}, [sp], #{offset}' if writeback else f'ldp {a}, {b}, [sp, #{offset}]'
            if line == 'epilog_return':
                line = 'ret'
        if re.match(r'^(stp|ldp|mov|smc|mrs|lsr|ret)\b', line):
            line = re.sub(r'\bfp\b', 'x29', line)
            line = re.sub(r'\blr\b', 'x30', line)
            result.append(re.sub(r'\s+', '', line))
    return result


def emulate(code):
    from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
    from unicorn import arm64_const as regs
    register = [getattr(regs, 'UC_ARM64_REG_X' + str(n)) for n in range(31)]
    base, frame, stack = 0x100000, 0x200080, 0x302000
    smc = struct.pack('<I', 0xd4000003)
    assert code.count(smc) == 1, 'expected exactly SMC #0'
    smc_address = base + code.index(smc)
    randomizer = random.Random(0x50494c)
    for vector in range(32):
        uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        uc.mem_map(base, 0x1000)
        uc.mem_map(frame & ~0xfff, 0x1000)
        uc.mem_map(stack - 0x2000, 0x4000)
        uc.mem_write(base, code)
        guard = bytes(randomizer.randrange(256) for _ in range(192))
        uc.mem_write(frame - 64, guard)
        input_frame = [randomizer.getrandbits(64) for _ in range(8)]
        output_frame = [randomizer.getrandbits(64) for _ in range(8)]
        if vector == 0:
            input_frame = [0, 1, 0xffffffff, 0xffffffffffffffff, 0x8000000000000000, 0x42000205, 0x12345678, 0]
        uc.mem_write(frame, struct.pack('<8Q', *input_frame))
        saved = [randomizer.getrandbits(64) for _ in range(31)]
        saved[0], saved[30] = frame, base + len(code)
        for reg, value in zip(register, saved):
            uc.reg_write(reg, value)
        uc.reg_write(regs.UC_ARM64_REG_SP, stack)
        calls = []

        def hook(engine, address, size, _):
            if address != smc_address:
                return
            assert size == 4
            assert engine.reg_read(regs.UC_ARM64_REG_SP) % 16 == 0
            assert [engine.reg_read(reg) for reg in register[:8]] == input_frame
            assert [engine.reg_read(reg) for reg in register[8:18]] == [0] * 10
            calls.append(address)
            for reg, value in zip(register[:8], output_frame):
                engine.reg_write(reg, value)
            # Volatile registers may be destroyed by the qualified monitor.
            for reg in register[8:18]:
                engine.reg_write(reg, randomizer.getrandbits(64))
            engine.reg_write(regs.UC_ARM64_REG_PC, address + 4)

        uc.hook_add(UC_HOOK_CODE, hook)
        uc.emu_start(base, base + len(code), count=100)
        assert calls == [smc_address]
        assert struct.unpack('<8Q', uc.mem_read(frame, 64)) == tuple(output_frame)
        assert bytes(uc.mem_read(frame - 64, 64)) == guard[:64]
        assert bytes(uc.mem_read(frame + 64, 64)) == guard[128:]
        assert uc.reg_read(regs.UC_ARM64_REG_SP) == stack
        assert [uc.reg_read(reg) for reg in register[18:]] == saved[18:]
    return 32


def emulate_current_el(code):
    from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
    from unicorn import arm64_const as regs
    # Real assembled leaf is MRS CurrentEL, LSR #2, RET. Intercept only MRS;
    # supplied EL values exercise the actual shift/return without claiming a
    # host emulator establishes a Windows privilege/conduit contract.
    assert len(code) == 112 and struct.unpack_from('<III', code, 100) == (0xd5384240, 0xd342fc00, 0xd65f03c0)
    base = 0x100000
    for level in range(4):
        uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        uc.mem_map(base, 0x1000)
        uc.mem_write(base, code)
        saved = list(range(0x12340000, 0x12340000 + 31))
        saved[30] = base + len(code)
        for index, value in enumerate(saved):
            uc.reg_write(getattr(regs, 'UC_ARM64_REG_X' + str(index)), value)
        uc.reg_write(regs.UC_ARM64_REG_SP, 0xabcdef00)

        def hook(engine, address, _, __):
            if address == base + 100:
                engine.reg_write(regs.UC_ARM64_REG_X0, level << 2)
                engine.reg_write(regs.UC_ARM64_REG_PC, address + 4)

        uc.hook_add(UC_HOOK_CODE, hook)
        uc.emu_start(base + 100, base + len(code), count=4)
        assert uc.reg_read(regs.UC_ARM64_REG_X0) == level
        assert uc.reg_read(regs.UC_ARM64_REG_SP) == 0xabcdef00
        for index in range(1, 31):
            assert uc.reg_read(getattr(regs, 'UC_ARM64_REG_X' + str(index))) == saved[index]
    return 4


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--toolchain', type=Path, required=True)
    parser.add_argument('--sdk-shared', type=Path, required=True)
    parser.add_argument('--python-deps', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if args.python_deps:
        sys.path.insert(0, str(args.python_deps.resolve()))
    lib = Path(__file__).resolve().parents[1] / 'Library/PILCrLib'
    gas, microsoft = lib / 'PilSmcccArm64.S', lib / 'PilSmcccArm64.asm'
    assert instructions(gas) == instructions(microsoft, True), 'assembly spellings differ'
    args.output.mkdir(parents=True, exist_ok=True)
    obj = args.output / 'PilSmcccArm64.obj'
    clang = str(args.toolchain / 'clang')
    run([clang, '--target=aarch64-pc-windows-msvc', '-c', str(gas), '-o', str(obj)])
    preprocessed = run([clang, '-E', '-P', '-x', 'c', '-D_ARM64_', '-Wno-invalid-pp-token',
                        '-I' + str(args.sdk_shared), str(microsoft)])
    for macro in ('NESTED_ENTRY', 'PROLOG_SAVE_REG_PAIR', 'PROLOG_SAVE_REG_PAIR_NO_FP',
                  'EPILOG_RESTORE_REG_PAIR', 'EPILOG_RETURN'):
        assert re.search(r'\b' + macro + r'\b[^\n]*\$?', preprocessed), macro
    no_fp = re.search(r'PROLOG_SAVE_REG_PAIR_NO_FP \$Reg1, \$Reg2, \$Offset(.*?)\bMEND\b', preprocessed, re.S)
    assert no_fp and 'stp fp, lr' in no_fp[1]
    assert not re.search(r'\b(?:mov|add)\s+fp\b', no_fp[1]), 'SDK macro creates an unmatched frame pointer'
    (args.output / 'PilSmcccArm64.pre.asm').write_text(preprocessed)
    unwind = run([str(args.toolchain / 'llvm-readobj'), '--unwind', str(obj)])
    for text in ('Format: COFF-ARM64', 'Function: CrPilArm64Smccc', 'FunctionLength: 100',
                 'stp x29, x30, [sp, #16]', 'stp x19, x20, [sp, #-32]!'):
        assert text in unwind, 'unwind missing ' + text
    (args.output / 'unwind.txt').write_text(unwind)
    disassembly = run([str(args.toolchain / 'llvm-objdump'), '-d', str(obj)])
    (args.output / 'disassembly.txt').write_text(disassembly)
    code = section_bytes(obj, '.text')
    vectors = emulate(code)
    el_vectors = emulate_current_el(code)
    manifest = {'source_sha256': {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in (gas, microsoft)},
                'coff_sha256': hashlib.sha256(obj.read_bytes()).hexdigest(), 'abi_vectors': vectors,
                'current_el_vectors': el_vectors,
                'msvc_preprocessed_only': True, 'hardware_smc_executed': False}
    (args.output / 'artifacts.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(f'PIL ARM64 COFF/unwind + {vectors} SMCCC ABI/{el_vectors} CurrentEL vectors + SDK macro/source parity PASS')


if __name__ == '__main__':
    main()
