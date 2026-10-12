#!/usr/bin/env python3
"""Actual retained SM8250 Lucid owner versus original Android C operations."""
from pathlib import Path
import argparse
import hashlib
import json
import re
import subprocess

ap = argparse.ArgumentParser(description=__doc__)
ap.add_argument('--output', type=Path, required=True)
a = ap.parse_args()
root = Path(__file__).resolve().parents[3]
pkg = root/'Platforms/CranePkg'
out = a.output.resolve()
out.mkdir(parents=True, exist_ok=True)
reference = root/'.tmp/waipio-continuation/hdk8245-cmd-phy/clock-plan/android-clk-alpha-pll.c'
dispcc = root/'.tmp/waipio-continuation/hdk8245-panel/android-sm8250/drivers/clk/qcom/dispcc-kona.c'
text = reference.read_text()
commands = []

def run(cmd, **kwargs):
    commands.append([str(x) for x in cmd])
    r = subprocess.run(cmd, capture_output=True, text=True, **kwargs)
    if r.returncode:
        print(r.stdout+r.stderr)
        r.check_returncode()
    return r

def function(name):
    m = re.search(r'^(?:static )?(?:void|int) '+name+r'\(', text, re.M)
    assert m, name
    begin = text.index('{', m.start())
    end, depth = begin+1, 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[m.start():end]+'\n'

names = ['lucid_pll_is_enabled', 'clk_alpha_pll_custom_configure',
         'clk_lucid_pll_configure', 'alpha_pll_lucid_enable', 'alpha_pll_lucid_disable']
bodies = ''.join(function(n) for n in names)
fields = set(re.findall(r'config->(\w+)', bodies))-{'custom_reg_offset', 'custom_reg_val'}
source = '''/* Independent pinned Android GPL-2.0-only bodies, test ABI only. */
#include <stdint.h>
#include <assert.h>
typedef uint32_t u32;
#define BIT(n) (1U<<(n))
#define wmb() ((void)0)
#define mb() ((void)0)
extern void OracleLucidWrite(u32,u32);
struct regmap {int unused;};
struct clk_alpha_pll {u32 offset;struct {struct regmap *regmap;} clkr;};
struct clk_hw {struct clk_alpha_pll *pll;};
#define to_clk_alpha_pll(h) ((h)->pll)
struct alpha_pll_config {
'''
source += ''.join(f' u32 {f};\n' for f in sorted(fields))
source += ''' const u32 *custom_reg_offset,*custom_reg_val;
};
static u32 R[0x44/4];
static int regmap_read(struct regmap *m,u32 o,u32 *v) {
 (void)m;assert(!(o&3)&&o<sizeof(R));*v=R[o/4];
 if(o==0&&R[0x38/4]==1)*v|=1U<<31;return 0;
}
static int regmap_write(struct regmap *m,u32 o,u32 v) {
 (void)m;assert(!(o&3)&&o<sizeof(R));OracleLucidWrite(o,v);
 R[o/4]=o?v:v&~(1U<<31);return 0;
}
static int regmap_update_bits(struct regmap *m,u32 o,u32 mask,u32 v) {
 u32 before;regmap_read(m,o,&before);return regmap_write(m,o,(before&~mask)|(v&mask));
}
static int clk_enable_regmap(struct clk_hw *h){(void)h;assert(0);return -1;}
static void clk_disable_regmap(struct clk_hw *h){(void)h;assert(0);}
static int wait_for_pll_enable_active(struct clk_alpha_pll *p){(void)p;assert(0);return -1;}
static int wait_for_pll_enable_lock(struct clk_alpha_pll *p){(void)p;return R[0x38/4]!=1;}
'''
constants = ['PLL_OUTCTRL', 'PLL_RESET_N', 'PLL_VOTE_FSM_ENA', 'PLL_UPDATE_BYPASS',
             'PLL_OUT_MASK', 'PLL_OPMODE_RUN', 'PLL_OPMODE_STANDBY', 'LUCID_PLL_CAL_VAL']
for name in constants:
    m = re.search(r'^\s*#\s*define\s+'+name+r'\s+([^\n]+)', text, re.M)
    assert m, name
    source += f'#define {name} {m.group(1)}\n'
block = text.split('[CLK_ALPHA_PLL_TYPE_LUCID] =', 1)[1].split('},', 1)[0]
registers = dict(re.findall(r'\[PLL_OFF_(\w+)\]\s*=\s*(0x[0-9a-fA-F]+)', block))
source += '#define PLL_MODE(p) ((p)->offset)\n'
for name in sorted(set(re.findall(r'\b(PLL_[A-Z0-9_]+)\(pll\)', bodies))-{'PLL_MODE'}):
    key = name.removeprefix('PLL_')
    # *_VAL spelling is shared by register table and accessor macros.
    assert key in registers, key
    source += f'#define {name}(p) ((p)->offset+{registers[key]})\n'
source += bodies
config = re.search(r'static const struct alpha_pll_config disp_cc_pll1_config = \{.*?\n\};', dispcc.read_text(), re.S)
assert config
source += config.group(0)+'\n'
source += '''void AndroidLucidReference(void) {
 struct regmap map={0};struct clk_alpha_pll pll={.clkr={&map}};struct clk_hw hw={&pll};
 for(unsigned i=0;i<sizeof(R)/sizeof(R[0]);i++)R[i]=0;
 R[0x0c/4]=0x20000;
 clk_lucid_pll_configure(&pll,&map,&disp_cc_pll1_config);
 assert(alpha_pll_lucid_enable(&hw)==0);alpha_pll_lucid_disable(&hw);
 assert(alpha_pll_lucid_enable(&hw)==0);alpha_pll_lucid_disable(&hw);
}
'''
oracle = out/'android-oracle.c'
oracle.write_text(source)
production = pkg/'Library/LucidPllCrLib/LucidPll.c'
test = pkg/'Tests/LucidPllTest.c'
inputs = {Path(__file__).resolve(), reference, dispcc, oracle, production, test}
frozen = {p: hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
for kernel in (False, True):
    mode = 'nt' if kernel else 'native'
    flags = ['clang', '-std=c11', '-g', '-O1', '-Wall', '-Wextra', '-Werror',
             '-Wno-sign-compare', '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
             '-I'+str(pkg/'Include')]
    if kernel:
        flags += ['-D_KERNEL_MODE', '-I'+str(pkg/'Tests/KernelMocks')]
    else:
        flags += ['-fshort-wchar', '-I'+str(root/'MU_BASECORE/MdePkg/Include'),
                  '-I'+str(root/'MU_BASECORE/MdePkg/Include/X64')]
    objects = []
    for i, src in enumerate([test, oracle, production]):
        obj, dep = out/f'{mode}-{i}.o', out/f'{mode}-{i}.d'
        run(flags+['-MM', '-MF', dep, src])
        for p in [Path(p) for p in dep.read_text().split(':', 1)[1].replace('\\\n', '').split()]:
            digest = hashlib.sha256(p.read_bytes()).hexdigest()
            assert p not in frozen or frozen[p]==digest, f'Input changed before compilation: {p}'
            frozen[p] = digest
            inputs.add(p)
        run(flags+['-c', src, '-o', obj])
        objects.append(obj)
    exe = out/mode
    run(['clang', '-fsanitize=address,undefined']+objects+['-o', exe])
    result = run([exe], timeout=30)
    (out/f'{mode}.log').write_text(result.stdout+result.stderr)
    print(mode+': '+result.stdout.strip())
flags = ['clang', '--target=aarch64-none-elf', '-std=c11', '-O2', '-g', '-ffreestanding',
         '-fshort-wchar', '-Wall', '-Wextra', '-Werror', '-I'+str(pkg/'Include'),
         '-I'+str(root/'MU_BASECORE/MdePkg/Include'), '-I'+str(root/'MU_BASECORE/MdePkg/Include/AArch64')]
dep = out/'arm64.d'
run(flags+['-MM', '-MF', dep, production])
for p in [Path(p) for p in dep.read_text().split(':', 1)[1].replace('\\\n', '').split()]:
    digest = hashlib.sha256(p.read_bytes()).hexdigest()
    assert p not in frozen or frozen[p]==digest, f'Input changed before ARM64 compilation: {p}'
    frozen[p] = digest
    inputs.add(p)
run(flags+['-c', production, '-o', out/'LucidPll.aarch64.o'])
for p, digest in frozen.items():
    assert hashlib.sha256(p.read_bytes()).hexdigest()==digest, f'Input changed during tests: {p}'
receipt = {'status': 'PASS', 'scope': __doc__, 'commands': commands,
           'inputs': {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(inputs)},
           'artifacts': {p.name: hashlib.sha256(p.read_bytes()).hexdigest()
                         for p in sorted(out.iterdir()) if p.is_file() and p.name!='artifacts.json'}}
(out/'artifacts.json').write_text(json.dumps(receipt, indent=2)+'\n')
