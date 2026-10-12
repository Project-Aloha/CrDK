#!/usr/bin/env python3
"""SM8250 command PHY actual-C faults and independently compiled source oracles."""
from pathlib import Path
import argparse
import hashlib
import json
import re
import subprocess
import xml.etree.ElementTree as ET

ap = argparse.ArgumentParser(description=__doc__)
ap.add_argument('--output', type=Path, required=True)
a = ap.parse_args()
root = Path(__file__).resolve().parents[3]
pkg = root / 'Platforms/CranePkg'
out = a.output.resolve()
out.mkdir(parents=True, exist_ok=True)
android = root / '.tmp/waipio-continuation/hdk8245-panel/android-sm8250/techpack/display/msm/dsi/dsi_phy_hw_v4_0.c'
mainline = root / '.vscode/linux/drivers/gpu/drm/msm/dsi/phy/dsi_phy_7nm.c'
xml = root / '.vscode/linux/drivers/gpu/drm/msm/registers/display/dsi_phy_7nm.xml'
dt = root / 'hdk8245_fdt'

def run(cmd, **kwargs):
    commands.append([str(x) for x in cmd])
    result = subprocess.run(cmd, text=True, capture_output=True, **kwargs)
    if result.returncode:
        print(result.stdout + result.stderr)
        result.check_returncode()
    return result

def function(text, name):
    match = re.search(r'^(?:static )?(?:void|int) ' + name + r'\(', text, re.M)
    assert match, name
    begin = text.index('{', match.start())
    level = 1
    end = begin + 1
    while level:
        level += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[match.start():end] + '\n'

commands = []
phy = android.read_text()
pll = mainline.read_text()
# Read the original binary properties, avoiding DTS binary/string ambiguities.
phy_node = '/soc/qcom,mdss_dsi_phy0@ae94400'
panel_node = '/soc/qcom,mdss_mdp@ae00000/qcom,mdss_dsi_r66451_fhd_plus_60hz_cmd'
def cells(node, name, kind='x'):
    return [int(v, 16) for v in run(['fdtget', '-t', kind, dt, node, name]).stdout.split()]

assert cells(phy_node, 'reg') == [0xae94400, 0x7c0, 0xae94200, 0x100]
lane = cells(phy_node, 'qcom,platform-lane-config', 'bx')
strength = cells(phy_node, 'qcom,platform-strength-ctrl', 'bx')
timing = cells(panel_node+'/qcom,mdss-dsi-display-timings/timing@0', 'qcom,mdss-dsi-panel-phy-timings', 'bx')
assert lane == [0, 0, 10, 10]*4 + [0, 0, 138, 138]
assert strength == [85, 3]*4 + [85, 0]
assert timing == [0, 16, 4, 4, 30, 30, 4, 4, 3, 2, 4, 0, 16, 20]
pre = '''/* Generated for tests from pinned GPL-2.0-only Qualcomm bodies. */
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
typedef uint8_t u8; typedef uint32_t u32; typedef uint64_t u64;
extern void OracleWrite(u32,u32);
#define BIT(n) (1U<<(n))
#define __iomem
#define mb() ((void)0)
#define DSI_PHY_WARN(...) ((void)0)
#define DSI_PHY_ERR(...) ((void)0)
#define DSI_PHY_DBG(...) ((void)0)
#define DSI_PHY_VERSION_4_1 1
#define DSI_LOGICAL_LANE_0 0
#define DSI_LOGICAL_LANE_1 1
#define DSI_LOGICAL_LANE_2 2
#define DSI_LOGICAL_LANE_3 3
#define DSI_LANE_MAX 5
#define DSI_PLL_SOURCE_STANDALONE 0
#define DSI_PLL_SOURCE_NATIVE 1
#define DSI_PLL_SOURCE_NON_NATIVE 2
static u32 ReferenceRevision;
#define DSI_R32(p,o) ((o)==0 ? ReferenceRevision : 0)
#define DSI_W32(p,o,v) OracleWrite((o),(v))
#define readl_poll_timeout_atomic(a,v,c,d,t) ((v)=1,(void)(a),(void)(d),(void)(t),(c)?0:1)
struct dsi_phy_hw {int version; void *base;};
struct dsi_phy_per_lane_cfgs {u32 lane[5][4],lane_v4[14];};
struct dsi_lane_map {u32 lane_map_v2[4];};
struct dsi_phy_cfg {
 struct dsi_lane_map lane_map;
 struct dsi_phy_per_lane_cfgs strength,lanecfg,timing;
 u64 bit_clk_rate_hz; int pll_source;
};
static int dsi_phy_conv_logical_to_phy_lane(struct dsi_lane_map *m,int n) {
 for(int i=0;i<4;i++)if((1U<<i)==m->lane_map_v2[n])return i;return -1;
}
'''
# Independent source constants and bodies, not production macro definitions.
defs = '\n'.join(line for line in phy.splitlines() if line.startswith('#define DSIPHY_')) + '\n'
names = ['dsi_phy_hw_v4_0_is_pll_on', 'dsi_phy_hw_v4_0_config_lpcdrx',
         'dsi_phy_hw_v4_0_lane_swap_config', 'dsi_phy_hw_v4_0_lane_settings',
         'dsi_phy_hw_v4_0_commit_phy_timing', 'dsi_phy_hw_v4_0_enable']
source = pre + defs + ''.join(function(phy, n) for n in names)
source += '''void AndroidPhyReference(u32 Revision) {
 struct dsi_phy_hw hw={.version=DSI_PHY_VERSION_4_1,.base=(void *)0x20000000};
 struct dsi_phy_cfg c={.lane_map={{1,2,4,8}},.bit_clk_rate_hz=327096544};
 ReferenceRevision=Revision;
'''
for i in range(5):
    for j in range(4):
        source += f' c.lanecfg.lane[{i}][{j}]={lane[i*4+j]};\n'
    for j in range(2):
        source += f' c.strength.lane[{i}][{j}]={strength[i*2+j]};\n'
for i, value in enumerate(timing):
    source += f' c.timing.lane_v4[{i}]={value};\n'
source += ' dsi_phy_hw_v4_0_enable(&hw,&c);\n}\n'
# PLL oracle compiles the real mainline commit and 4.1 analog branches.
ns = '{http://nouveau.freedesktop.org/}'
domain = next(d for d in ET.parse(xml).getroot().findall(ns+'domain') if d.get('name') == 'DSI_7nm_PHY_PLL')
source += '\n'.join(f'#define REG_DSI_7nm_PHY_PLL_{r.get("name")} {r.get("offset")}' for r in domain.findall(ns+'reg32')) + '\n'
source += '\n'.join(line for line in pll.splitlines() if line.startswith('#define DSI_PHY_7NM_QUIRK_')) + '\n'
source += '''
#define writel(v,a) OracleWrite(0x500U+(u32)((uintptr_t)(a)-0x10000000U),(v))
struct msm_dsi_phy_cfg {u32 quirks;};
struct msm_dsi_phy {struct msm_dsi_phy_cfg *cfg;void *pll_base;bool cphy_mode;};
struct dsi_pll_7nm {struct msm_dsi_phy *phy;u64 vco_current_rate;struct dsi_pll_7nm *slave;};
struct dsi_pll_config {u32 decimal_div_start,frac_div_start,pll_clock_inverters;};
'''
source += function(pll, 'dsi_pll_commit') + function(pll, 'dsi_pll_config_hzindep_reg')
source += '''void MainlinePllReference(void) {
 struct msm_dsi_phy_cfg cfg={.quirks=DSI_PHY_7NM_QUIRK_V4_1};
 struct msm_dsi_phy phy={.cfg=&cfg,.pll_base=(void *)0x10000000};
 struct dsi_pll_7nm pll={.phy=&phy,.vco_current_rate=654193088};
 /* Integer formula from dsi_pll_calc_dec_frac, with the exact DT reference. */
 u64 q=654193088ULL*(1ULL<<18)/(19200000ULL*2);
 struct dsi_pll_config c={(u32)(q>>18),(u32)(q&((1U<<18)-1)),0xa0};
 dsi_pll_commit(&pll,&c);dsi_pll_config_hzindep_reg(&pll);
}
'''
oracle = out / 'upstream-oracle.c'
oracle.write_text(source)
production = [pkg/'Library/MdssDsiCmdCrLib'/n for n in ['PhySm8250.c', 'Io.c', 'Plan.c', 'PanelR66451.c']]
production.append(pkg/'Library/MdssDscCrLib/MdssDsc.c')
inputs = [Path(__file__).resolve(), pkg/'Tests/MdssCmdPhyTest.c', android, mainline, xml, dt, oracle] + production
frozen = {p: hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
for kernel in (False, True):
    mode = 'nt' if kernel else 'native'
    flags = ['clang', '-std=c11', '-g', '-O1', '-Wall', '-Wextra', '-Werror',
             '-Wno-unused-parameter', '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
             '-I'+str(pkg/'Include')]
    if kernel:
        flags += ['-D_KERNEL_MODE', '-I'+str(pkg/'Tests/KernelMocks')]
    else:
        flags += ['-fshort-wchar', '-I'+str(root/'MU_BASECORE/MdePkg/Include'),
                  '-I'+str(root/'MU_BASECORE/MdePkg/Include/X64')]
    objects = []
    for i, src in enumerate([pkg/'Tests/MdssCmdPhyTest.c', oracle]+production):
        obj = out/f'{mode}-{i}.o'
        dep = out/f'{mode}-{i}.d'
        run(flags+['-MM', '-MF', dep, src])
        dependencies = [Path(p) for p in dep.read_text().split(':', 1)[1].replace('\\\n', '').split()]
        for p in dependencies:
            digest = hashlib.sha256(p.read_bytes()).hexdigest()
            assert p not in frozen or frozen[p] == digest, f'Input changed before compile: {p}'
            frozen[p] = digest
        run(flags+['-MMD', '-MF', dep, '-c', src, '-o', obj])
        inputs += [Path(p) for p in dep.read_text().split(':', 1)[1].replace('\\\n', '').split()]
        objects.append(obj)
    exe = out/mode
    run(['clang', '-fsanitize=address,undefined']+objects+['-o', exe])
    result = run([exe], timeout=30)
    (out/f'{mode}.log').write_text(result.stdout+result.stderr)
    print(mode+': '+result.stdout.strip())
run(['clang', '--target=aarch64-none-elf', '-std=c11', '-O2', '-g', '-ffreestanding',
     '-fshort-wchar', '-Wall', '-Wextra', '-Werror', '-I'+str(pkg/'Include'),
     '-I'+str(root/'MU_BASECORE/MdePkg/Include'), '-I'+str(root/'MU_BASECORE/MdePkg/Include/AArch64'),
     '-c', production[0], '-o', out/'PhySm8250.aarch64.o'])
for p, digest in frozen.items():
    assert hashlib.sha256(p.read_bytes()).hexdigest() == digest, f'Input changed during tests: {p}'
receipt = {'status': 'PASS', 'scope': __doc__, 'commands': commands,
           'dt_phy_window': [0xae94400, 0x7c0], 'pll_offset': 0x500,
           'vco_requested_hz': 654193088, 'vco_actual_hz': '20934178125/32',
           'lane_actual_hz': '20934178125/64',
           'inputs': {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(set(inputs))},
           'artifacts': {p.name: hashlib.sha256(p.read_bytes()).hexdigest()
                         for p in sorted(out.iterdir()) if p.is_file() and p.name!='artifacts.json'}}
(out/'artifacts.json').write_text(json.dumps(receipt, indent=2)+'\n')
