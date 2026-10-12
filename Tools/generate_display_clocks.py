#!/usr/bin/env python3
"""Curate selected SM8250/SM8350 display facts; ClockLib owns all algorithms. MIT."""
import argparse,hashlib,re,subprocess
from pathlib import Path
root=Path(__file__).resolve().parents[3]
a=argparse.ArgumentParser()
a.add_argument('--kona-source',type=Path,required=True)
a.add_argument('--kona-gcc',type=Path,required=True)
a.add_argument('--kona-dt',type=Path,required=True)
a.add_argument('--lahaina-kernel',type=Path,default=root/'.vscode/android_kernel_oneplus_sm8350')
a.add_argument('--check',action='store_true');args=a.parse_args()
rev='13ca08fdf0979fdd61d5e8991661874bb2d19150'
def vendor(path):return subprocess.check_output(['git','-C',str(args.lahaina_kernel),'show',rev+':'+path],text=True)
def blocks(text):return {n:(k,b) for k,n,b in re.findall(r'^static (?:const )?struct (\w+) \*?(\w+)(?:\[\])? = \{(.*?)^\};',text,re.M|re.S)}
def field(body,key,default=None):
    m=re.search(r'\.'+re.escape(key)+r'\s*=\s*([^,\n]+)',body)
    if m:return m[1].strip()
    assert default is not None,key
    return default
def number(body,key,default=None):return int(field(body,key,default),0)
def sha(text):return hashlib.sha256(text.encode()).hexdigest()
for soc,source,gcc,dt,revision in [
 ('8250',args.kona_source.read_text(),args.kona_gcc.read_text(),args.kona_dt.read_text(),'cad09e061ef6cf689a4a1e54d27562e0f042236a'),
 ('8350',vendor('drivers/clk/qcom/dispcc-lahaina.c'),vendor('drivers/clk/qcom/gcc-lahaina.c'),vendor('arch/arm64/boot/dts/vendor/qcom/lahaina.dtsi'),rev)]:
    b=blocks(source);g=blocks(gcc)
    assert re.search(r'reg\s*=\s*<0xaf03000 0x4>',dt)
    assert b['disp_cc_pll1'][0]=='clk_alpha_pll' and number(b['disp_cc_pll1'][1],'offset')==0x1000
    cfg=b['disp_cc_pll1_config'][1]
    assert number(cfg,'l')==31 and number(cfg,'alpha')==0x4000
    out=['/* Generated selected SM'+soc+' DISPCC facts; SPDX-License-Identifier: MIT',
         ' * OnePlus Android '+revision,
         ' * DISPCC SHA256 '+sha(source),' * GCC SHA256 '+sha(gcc),' * DT SHA256 '+sha(dt),
         ' * Register/rate facts only. Direct PLL qualification is shared ClockLib;',
         ' * no cold analogue startup is granted by this inherited profile. */']
    for kind in ('ahb','xo','hf_axi','sf_axi'):
        name='gcc_disp_'+kind+'_clk';body=g[name][1]
        assert g[name][0]=='clk_branch' and field(body,'ops')=='&clk_branch2_ops'
        halt={'BRANCH_HALT':'CLOCK_NODE_BRANCH_HALT','BRANCH_HALT_DELAY':'CLOCK_NODE_BRANCH_HALT_DELAY',
              'BRANCH_HALT_SKIP':'CLOCK_NODE_BRANCH_HALT_SKIP','BRANCH_HALT_VOTED':'CLOCK_NODE_BRANCH_HALT | CLOCK_NODE_BRANCH_VOTED'}[field(body,'halt_check')]
        out += ['STATIC ClockNode '+name+' = {',f'  .Name="{name}", .Type=CLOCK_NODE_TYPE_BRANCH_2, .ParentController=&mGcc,',
                f'  .EnableRegister={number(body,"enable_reg"):#x}, .HaltRegister={number(body,"halt_reg"):#x}, .EnableMsk=BIT(0),',
                f'  .HaltCheckFlag={halt}, .KeepEnabled={"TRUE" if kind in ("ahb","xo") else "FALSE"},',
                f'  .HwcgRegister={number(body,"hwcg_reg","0"):#x}, .HwcgMsk={"BIT("+field(body,"hwcg_bit")+")" if ".hwcg_reg" in body else "0"},','};']
    out += ['STATIC ClockNode disp_cc_pll1 = {',
            '  .Name="disp_cc_pll1", .Type=CLOCK_NODE_TYPE_READ_ONLY_SOURCE, .ParentController=&mDisp,',
            '  .FixedRateHz=600000000, .ReadOnlySourceCheck=CheckDisplayPll,','};']
    roots=['ahb','byte0','byte1','esc0','esc1','mdp','pclk0','pclk1','vsync']
    for kind in roots:
        name='disp_cc_mdss_'+kind+'_clk_src';body=b[name][1]
        assert b[name][0]=='clk_rcg2' and number(body,'hid_width')==5
        external=kind.startswith(('byte','pclk'))
        table=b[field(body,'freq_tbl')][1] if not external else ''
        freq=200000000 if kind=='mdp' else 19200000
        parent='P_DISP_CC_PLL1_OUT_MAIN' if kind=='mdp' else 'P_BI_TCXO'
        divider=3 if kind=='mdp' else 1
        if not external:assert re.search(r'F\('+str(freq)+r', '+parent+r', '+str(divider)+r', 0, 0\)',table),name
        mapping=dict(re.findall(r'\{ (\w+), (\d+) \}',b[field(body,'parent_map')][1]))
        selector=int(mapping[parent])
        if not external:out += [f'STATIC ClockRcgFreqTable {name}_rates[] = {{',
                f'  {{.Source={{{selector},{selector}}}, .PreDiv={divider*2-1}, .FrequencyHz={freq}}},','};']
        out += [f'STATIC ClockNode {name} = {{',f'  .Name="{name}", .Type=CLOCK_NODE_TYPE_ROOT_CLOCK_GENERATOR_2, .ParentController=&mDisp,',
                f'  .CmdRegister={number(body,"cmd_rcgr"):#x}, .MNDWidth={number(body,"mnd_width")}, .HIDWidth=5,',
                f'  .HwClockCtrl={"TRUE" if kind in ("ahb","mdp") else "FALSE"},']
        if not external:out += [f'  .FreqTable={name}_rates, .FreqCount=ARRAY_SIZE({name}_rates),']
        if kind=='mdp':out += ['  .FixedRateHz=200000000,']
        out += ['};']
    branches=['ahb','byte0','byte0_intf','byte1','byte1_intf','esc0','esc1','mdp','mdp_lut','pclk0','pclk1','vsync']
    for kind in branches:
        name='disp_cc_mdss_'+kind+'_clk';body=b[name][1]
        assert b[name][0]=='clk_branch' and field(body,'ops')=='&clk_branch2_ops'
        parentkind='mdp' if kind=='mdp_lut' else kind.replace('_intf','')
        parent='disp_cc_mdss_'+parentkind+'_clk_src';rate=200000000 if parentkind=='mdp' else 19200000
        # ClockLib branches own dependency order; RCG configuration validates
        # its already-owned fixed source and does not acquire parents itself.
        # Both MDP leaves hold PLL before source, so a rejected inherited PLL
        # cannot cause a speculative RCG write or a synthetic source lease.
        mdp=parentkind=='mdp'
        out += [f'STATIC ClockNode *{name}_parents[]={{'+('&disp_cc_pll1,' if mdp else '')+f'&{parent}'+'};']
        if not kind.startswith(('byte','pclk')):out += [f'STATIC CONST UINT64 {name}_parent_rates[]={{'+('600000000,' if mdp else '')+str(rate)+'};']
        out += [f'STATIC ClockNode {name} = {{',f'  .Name="{name}", .Type=CLOCK_NODE_TYPE_BRANCH_2, .ParentController=&mDisp,',
                f'  .EnableRegister={number(body,"enable_reg"):#x}, .HaltRegister={number(body,"halt_reg"):#x}, .EnableMsk=BIT(0),',
                f'  .HaltCheckFlag=CLOCK_NODE_BRANCH_HALT{" | CLOCK_NODE_BRANCH_VOTED" if "VOTED" in field(body,"halt_check") else ""},',
                f'  .ParentCount={2 if mdp else 1}, .Parents={name}_parents,']
        # External byte/pixel rates are provider-owned; zero-rate branch calls
        # acquire the actual RCG without an invented nominal PHY output.
        if not kind.startswith(('byte','pclk')):out += [f'  .ParentRates={name}_parent_rates,']
        if mdp:out += ['  .FixedRateHz=200000000,']
        if kind.endswith('_intf'):
            d=b['disp_cc_mdss_'+parentkind+'_div_clk_src'][1]
            assert number(d,'shift')==0
            out += [f'  .DividerRegister={number(d,"reg"):#x}, .DividerWidth={number(d,"width")},',
                    f'  .DividerReadOnly={"TRUE" if field(d,"ops")=="&clk_regmap_div_ro_ops" else "FALSE"},']
        out += ['};']
    out += ['STATIC ClockNode disp_cc_mdss_core_gdsc = {',
            '  .Name="disp_cc_mdss_core_gdsc", .Type=CLOCK_NODE_TYPE_GDSC, .ParentController=&mDisp,',
            '  .GdscRegister=0x3000, .PwrStsFlag=CLOCK_NODE_GDSC_PWR_STS_OFF_ON,',
            '  .GdscFlags=CLOCK_NODE_GDSC_FLAG_HW_CTL | CLOCK_NODE_GDSC_FLAG_RETAIN_FF_EN,','};',
            'STATIC ClockNode *mDisplayClocks[]={&disp_cc_pll1,']
    out += ['  &disp_cc_mdss_'+n+'_clk_src,' for n in roots]
    out += ['  &disp_cc_mdss_'+n+'_clk,' for n in branches]
    out += ['};','STATIC ClockNode *mDisplayGdscs[]={&disp_cc_mdss_core_gdsc};','']
    dest=root/'Platforms/CranePkg/Target'/('Sm'+soc)/'CrDisplayClockGenerated.h'
    result='\n'.join(out)
    if args.check:assert dest.read_text()==result,'display facts need regeneration'
    else:dest.write_text(result)
    print('SM'+soc+' display facts generated/checked')
