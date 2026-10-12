#!/usr/bin/env python3
"""Compare actual unused SM8250 profile data against pinned original sources/FDT."""
from pathlib import Path
import argparse
import hashlib
import json
import re
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    out = parser.parse_args().output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    root = Path(__file__).resolve().parents[3]
    pkg = root / 'Platforms/CranePkg'
    android_commit = 'cad09e061ef6cf689a4a1e54d27562e0f042236a'
    mainline_commit = 'a53261b60834ce617cc4f3c7723dda8285c59789'
    commands, frozen = [], {}

    def digest(path):
        return hashlib.sha256(path.read_bytes()).hexdigest()

    def freeze(path):
        path = Path(path).resolve()
        value = digest(path)
        assert path not in frozen or frozen[path] == value, f'Input drift: {path}'
        frozen[path] = value
        return path

    def run(args, **kwargs):
        args = list(map(str, args))
        commands.append(args)
        result = subprocess.run(args, capture_output=True, text=True, **kwargs)
        if result.returncode:
            print(result.stdout + result.stderr)
            result.check_returncode()
        return result.stdout

    def original(repo, commit, name, local):
        path = freeze(root / local)
        data = run(['git', '-C', root / repo, 'show', commit + ':' + name])
        assert data == path.read_text(), f'Unpinned reference: {path}'
        return data

    ad = original('.vscode/android_kernel_oneplus_sm8250', android_commit,
                  'drivers/clk/qcom/dispcc-kona.c',
                  '.tmp/waipio-continuation/hdk8245-panel/android-sm8250/drivers/clk/qcom/dispcc-kona.c')
    ag = original('.vscode/android_kernel_oneplus_sm8250', android_commit,
                  'drivers/clk/qcom/gcc-kona.c',
                  '.tmp/waipio-continuation/hdk8245-cmd-phy/clock-plan/android-gcc-kona.c')
    md = original('.vscode/linux', mainline_commit,
                  'drivers/clk/qcom/dispcc-sm8250.c', '.vscode/linux/drivers/clk/qcom/dispcc-sm8250.c')
    mg = original('.vscode/linux', mainline_commit,
                  'drivers/clk/qcom/gcc-sm8250.c', '.vscode/linux/drivers/clk/qcom/gcc-sm8250.c')
    avdd = original('.vscode/android_kernel_oneplus_sm8250', android_commit,
                    'drivers/clk/qcom/vdd-level.h', '.vscode/android_kernel_oneplus_sm8250/drivers/clk/qcom/vdd-level.h')
    alevels = original('.vscode/android_kernel_oneplus_sm8250', android_commit,
                    'include/dt-bindings/regulator/qcom,rpmh-regulator-levels.h',
                    '.vscode/android_kernel_oneplus_sm8250/include/dt-bindings/regulator/qcom,rpmh-regulator-levels.h')
    apll = original('.vscode/android_kernel_oneplus_sm8250', android_commit,
                    'drivers/clk/qcom/clk-alpha-pll.c',
                    '.tmp/waipio-continuation/hdk8245-cmd-phy/clock-plan/android-clk-alpha-pll.c')
    arpmh = original('.vscode/android_kernel_oneplus_sm8250', android_commit,
                    'drivers/clk/qcom/clk-rpmh.c', '.vscode/android_kernel_oneplus_sm8250/drivers/clk/qcom/clk-rpmh.c')
    dt = freeze(root / 'hdk8245_fdt')
    assert digest(dt) == '2743c213f1c286030eb65647c693cf0e2dfa69ee2a84ceac0e17a86d82de3070'

    # These parsers read original initializers; they never read production C
    # for expected register offsets, masks, parents, widths or halt policies.
    def block(text, name):
        match = re.search(r'\b' + re.escape(name) + r'\s*(?:\[\])?\s*=\s*\{', text)
        assert match, name
        start = text.index('{', match.start())
        end, depth = start + 1, 1
        while depth:
            depth += (text[end] == '{') - (text[end] == '}')
            end += 1
        return text[start:end]

    def field(text, key, default=None):
        match = re.search(r'\.' + key + r'\s*=\s*([^,\n]+)', text)
        if not match:
            assert default is not None, key
            return default
        return match[1].strip()

    def number(text, key, default=None):
        value = field(text, key, default)
        return value if isinstance(value, int) else int(value, 0)

    def fdt(node, prop, kind='x'):
        value = run(['fdtget', '-t', kind, dt, node, prop]).strip()
        return value if kind == 's' else [int(x, 16) for x in value.split()]

    def props(node):
        return run(['fdtget', '-p', dt, node]).splitlines()

    expected = {}

    def put(prefix, **values):
        for key, value in values.items():
            expected[prefix + '.' + key] = str(int(value) if isinstance(value, bool) else value)

    put('profile', Id=0x82450001, Soc=8250, Board=8245, DsiControllerMask=1,
        AndroidCommit=android_commit, MainlineCommit=mainline_commit, FdtSha256=digest(dt),
        ControllerCount=2, RcgCount=6, BranchCount=12)
    for name, node in [('gcc', '/soc/qcom,gcc@100000'), ('dispcc', '/soc/qcom,dispcc@af00000')]:
        base, size = fdt(node, 'reg')
        put('window.' + name, Physical=base, Bytes=size, Compatible=fdt(node, 'compatible', 's').split()[0])
    xo_board = fdt('/soc/clocks/xo-board', 'clock-frequency')[0]
    xo_ref = re.search(r'DEFINE_CLK_RPMH_ARC\(kona,\s*bi_tcxo,\s*bi_tcxo_ao,\s*"([^"]+)",\s*(0x[\da-f]+),\s*(\d+)\)', arpmh)
    assert xo_ref
    xo_divider, xo_on = int(xo_ref[3]), int(xo_ref[2], 0)
    assert 'return prate / r->div;' in arpmh
    xo = xo_board // xo_divider
    xo_enable = int(re.search(r'#define CLK_RPMH_ARC_EN_OFFSET\s+(\d+)', arpmh)[1])
    macro = arpmh[arpmh.index('#define __DEFINE_CLK_RPMH'):arpmh.index('#define DEFINE_CLK_RPMH_ARC')]
    normal = macro[:macro.index('static struct clk_rpmh _platform##_##_name_active =')]
    assert '.parent_names = (const char *[]){ "xo_board" }' in normal
    put('xo', Name='bi_tcxo', ParentName=fdt('/soc/clocks/xo-board', 'clock-output-names', 's'),
        RpmhResource=xo_ref[1], BoardHz=xo_board, OutputHz=xo, Divider=xo_divider,
        EnableOffset=xo_enable, OnValue=xo_on, ActiveSet='BIT(RPMH_ACTIVE_ONLY_STATE)' in normal,
        WakeSet='BIT(RPMH_WAKE_ONLY_STATE)' in normal, SleepSet='BIT(RPMH_SLEEP_STATE)' in normal)
    pll_config = block(ad, 'disp_cc_pll1_config')
    pll_rate = (number(pll_config, 'l') * 65536 + number(pll_config, 'alpha')) * xo // 65536
    # ABI IDs and the selected route are explicit contract choices. All
    # hardware fields below are looked up by that selected route in originals.
    sources = {'bi_tcxo': 1, 'disp_cc_pll1': 2, 'dsi0_phy_pll_out_byteclk': 3,
               'dsi0_phy_pll_out_dsiclk': 4}
    choices = [('ahb', 5, 'bi_tcxo', xo), ('mdp', 6, 'disp_cc_pll1', 200000000),
               ('vsync', 7, 'bi_tcxo', xo), ('esc0', 8, 'bi_tcxo', xo),
               ('byte0', 9, 'dsi0_phy_pll_out_byteclk', 0),
               ('pclk0', 10, 'dsi0_phy_pll_out_dsiclk', 0)]
    for short, ident, parent, rate in choices:
        name = 'disp_cc_mdss_' + short + '_clk_src'
        source, cross = block(ad, name), block(md, name)
        names = re.findall(r'"([^"]+)"', block(ad, field(source, 'parent_names')))
        maps = re.findall(r'\{\s*(P_\w+),\s*(\d+)\s*\}', block(ad, field(source, 'parent_map')))
        token, selector = maps[names.index(parent)]
        main_map = dict(re.findall(r'\{\s*(P_\w+),\s*(\d+)\s*\}', block(md, field(cross, 'parent_map'))))
        assert main_map[token] == selector
        for key in ('cmd_rcgr', 'hid_width', 'mnd_width'):
            assert number(source, key) == number(cross, key), (name, key)
        if rate:
            table = block(ad, field(source, 'freq_tbl'))
            rates = re.findall(r'F\(\s*(\d+),\s*(P_\w+),\s*([\d.]+),\s*(\d+),\s*(\d+)\)', table)
            selected = next(row for row in rates if int(row[0]) == rate and row[1] == token)
            hid, m, n = int(2 * float(selected[2]) - 1), int(selected[3]), int(selected[4])
            parent_hz = pll_rate if parent == 'disp_cc_pll1' else xo
            assert parent_hz * 2 // (hid + 1) == rate
        else:
            # The qualified external plan selects unity HID and bypass M/N.
            # ClockExternal/PHY ownership will validate its actual rates later.
            hid, m, n = 1, (1 if short == 'pclk0' else 0), (1 if short == 'pclk0' else 0)
        sources[name] = ident
        put('rcg.' + name, Id=ident, Parent=sources[parent], ParentName=parent,
            CommandOffset=number(source, 'cmd_rcgr'), HidWidth=number(source, 'hid_width'),
            MndWidth=number(source, 'mnd_width'), Selector=int(selector), HidValue=hid,
            M=m, N=n, FixedHz=rate, AndroidSafeConfig='enable_safe_config = true' in source,
            AndroidHardwareControl='HW_CLK_CTRL_MODE' in source)
    sources['disp_cc_mdss_byte0_div_clk_src'] = 11
    halt = {'BRANCH_HALT': 0, 'BRANCH_HALT_DELAY': 1, 'BRANCH_VOTED': 2, 'BRANCH_HALT_VOTED': 2}
    branches = ['gcc_disp_' + x + '_clk' for x in ('ahb', 'xo', 'hf_axi', 'sf_axi')]
    branches += ['disp_cc_mdss_' + x + '_clk' for x in ('ahb', 'mdp', 'mdp_lut', 'vsync', 'esc0', 'byte0', 'byte0_intf', 'pclk0')]
    for name in branches:
        is_gcc = name.startswith('gcc_')
        source = block(ag if is_gcc else ad, name)
        if name == 'gcc_disp_ahb_clk':
            probe = re.search(r'qcom_branch_set_clk_en\(regmap,\s*(0x[\da-f]+)\); /\* GCC_DISP_AHB_CLK \*/', mg)
            assert probe and int(probe[1], 16) == number(source, 'enable_reg')
            main_halt = 3
        else:
            cross = block(mg if is_gcc else md, name)
            for key in ('enable_reg', 'halt_reg'):
                assert number(source, key) == number(cross, key)
            assert field(cross, 'enable_mask') == field(source, 'enable_mask') == 'BIT(0)'
            main_halt = halt[field(cross, 'halt_check')]
        enable_mask = 1 << int(re.fullmatch(r'BIT\((\d+)\)', field(source, 'enable_mask'))[1])
        parent = 0
        if not is_gcc:
            parents = re.search(r'\.parent_names\s*=\s*\(const char \*\[\]\)\s*\{\s*"([^"]+)"', source)
            assert parents, name
            parent = sources[parents[1]]
        put('branch.' + name, Controller=0 if is_gcc else 1, Parent=parent,
            EnableOffset=number(source, 'enable_reg'), EnableMask=enable_mask,
            HaltOffset=number(source, 'halt_reg'), HwcgOffset=number(source, 'hwcg_reg', 0),
            HwcgMask=(1 << number(source, 'hwcg_bit')) if '.hwcg_bit' in source else 0,
            AndroidHalt=halt[field(source, 'halt_check')], MainlineHalt=main_halt,
            AndroidCritical='CLK_IS_CRITICAL' in source)
    divider_name = 'disp_cc_mdss_byte0_div_clk_src'
    divider = block(ad, divider_name)
    cross = block(md, divider_name)
    for key in ('reg', 'shift', 'width'):
        assert number(divider, key) == number(cross, key)
    put('divider', Name=divider_name, Id=11, Parent=9, Offset=number(divider, 'reg'),
        Shift=number(divider, 'shift'), Width=number(divider, 'width'), Divisor=2,
        ReadOnly=field(divider, 'ops') == '&clk_regmap_div_ro_ops')
    domain = block(md, 'mdss_gdsc')
    node = '/soc/qcom,gdsc@af03000'
    domain_offset = fdt(node, 'reg')[0] - fdt('/soc/qcom,dispcc@af00000', 'reg')[0]
    assert domain_offset == number(domain, 'gdscr')
    put('gdsc', Name=fdt(node, 'regulator-name', 's'), Offset=domain_offset,
        FdtHardwareTrigger='qcom,support-hw-trigger' in props(node),
        FdtRetainRegisters='qcom,retain-regs' in props(node),
        MainlineHardwareControl='HW_CTRL' in field(domain, 'flags'),
        MainlineRetainFlipFlops='RETAIN_FF_ENABLE' in field(domain, 'flags'),
        MainlineEnableRestWait=number(domain, 'en_rest_wait_val'),
        MainlineEnableFewWait=number(domain, 'en_few_wait_val'),
        MainlineClockDisableWait=number(domain, 'clk_dis_wait_val'))
    put('pll1', Name='disp_cc_pll1', ParentName='bi_tcxo', Offset=number(block(ad, 'disp_cc_pll1'), 'offset'),
        DirectOwnerProfile=0x825001, ReferenceHz=xo, OutputHz=pll_rate)
    # Source-qualified rate -> common ARC level; board parent identity remains
    # a FDT fact, and selector/address resolution is deliberately absent.
    level_constants = {x: int(y) for x, y in re.findall(r'#define\s+(RPMH_REGULATOR_LEVEL_\w+)\s+(\d+)', alevels)}
    enum_levels = {x: level_constants[y] for x, y in re.findall(r'\[(VDD_\w+)\]\s*=\s*(RPMH_REGULATOR_LEVEL_\w+)', avdd)}
    max_rates = re.findall(r'\[(VDD_\w+)\]\s*=\s*(\d+)', block(ad, 'disp_cc_pll1'))
    def rate_level(rate):
        return next(enum_levels[x] for x, y in max_rates if rate <= int(y))
    cal_l = int(re.search(r'#define LUCID_PLL_CAL_VAL\s+(0x[\da-fA-F]+)', apll)[1], 0)
    mmcx_parent = '/soc/rsc@18200000/rpmh-regulator-mmcxlvl'
    mmcx = mmcx_parent + '/regulator-pm8150a-s4-level'
    mx_parent = '/soc/rsc@18200000/rpmh-regulator-mxlvl'
    mx = mx_parent + '/regulator-pm8150a-s3-mmcx-sup-level'
    cx_parent = '/soc/rsc@18200000/rpmh-regulator-cxlvl'
    cx = cx_parent + '/regulator-pm8150-s3-mmcx-sup-level'
    assert fdt('/soc/qcom,dispcc@af00000', 'vdd_mm-supply') == fdt(mmcx, 'phandle')
    assert fdt(mmcx_parent, 'pm8150a_s4_level-parent-supply') == fdt(mx, 'phandle')
    assert fdt(mx_parent, 'pm8150a_s3_mmcx_sup_level-parent-supply') == fdt(cx, 'phandle')
    minimum = fdt(mmcx, 'regulator-min-microvolt')[0]
    dropout = fdt(mmcx, 'qcom,min-dropout-voltage-level')[0]
    sets = fdt(mmcx, 'qcom,set')[0]
    put('rails', SupplyName='vdd_mm', MmcxResource=fdt(mmcx_parent, 'qcom,resource-name', 's'),
        MxResource=fdt(mx_parent, 'qcom,resource-name', 's'), CxResource=fdt(cx_parent, 'qcom,resource-name', 's'),
        MmcxBoardMinimum=minimum, MmcxCalibrationMinimum=max(minimum, rate_level(cal_l * xo)),
        MmcxOperatingMinimum=max(minimum, rate_level(pll_rate)),
        CxParentMinimum=fdt(cx, 'regulator-min-microvolt')[0], ActiveSet=bool(sets & 1),
        SleepSet=bool(sets & 2), MxDropoutFromSelectedMmcx=dropout - (1 << 32) if dropout & (1 << 31) else dropout)
    (out / 'source-derived-expected.json').write_text(json.dumps(expected, indent=2) + '\n')
    sources_c = [freeze(pkg / 'Tests/Sm8250DispccProfileTest.c'),
                 freeze(pkg / 'Target/Sm8250/CrDispccProfile.c')]
    freeze(Path(__file__))

    def dependencies(flags, source, dep):
        run(flags + ['-MM', '-MF', dep, source])
        for item in dep.read_text().split(':', 1)[1].replace('\\\n', '').split():
            freeze(Path(item))

    for kernel in (False, True):
        mode = 'nt' if kernel else 'native'
        flags = ['clang', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                 '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-I' + str(pkg / 'Include')]
        if kernel:
            flags += ['-D_KERNEL_MODE', '-I' + str(pkg / 'Tests/KernelMocks')]
        else:
            flags += ['-fshort-wchar', '-I' + str(root / 'MU_BASECORE/MdePkg/Include'),
                      '-I' + str(root / 'MU_BASECORE/MdePkg/Include/X64')]
        objects = []
        for i, source in enumerate(sources_c):
            obj, dep = out / f'{mode}-{i}.o', out / f'{mode}-{i}.d'
            dependencies(flags, source, dep)
            run(flags + ['-c', source, '-o', obj])
            objects.append(obj)
        exe = out / mode
        run(['clang', '-fsanitize=address,undefined'] + objects + ['-o', exe])
        output = run([exe], timeout=30)
        (out / f'{mode}.tsv').write_text(output)
        rows = [line.split('\t', 1) for line in output.splitlines()]
        actual = dict(rows)
        assert len(rows) == len(actual), 'duplicate output field'
        if actual != expected:
            delta = {key: [expected.get(key), actual.get(key)] for key in expected.keys() | actual.keys()
                     if expected.get(key) != actual.get(key)}
            raise AssertionError(json.dumps(delta, indent=2))
        print(f'{mode}: PASS {len(actual)} source-derived fields; unsupported-ID and graph bounds checks')
    arm_flags = ['clang', '--target=aarch64-none-elf', '-std=c11', '-O2', '-g', '-ffreestanding',
                 '-fshort-wchar', '-Wall', '-Wextra', '-Werror', '-I' + str(pkg / 'Include'),
                 '-I' + str(root / 'MU_BASECORE/MdePkg/Include'),
                 '-I' + str(root / 'MU_BASECORE/MdePkg/Include/AArch64')]
    dependencies(arm_flags, sources_c[1], out / 'aarch64.d')
    arm_obj = out / 'CrDispccProfile.aarch64.o'
    run(arm_flags + ['-c', sources_c[1], '-o', arm_obj])
    undefined = run(['llvm-nm', '--undefined-only', arm_obj])
    assert not undefined.strip(), 'Pure profile unexpectedly requires external runtime symbols'
    print('aarch64: PASS; pure object has no undefined runtime symbols')
    for path, value in frozen.items():
        assert digest(path) == value, f'Input changed during tests: {path}'
    receipt = {'status': 'PASS', 'scope': __doc__, 'field_count': len(expected), 'commands': commands,
               'inputs': {str(path): value for path, value in sorted(frozen.items())},
               'artifacts': {path.name: digest(path) for path in sorted(out.iterdir())
                             if path.is_file() and path.name != 'artifacts.json'}}
    (out / 'artifacts.json').write_text(json.dumps(receipt, indent=2) + '\n')


if __name__ == '__main__':
    main()
