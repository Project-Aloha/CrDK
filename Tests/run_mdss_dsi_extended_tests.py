#!/usr/bin/env python3
"""New portable DSI/PHY actual sources + untouched Linux/Android source oracle."""
from pathlib import Path
import argparse,hashlib,json,re,subprocess
ap=argparse.ArgumentParser();ap.add_argument('--output',type=Path,required=True);a=ap.parse_args()
root=Path(__file__).resolve().parents[3];pkg=root/'Platforms/CranePkg';out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
linux=root/'.vscode/linux/drivers/gpu/drm/msm/dsi/phy/dsi_phy.c'
android=root/'.vscode/android_kernel_modules_and_devicetree_oneplus_sm8450/vendor/qcom/opensource/display-drivers/msm/dsi'
pll=android/'dsi_pll_5nm.c';tables=android/'dsi_pll_5nm.h'
intf=root/'.vscode/linux/drivers/gpu/drm/msm/disp/dpu1/dpu_hw_intf.c'
top=root/'.vscode/linux/drivers/gpu/drm/msm/disp/dpu1/dpu_hw_top.c'
def function(s,name):
 m=re.search(r'(?:static\s+)?(?:inline\s+)?(?:int|s32|void)\s+'+name+r'\s*\(',s)
 start=m.start();b=s.index('{',m.end());depth=1;e=b+1
 while depth:
  depth+=(s[e]=='{')-(s[e]=='}');e+=1
 return s[start:e]+'\n'
pre=r'''/* GPL-2.0-only independent source oracle; test-only generated file. */
#include <stdbool.h>
#include <string.h>
#undef NULL
#include <Library/MdssDsiCrLib.h>
typedef INT32 s32;typedef UINT32 u32;typedef UINT64 u64;
#define max_t(t,a,b) ((t)(a)>(t)(b)?(t)(a):(t)(b))
#define min_t(t,a,b) ((t)(a)<(t)(b)?(t)(a):(t)(b))
#define S_DIV_ROUND_UP(n,d) ((n)>=0?((n)+(d)-1)/(d):((n)-(d)+1)/(d))
#define DIV_ROUND_UP(n,d) (((n)+(d)-1)/(d))
#define mult_frac(v,n,d) ((u64)(v)*(n)/(d))
#define NSEC_PER_MSEC 1000000UL
#define EINVAL 22
#define DBG(...) ((void)0)
#ifndef ARRAY_SIZE
#define ARRAY_SIZE(x) (sizeof(x)/sizeof((x)[0]))
#endif
#define do_div(n,d) ((n)/=(d))
#define div_u64(n,d) ((n)/(d))
#define DSI_PHY_TYPE_DPHY 0
#define DSI_PLL_DBG(...) ((void)0)
struct msm_dsi_phy_clk_request {unsigned long bitclk_rate,escclk_rate;};
struct msm_dsi_dphy_timing {
 s32 clk_prepare,clk_zero,clk_trail,hs_prepare,hs_zero,hs_trail,hs_rqst,hs_exit;
 struct {s32 clk_post,clk_pre;bool byte_intf_clk_div_2;} shared_timings;
};
struct dsi_pll_div_table {u64 min_hz,max_hz;u32 pll_div,phy_div;};
struct dsi_pll_resource {u32 type,bpp,lanes,pll_div,phy_div;u64 byteclk_rate,vco_rate,pclk_rate;};
static void dsi_pll_set_pll_post_div(struct dsi_pll_resource *p,u32 v){p->pll_div=v;}
static void dsi_pll_set_phy_post_div(struct dsi_pll_resource *p,u32 v){p->phy_div=v;}
static u32 dsi_pll_get_phy_post_div(struct dsi_pll_resource *p){return p->phy_div;}
'''
ls=linux.read_text();ps=pll.read_text();ts=tables.read_text()
body=function(ls,'linear_inter')+function(ls,'msm_dsi_dphy_timing_calc_v4')+function(ls,'msm_dsi_cphy_timing_calc_v4')
body+=''.join(re.findall(r'struct dsi_pll_div_table pll_5nm_[dc]phy_[lh]b\[\] = \{.*?\n\};',ts,re.S))+'\n'
body+=function(ps,'dsi_pll_5nm_set_byteclk_div')+function(ps,'dsi_pll_calc_dphy_pclk_div')+function(ps,'dsi_pll_calc_cphy_pclk_div')
wrap=r'''
void ReferenceTiming(UINT64 Rate,BOOLEAN Cphy,UINT32 *T){
 struct msm_dsi_dphy_timing t={0};struct msm_dsi_phy_clk_request c={Rate,19200000};
 if(Cphy)msm_dsi_cphy_timing_calc_v4(&t,&c);else msm_dsi_dphy_timing_calc_v4(&t,&c);
 memset(T,0,14*sizeof(*T));T[4]=t.hs_exit;T[8]=t.hs_rqst;T[9]=2;T[10]=4;
 if(Cphy){T[5]=t.shared_timings.clk_pre;T[6]=t.clk_prepare;T[7]=t.shared_timings.clk_post;}
 else{T[1]=t.clk_zero;T[2]=t.clk_prepare;T[3]=t.clk_trail;T[5]=t.hs_zero;
  T[6]=t.hs_prepare;T[7]=t.hs_trail;T[12]=t.shared_timings.clk_pre;T[13]=t.shared_timings.clk_post;}
}
void ReferenceClock(UINT64 Byte,BOOLEAN Cphy,UINT32 Lanes,UINT64 *Vco,
 UINT32 *Pll,UINT32 *Phy,UINT32 *Div,UINT64 *Source){
 struct dsi_pll_resource p={.type=Cphy,.bpp=24,.lanes=Lanes,.byteclk_rate=Byte};
 dsi_pll_5nm_set_byteclk_div(&p,true);*Vco=p.vco_rate;*Pll=p.pll_div;*Phy=p.phy_div;
 *Div=Cphy?dsi_pll_calc_cphy_pclk_div(&p):dsi_pll_calc_dphy_pclk_div(&p);
 *Source=Cphy?p.vco_rate/p.pll_div*2/7/ *Div:p.vco_rate/p.pll_div/p.phy_div/2/ *Div;
}
'''
dpu=r'''
#undef BIT
#define BIT(n) (1U<<(n))
#define INTF_DP 1
#define INTF_DSI 2
#define INTF_MODE_CMD 1
#define INTF_1 2
#define INTF_2 3
#define BPC8 3
#define MSM_FORMAT_IS_YUV(f) 0
struct dpu_hw_blk_reg_map {u32 *r;};
#define DPU_REG_READ(c,o) ((c)->r[(o)/4])
#define DPU_REG_WRITE(c,o,v) ((c)->r[(o)/4]=(v))
struct version {u32 core_major_ver;};struct capability {u32 type;};
struct dpu_hw_intf {struct dpu_hw_blk_reg_map hw;struct capability *cap;struct version *mdss_ver;};
struct msm_format {u32 bpc_g_y,bpc_b_cb,bpc_r_cr;};
struct dpu_hw_intf_timing_params {
 u32 hsync_pulse_width,h_back_porch,width,h_front_porch,vsync_pulse_width,v_back_porch,height,v_front_porch;
 u32 xres,yres,hsync_skew,vsync_polarity,hsync_polarity,dce_bytes_per_line,border_clr,underflow_clr;
 bool wide_bus_en,compression_en;
};
struct dpu_hw_mdp {struct dpu_hw_blk_reg_map hw;};
struct split_pipe_cfg {u32 en,mode,intf,split_flush_en;};
'''
it=intf.read_text();tt=top.read_text()
dpu+='\n'.join(l for l in it.splitlines() if l.startswith('#define INTF_'))+'\n'
dpu+='\n'.join(l for l in tt.splitlines() if l.startswith('#define FLD_'))+'\n'
dpu+='#define SSPP_SPARE 0x28\n#define SPLIT_DISPLAY_EN 0x2f4\n#define SPLIT_DISPLAY_LOWER_PIPE_CTRL 0x3f0\n#define SPLIT_DISPLAY_UPPER_PIPE_CTRL 0x2f8\n'
dpu+=function(it,'dpu_hw_intf_setup_timing_engine')+function(tt,'dpu_hw_setup_split_pipe')
dpu+=r'''
void ReferenceDpuTiming(const CR_MDSS_DSI_PLAN *P,u32 Index,u32 *Registers){
 const CR_MDSS_MODE *M=&P->Link[Index];struct capability c={INTF_DSI};struct version v={8};
 struct dpu_hw_intf i={{Registers},&c,&v};struct msm_format f={3,3,3};
 struct dpu_hw_intf_timing_params p={.hsync_pulse_width=M->HSync,.h_back_porch=M->HBackPorch,
  .width=P->PayloadWidth,.h_front_porch=M->HFrontPorch,.vsync_pulse_width=M->VSync,
  .v_back_porch=M->VBackPorch,.height=M->Height,.v_front_porch=M->VFrontPorch,
  .xres=P->PayloadWidth,.yres=M->Height,.border_clr=M->BorderColor,.underflow_clr=255,
  .compression_en=P->Mode.Dsc};
 if(P->Mode.Dsc)p.dce_bytes_per_line=P->Mode.Compression.ChunkBytes*
  (P->Mode.Compression.SliceCount/P->Mode.Controllers);
 dpu_hw_intf_setup_timing_engine(&i,&p,&f);
}
void ReferenceDpuSplit(bool Dual,u32 *Registers){struct dpu_hw_mdp m={{Registers}};
 struct split_pipe_cfg s={.en=Dual,.mode=0,.intf=INTF_1,.split_flush_en=Dual};
 dpu_hw_setup_split_pipe(&m,&s);
}
'''
oracle=out/'reference.c';oracle.write_text(pre+body+wrap+dpu)
inputs=[pkg/'Tests/MdssDsiExtendedTest.c',pkg/'Library/MdssDsiCrLib/MdssDsi.c',
 pkg/'Library/MdssDsiCrLib/DsiPhyExtended.c',pkg/'Library/MdssDsiCrLib/MdssDsiInternal.h',
 pkg/'Library/MdssCrLib/Mdss.c',pkg/'Library/MdssCrLib/MdssHardware.c',pkg/'Library/MdssDscCrLib/MdssDsc.c',
 pkg/'Include/Library/MdssDsiCrLib.h',pkg/'Include/Library/MdssDscCrLib.h',linux,pll,tables,
 pkg/'Library/MdssDsiCrLib/MdssDsiCommands.c',pkg/'Library/MdssDualCrLib/MdssDual.c',
 pkg/'Include/Library/MdssDualCrLib.h',intf,top]
inputs += [pkg/'Library/MdssDsiCrLib/MdssDsiClock.c',pkg/'Library/ClockExternalLib/ClockExternalPlan.c',pkg/'Include/Library/ClockExternalPlan.h',pkg/'Include/Library/ClockExternalLib.h']
results=[]
for kernel in (False,True):
 flags=['clang','-std=c11','-g','-O1','-Wall','-Wextra','-Werror','-Wno-sign-compare','-Wno-unused-parameter',
  '-fsanitize=address,undefined','-fno-sanitize-recover=all','-ffunction-sections','-Wl,--gc-sections','-I'+str(pkg/'Include')]
 if kernel:flags+=['-D_KERNEL_MODE','-I'+str(pkg/'Tests/KernelMocks')]
 else:flags+=['-fshort-wchar','-I'+str(root/'MU_BASECORE/MdePkg/Include'),'-I'+str(root/'MU_BASECORE/MdePkg/Include/X64')]
 exe=out/('nt' if kernel else 'native');src=[inputs[i] for i in (0,1,2,4,5,6,12,13)]+[pkg/'Library/MdssDsiCrLib/MdssDsiClock.c',pkg/'Library/ClockExternalLib/ClockExternalPlan.c',oracle]
 cmd=flags+[str(p) for p in src]+['-o',str(exe)]
 r=subprocess.run(cmd,text=True,capture_output=True);(out/(exe.name+'-compile.log')).write_text(r.stdout+r.stderr)
 if r.returncode:print(r.stderr);raise SystemExit(r.returncode)
 r=subprocess.run([str(exe)],text=True,capture_output=True,timeout=60);(out/(exe.name+'.log')).write_text(r.stdout+r.stderr)
 print(r.stdout+r.stderr,end='');r.check_returncode();results.append(exe.name)
(out/'artifacts.json').write_text(json.dumps({'status':'PASS','results':results,
 'inputs':{str(p.relative_to(root)):hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}},indent=2)+'\n')
