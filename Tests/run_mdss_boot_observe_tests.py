#!/usr/bin/env python3
"""Read-only SM8250 boot-route observer versus original Android/FDT oracles."""
from pathlib import Path
import argparse
import hashlib
import json
import re
import subprocess

ap=argparse.ArgumentParser(description=__doc__)
ap.add_argument('--output',type=Path,required=True)
ap.add_argument('--generate-only',action='store_true')
a=ap.parse_args();root=Path(__file__).resolve().parents[3];pkg=root/'Platforms/CranePkg'
base=root/'.tmp/waipio-continuation/hdk8245-panel/boot-observe';src=base/'sources'
out=a.output.resolve();out.mkdir(parents=True,exist_ok=True);commands=[]
manifest_path=base/'source-manifest.json'
manifest=json.loads(manifest_path.read_text())
for name,expected in manifest['files'].items():
    assert hashlib.sha256((src/name).read_bytes()).hexdigest()==expected,name
assert hashlib.sha256((root/'hdk8245_fdt').read_bytes()).hexdigest()=='2743c213f1c286030eb65647c693cf0e2dfa69ee2a84ceac0e17a86d82de3070'

def run(args,**kw):
    commands.append([str(x) for x in args]);r=subprocess.run(args,text=True,capture_output=True,**kw)
    if r.returncode:print(r.stdout+r.stderr);r.check_returncode()
    return r
def block(text,start):
    p=text.index('{',start);end=p+1;depth=1
    while depth:
        depth+=(text[end]=='{')-(text[end]=='}');end+=1
    return text[start:end]
def function(text,name):
    m=re.search(r'^static (?:inline )?u32 '+name+r'\(',text,re.M);assert m,name
    return block(text,m.start())+'\n'
ctl=(src/'sde_hw_ctl.c').read_text();mdss=(src/'sde_hw_mdss.h').read_text()
def enum(name):
    return block(mdss,mdss.index('enum '+name+' {'))+';\n'
def define(text,name):
    lines=text.splitlines(); pattern=r'^\s*#define\s+'+re.escape(name)+r'(?=\s|\()'
    for i,line in enumerate(lines):
        if re.search(pattern,line):
            result=[line]
            while result[-1].rstrip().endswith('\\'):
                i+=1;result.append(lines[i])
            return '\n'.join(result)+'\n'
    raise AssertionError(name)
def fdt(prop,node='/soc/qcom,mdss_mdp@ae00000'):
    return [int(x,16) for x in run(['fdtget','-t','x',root/'hdk8245_fdt',node,prop]).stdout.split()]
ctls=[v-0x1000 for v in fdt('qcom,sde-ctl-off')]
sspp=[v-0x1000 for v in fdt('qcom,sde-sspp-off')]
intfs=[v-0x1000 for v in fdt('qcom,sde-intf-off')]
pps=[v-0x1000 for v in fdt('qcom,sde-pp-off')]
dscs=[v-0x1000 for v in fdt('qcom,sde-dsc-off')]
lms=[v-0x1000 for v in fdt('qcom,sde-mixer-off')]
reserve=fdt('reg','/reserved-memory/cont_splash_region@9c000000')
irq_file=base/'irq-audit/irq-inventory.json'
assert hashlib.sha256(irq_file.read_bytes()).hexdigest()=='628583b3248d45578ba6af4f12d095f66e7f132a5b8f50d5be356ca7ad89e922'
irq=json.loads(irq_file.read_text())
banks=sorted((b for b in irq['banks'] if b.get('normalized_index') is not None),key=lambda b:b['normalized_index'])
oracle='''/* Original Qualcomm Android GPL-2.0-only CTL decoder; test ABI shim.
 * Copyright (c) 2015-2019, The Linux Foundation. All rights reserved. */
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#ifndef _KERNEL_MODE
#undef NULL
#endif
#include <Library/MdssBoot8250CrLib.h>
typedef uint32_t u32;
#define BIT(n) (1U<<(n))
#define CTL_NUM_EXT 4
#define CTL_SSPP_MAX_RECTS 2
#define pr_err(...) ((void)0)
'''
oracle+=enum('sde_sspp')+enum('sde_lm')
for n in ['CTL_LAYER','CTL_LAYER_EXT','CTL_LAYER_EXT2','CTL_LAYER_EXT3','CTL_INTF_ACTIVE']:
    oracle+=define(ctl,n)
start=ctl.index('struct ctl_sspp_stage_reg_map {');oracle+=block(ctl,start)+';\n'
start=ctl.index('static const struct ctl_sspp_stage_reg_map\nsspp_reg_cfg_tbl')
oracle+=block(ctl,start)+';\n'
oracle+='''struct sde_hw_blk_reg_map {u32 Words[4],Intf;};
struct sde_hw_ctl {struct sde_hw_blk_reg_map hw;};
struct sde_sspp_index_info {enum sde_sspp sspp;bool is_virtual;};
static u32 ReferenceRead(struct sde_hw_blk_reg_map *c,u32 off) {
 if(off==CTL_INTF_ACTIVE)return c->Intf;
 const u32 offsets[]={CTL_LAYER(LM_0),CTL_LAYER_EXT(LM_0),CTL_LAYER_EXT2(LM_0),CTL_LAYER_EXT3(LM_0)};
 for(unsigned i=0;i<4;i++)if(off==offsets[i])return c->Words[i];assert(0);return 0;
}
#define SDE_REG_READ(c,o) ReferenceRead(c,o)
'''
oracle+=function(ctl,'sde_hw_ctl_get_staged_sspp')+function(ctl,'sde_hw_ctl_get_intf_v1')
oracle+='''u32 BootReferenceDecode(const u32 Words[4],u32 *Pipe,u32 *Virtual) {
 struct sde_hw_ctl c={0};struct sde_sspp_index_info info[32]={0};memcpy(c.hw.Words,Words,16);
 u32 n=sde_hw_ctl_get_staged_sspp(&c,LM_0,info,32);
 for(u32 i=0;i<n;i++){Pipe[i]=info[i].sspp;Virtual[i]=info[i].is_virtual;}return n;
}
u32 BootReferenceStage(u32 Pipe,u32 Rect,u32 Stage,u32 Words[4]) {
 assert(Pipe>0&&Pipe<SSPP_MAX&&Rect<2);memset(Words,0,16);
 const struct ctl_sspp_stage_reg_map *m=&sspp_reg_cfg_tbl[Pipe][Rect];
 if(!m->bits)return 0;
 Words[m->ext]=(Stage&((1U<<m->bits)-1))<<m->start;
 if(m->sec_bit_mask&&(Stage&8))Words[1]|=m->sec_bit_mask;return 1;
}
u32 BootReferenceIntf(u32 Value){struct sde_hw_ctl c={.hw={.Intf=Value}};return sde_hw_ctl_get_intf_v1(&c);}
'''
for filename,names in {
 'sde_hw_sspp.c':['SSPP_SRC_SIZE','SSPP_SRC_XY','SSPP_OUT_SIZE','SSPP_OUT_XY','SSPP_SRC0_ADDR','SSPP_SRC_YSTRIDE0','SSPP_SRC_FORMAT','SSPP_SRC_UNPACK_PATTERN','SSPP_SRC_OP_MODE','SSPP_SW_PIX_EXT_C0_REQ_PIXELS','SSPP_SW_PIX_EXT_C1C2_REQ_PIXELS','SSPP_SW_PIX_EXT_C3_REQ_PIXELS','MDSS_MDP_OP_PE_OVERRIDE'],
 'sde_hw_intf.c':['INTF_TEAR_TEAR_CHECK_EN','INTF_MUX','INTF_TEAR_START_POS','INTF_TEAR_SYNC_WRCOUNT','INTF_TEAR_SYNC_CONFIG_VSYNC'],
 'sde_hw_pingpong.c':['PP_DSC_MODE'],
 'sde_hw_dsc.c':['DSC_COMMON_MODE','DSC_PICTURE','DSC_SLICE','DSC_CTL'],
 'dsi_ctrl_reg.h':['DSI_CTRL']}.items():
    data=(src/filename).read_text()
    for name in names:oracle+=define(data,name)
oracle+='enum {DSC_NONE=0,DSC_MAX=5};\n'
oracle+='const UINT32 BootReferenceIrqOffsets[12][3]={\n'+''.join(
    '{'+','.join(b[k] for k in ['en_dpu_offset','status_dpu_offset','clr_dpu_offset'])+'},\n' for b in banks)+'};\n'
for name,values in [('Ctl',ctls),('Sspp',sspp),('Intf',intfs),('Pp',pps),('Dsc',dscs),('Lm',lms)]:
    oracle+='static const u32 '+name+'[]={'+','.join(hex(x) for x in values)+'};\n'
oracle+=f'#define FRAME_PHYSICAL {hex(reserve[1])}U\n'
oracle+='''void BootReferenceSeed(UINT32 SourceIndex,void (*SetReg)(UINT32,UINT32,UINT32),CR_MDSS_BOOT_DMA_OBSERVATION *Dma) {
 assert(SourceIndex==0||SourceIndex==4);u32 w[4];
 SetReg(CrMdssBootTop,0,0x60000000);
 const u32 layeroff[]={CTL_LAYER(LM_0),CTL_LAYER_EXT(LM_0),CTL_LAYER_EXT2(LM_0),CTL_LAYER_EXT3(LM_0)};
 assert(BootReferenceStage(SourceIndex==0?SSPP_VIG0:SSPP_DMA0,0,1,w));
 for(u32 i=0;i<4;i++)SetReg(CrMdssBootDpu,Ctl[0]+layeroff[i],w[i]);
 SetReg(CrMdssBootDpu,Ctl[0]+0x14,BIT(17));
 SetReg(CrMdssBootDpu,Ctl[0]+CTL_INTF_ACTIVE,BIT(1));
 SetReg(CrMdssBootDpu,Ctl[0]+0xe8,BIT(0));
 u32 b=Sspp[SourceIndex],size=(2340U<<16)|1080U;
 SetReg(CrMdssBootDpu,b+SSPP_SRC_SIZE,size);SetReg(CrMdssBootDpu,b+SSPP_OUT_SIZE,size);
 SetReg(CrMdssBootDpu,b+SSPP_SRC0_ADDR,FRAME_PHYSICAL);
 SetReg(CrMdssBootDpu,b+SSPP_SRC_YSTRIDE0,1080*4);
 /* Source setup_format formula: four 8-bit components, 4B interleaved tight unpack. */
 SetReg(CrMdssBootDpu,b+SSPP_SRC_FORMAT,(3U<<6)|(3U<<4)|(3U<<2)|3U|(3U<<12)|BIT(17)|(3U<<9));
 SetReg(CrMdssBootDpu,b+SSPP_SRC_UNPACK_PATTERN,(3U<<24)|(2U<<16)|(0U<<8)|1U);
 SetReg(CrMdssBootDpu,b+SSPP_SRC_OP_MODE,MDSS_MDP_OP_PE_OVERRIDE);
 SetReg(CrMdssBootDpu,b+SSPP_SW_PIX_EXT_C0_REQ_PIXELS,size);
 SetReg(CrMdssBootDpu,b+SSPP_SW_PIX_EXT_C1C2_REQ_PIXELS,size);
 SetReg(CrMdssBootDpu,b+SSPP_SW_PIX_EXT_C3_REQ_PIXELS,size);
 SetReg(CrMdssBootDpu,Lm[0]+4,size);
 SetReg(CrMdssBootDpu,Pp[0]+PP_DSC_MODE,1);
 SetReg(CrMdssBootDpu,Intf[1]+INTF_TEAR_TEAR_CHECK_EN,1);
 SetReg(CrMdssBootDpu,Intf[1]+INTF_MUX,0);
 SetReg(CrMdssBootDpu,Intf[1]+INTF_TEAR_START_POS,2340);
 SetReg(CrMdssBootDpu,Intf[1]+INTF_TEAR_SYNC_WRCOUNT,2345);
 SetReg(CrMdssBootDpu,Intf[1]+INTF_TEAR_SYNC_CONFIG_VSYNC,BIT(19)|BIT(20)|1000);
 SetReg(CrMdssBootDpu,Dsc[0]+DSC_PICTURE,(1080U<<16)|2340U);
 SetReg(CrMdssBootDpu,Dsc[0]+DSC_SLICE,(540U<<16)|20U);
 for(u32 i=0;i<4;i++)SetReg(CrMdssBootDpu,Dsc[i]+DSC_CTL(i+1),i?15:0);
 SetReg(CrMdssBootHost0,DSI_CTRL,BIT(8)|(15U<<4)|BIT(2)|BIT(0));
 SetReg(CrMdssBootPhy0,0x3c,1);SetReg(CrMdssBootPhy0,0x24,0x7f);SetReg(CrMdssBootPhy0,0x14,0x21);
 for(u32 i=0;i<3;i++){const u32 mask[]={BIT(8)|BIT(26),BIT(9),7};SetReg(CrMdssBootDpu,BootReferenceIrqOffsets[i][0],mask[i]);}
 memset(Dma,0,sizeof(*Dma));Dma->ProducerGeneration=7;Dma->DeviceAddress=FRAME_PHYSICAL;
 Dma->PhysicalAddress=FRAME_PHYSICAL;Dma->Bytes=1080*4*2340;Dma->Count=1;
 Dma->Streams[0].Smr=BIT(31)|(0x402U<<16)|0x820U;Dma->Streams[0].S2cr=1;
 Dma->Streams[0].Sctlr=1;Dma->Streams[0].Index=3;Dma->Streams[0].Bank=1;
}
'''
oracle_path=out/'BootObserveReference.c';oracle_path.write_text(oracle)
if a.generate_only:
    print(oracle_path);raise SystemExit(0)
test=pkg/'Tests/MdssBootObserveTest.c';production=pkg/'Library/MdssBoot8250CrLib/Observe.c'
inputs={Path(__file__).resolve(),test,production,oracle_path,irq_file,manifest_path,root/'hdk8245_fdt',*src.iterdir()}
digest=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
frozen={p:digest(p) for p in inputs}
def freeze_deps(dep):
    for p in [Path(x) for x in dep.read_text().split(':',1)[1].replace('\\\n','').split()]:
        d=digest(p);assert p not in frozen or frozen[p]==d,p;frozen[p]=d
for kernel in (False,True):
    mode='nt' if kernel else 'native'
    flags=['clang','-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-Wno-sign-compare',
           '-fsanitize=address,undefined','-fno-sanitize-recover=all','-I'+str(pkg/'Include')]
    if kernel:flags+=['-D_KERNEL_MODE','-I'+str(pkg/'Tests/KernelMocks')]
    else:flags+=['-fshort-wchar','-I'+str(root/'MU_BASECORE/MdePkg/Include'),'-I'+str(root/'MU_BASECORE/MdePkg/Include/X64')]
    objs=[]
    for i,p in enumerate([test,oracle_path,production]):
        dep=out/f'{mode}-{i}.d';obj=out/f'{mode}-{i}.o'
        # Original Android reader shifts a signed mask into bit31. Retain its
        # exact kernel source; only this oracle TU omits that UBSan subcheck.
        unit_flags=flags+(['-fno-sanitize=shift-base'] if p==oracle_path else [])
        run(unit_flags+['-MM','-MF',dep,p]);freeze_deps(dep);run(unit_flags+['-c',p,'-o',obj]);objs.append(obj)
    exe=out/mode;run(['clang','-fsanitize=address,undefined',*objs,'-o',exe])
    r=run([exe],timeout=30);(out/f'{mode}.log').write_text(r.stdout+r.stderr);print(mode+': '+r.stdout.strip())
flags=['clang','--target=aarch64-none-elf','-std=c11','-O2','-g','-ffreestanding','-fshort-wchar','-fstack-usage',
       '-Wall','-Wextra','-Werror','-I'+str(pkg/'Include'),'-I'+str(root/'MU_BASECORE/MdePkg/Include'),
       '-I'+str(root/'MU_BASECORE/MdePkg/Include/AArch64')]
dep=out/'aarch64.d';run(flags+['-MM','-MF',dep,production]);freeze_deps(dep)
run(flags+['-c',production,'-o',out/'Observe.aarch64.o'])
for p,d in frozen.items():assert digest(p)==d,p
(out/'artifacts.json').write_text(json.dumps({'status':'PASS','scope':__doc__,'commands':commands,
 'inputs':{str(p):d for p,d in sorted(frozen.items())},
 'artifacts':{p.name:digest(p) for p in sorted(out.iterdir()) if p.is_file() and p.name!='artifacts.json'}},indent=2)+'\n')
