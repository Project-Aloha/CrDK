#!/usr/bin/env python3
"""Actual SM8250 command host/panel against pinned Android functions and XML."""
from pathlib import Path
import argparse, hashlib, json, re, subprocess
p=argparse.ArgumentParser();p.add_argument('--output',type=Path,required=True);a=p.parse_args()
root=Path(__file__).resolve().parents[3];pkg=root/'Platforms/CranePkg';out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
commit='cad09e061ef6cf689a4a1e54d27562e0f042236a'
repo=root/'.vscode/android_kernel_oneplus_sm8250'
sources={}
def source(name):
    path='techpack/display/msm/dsi/'+name
    b=subprocess.check_output(['git','-C',str(repo),'show',commit+':'+path]);sources[path]=hashlib.sha256(b).hexdigest()
    (out/name).write_bytes(b);return b.decode()
def function(s,name):
    m=re.search(r'(?:static\s+)?(?:void|int)\s+'+name+r'\s*\(',s);assert m,name
    start=m.start();i=s.index('{',m.end());depth=1;j=i+1
    while depth:depth+=(s[j]=='{')-(s[j]=='}');j+=1
    return s[start:j]+'\n'
host=source('dsi_ctrl_hw_cmn.c');regs=source('dsi_ctrl_reg.h');panel=source('dsi_panel.c')
pre=r'''/* Copyright (c) 2015-2020 The Linux Foundation. GPL-2.0-only.
 * Unchanged function bodies from pinned Android SM8250, test harness only. */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
typedef uint32_t u32;typedef uint8_t u8;
#define BIT(n) (1U<<(n))
#define DIV_ROUND_UP(n,d) (((n)+(d)-1)/(d))
#define DSI_CTRL_HW_DBG(...) ((void)0)
#define DSI_ERR(...) ((void)0)
#define wmb() ((void)0)
#define DSI_TRIGGER_MAX 6
#define DSI_PIXEL_FORMAT_MAX 7
#define DSI_TE_ON_EXT_PIN 1
#define DSI_DATA_LANE_0 1
#define DSI_DATA_LANE_1 2
#define DSI_DATA_LANE_2 4
#define DSI_DATA_LANE_3 8
#define DSI_CTRL_DMA_LINK_SEL (BIT(12)|BIT(13))
#define DSI_CTRL_MDP0_LINK_SEL (BIT(20)|BIT(22))
struct dsi_ctrl_hw {u32 *r;bool phy_isolation_enabled,null_insertion_enabled;};
#define DSI_R32(c,o) ((c)->r[(o)/4])
#define DSI_W32(c,o,v) ((c)->r[(o)/4]=(v))
struct dsi_host_common_cfg {u32 te_mode,dma_cmd_trigger,mdp_cmd_trigger,t_clk_post,t_clk_pre;
 bool append_tx_eot,ignore_rx_eot,en_crc_check,en_ecc_check,bit_swap_red,bit_swap_green,bit_swap_blue;
 u32 data_lanes,dst_format;struct {bool split_link_enabled;} split_link;};
struct dsi_cmd_engine_cfg {u32 max_cmd_packets_interleave,wr_mem_start,wr_mem_continue;bool insert_dcs_command;};
struct msm_display_dsc_info {int slice_width,slice_per_pkt,bpp,eol_byte_num,pclk_per_line,bytes_in_slice,bytes_per_pkt,pkt_per_line;};
struct dsi_mode_info {bool dsc_enabled;struct msm_display_dsc_info *dsc;u32 h_active,v_active;};
struct dsi_rect {u32 w,h;};
'''
body=regs+'\n'+re.search(r'static const u8 cmd_mode_format_map.*?};',host,re.S)[0]+'\n'
body+=function(panel,'dsi_dsc_pclk_param_calc')
for name in ['dsi_split_link_setup','dsi_setup_trigger_controls','dsi_ctrl_hw_cmn_host_setup','dsi_ctrl_hw_cmn_cmd_engine_setup','dsi_ctrl_hw_cmn_setup_cmd_stream']:body+=function(host,name)
body+=r'''
void ReferenceHost(u32 *r){struct dsi_ctrl_hw h={.r=r};
 struct dsi_host_common_cfg c={.te_mode=1,.dma_cmd_trigger=3,.mdp_cmd_trigger=0,.t_clk_pre=16,.t_clk_post=20,.data_lanes=15,.dst_format=3};
 struct dsi_cmd_engine_cfg e={.wr_mem_start=0x2c,.wr_mem_continue=0x3c,.insert_dcs_command=true};
 struct msm_display_dsc_info d={.slice_width=540,.slice_per_pkt=1,.bpp=8};
 struct dsi_mode_info m={true,&d,1080,2340};
 dsi_ctrl_hw_cmn_host_setup(&h,&c);dsi_ctrl_hw_cmn_cmd_engine_setup(&h,&c,&e);
 dsi_ctrl_hw_cmn_setup_cmd_stream(&h,&m,3240,0,0);r[1]|=5;
}
'''
oracle=out/'reference.c';oracle.write_text(pre+body)
xml=pkg/'Tests/Fixtures/Hdk8245Panel/Panel_r66451_fhd_plus_60hz_cmd.xml';xb=xml.read_bytes()
assert hashlib.sha256(xb).hexdigest()=='8e00f827aa9234a4936ee0893866e4e37e54312f8c0b93d3c8d706ad1a7d2118'
fixture='/* Generated independently from immutable stock XML. */\n'
for tag,name in [('DSIInitSequence','ExpectedOn'),('DSITermSequence','ExpectedOff')]:
    seq=[]
    for line in re.search('<'+tag+'>(.*?)</'+tag+'>',xb.decode(),re.S)[1].strip().splitlines():
        b=bytes.fromhex(line)
        if b[0]==255:seq[-1][2]=b[1]*1000
        else:seq.append([b[0],b[1:],0])
    fixture+='static const EXPECTED '+name+'[]={\n'
    for t,b,d in seq:fixture+='{%d,%d,%d,{%s}},\n'%(t,len(b),d,','.join(map(str,b)))
    fixture+='};\n'
(out/'packets.h').write_text(fixture)
inputs=[pkg/'Tests/MdssCmdHostTest.c']+[pkg/'Library/MdssDsiCmdCrLib'/n for n in ['Plan.c','Io.c','Host.c','PanelR66451.c','Panel.c']]+[pkg/'Library/MdssDscCrLib/MdssDsc.c']
results=[]
for kernel in (False,True):
    flags=['clang','-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-Wno-sign-compare','-Wno-unused-parameter','-fsanitize=address,undefined','-fno-sanitize-recover=all','-ffunction-sections','-Wl,--gc-sections','-I'+str(pkg/'Include'),'-I'+str(out)]
    flags+=['-D_KERNEL_MODE','-I'+str(pkg/'Tests/KernelMocks')] if kernel else ['-fshort-wchar','-I'+str(root/'MU_BASECORE/MdePkg/Include'),'-I'+str(root/'MU_BASECORE/MdePkg/Include/X64')]
    exe=out/('nt' if kernel else 'native')
    r=subprocess.run(flags+list(map(str,inputs))+[str(oracle),'-o',str(exe)],text=True,capture_output=True)
    (out/(exe.name+'-compile.log')).write_text(r.stdout+r.stderr)
    if r.returncode:print(r.stderr);r.check_returncode()
    r=subprocess.run([str(exe)],text=True,capture_output=True,timeout=60);(out/(exe.name+'.log')).write_text(r.stdout+r.stderr)
    print(r.stdout+r.stderr,end='');r.check_returncode();results.append(exe.name)
inputs+=[pkg/'Include/Library/MdssDsiCmdCrLib.h',pkg/'Library/MdssDsiCmdCrLib/Internal.h',xml,Path(__file__)]
(out/'artifacts.json').write_text(json.dumps({'status':'PASS','variants':results,'android_commit':commit,'sources':sources,'inputs':{str(i.relative_to(root)):hashlib.sha256(i.read_bytes()).hexdigest() for i in inputs}},indent=2)+'\n')
