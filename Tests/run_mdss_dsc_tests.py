#!/usr/bin/env python3
"""Actual DSC core plus independently compiled Qualcomm reference and fault IO."""
from pathlib import Path
import argparse,hashlib,json,re,subprocess
ap=argparse.ArgumentParser();ap.add_argument('--output',type=Path,required=True);a=ap.parse_args()
root=Path(__file__).resolve().parents[3];pkg=root/'Platforms/CranePkg';out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
ref=root/'.vscode/android_kernel_modules_and_devicetree_oneplus_sm8450/vendor/qcom/opensource/display-drivers/msm/sde_dsc_helper.c'
s=ref.read_text();body=s[s.index('enum sde_dsc_ratio_type'):s.index('int sde_dsc_populate_dsc_private_params')]
pps=s[s.index('int sde_dsc_create_pps_buf_cmd'):]
# Exact upstream functions/tables are compiled against a minimal DRM data ABI.
fields=set(re.findall(r'dsc->(\w+)',body+pps))-{'rc_range_params','rc_buf_thresh'}
pre="""/* Independent Qualcomm GPL-2.0-only source oracle; generated only for tests. */
#include <stdint.h>
#include <string.h>
#undef NULL
#include <Library/MdssDscCrLib.h>
typedef uint8_t u8;typedef uint16_t u16;typedef uint32_t u32;
#define BIT(n) (1U<<(n))
#ifndef ARRAY_SIZE
#define ARRAY_SIZE(x) (sizeof(x)/sizeof((x)[0]))
#endif
#define DIV_ROUND_UP(x,d) (((x)+(d)-1)/(d))
#define DSC_BPP(x) ((x).bits_per_pixel>>4)
#define DSC_NUM_BUF_RANGES 15
#define DSC_MUX_WORD_SIZE_8_10_BPC 48
#define DSC_MUX_WORD_SIZE_12_BPC 64
#define MSM_CHROMA_444 0
#define MSM_CHROMA_422 1
#define MSM_CHROMA_420 2
#define EINVAL 22
#define SDE_ERROR(...) ((void)0)
#define DSC_PPS_BPC_SHIFT 4
#define DSC_PPS_MSB_SHIFT 8
#define DSC_PPS_LSB_MASK 255
#define SDE_DSC_PPS_SIZE 128
struct range {int range_min_qp,range_max_qp,range_bpg_offset;};
struct drm_dsc_config {
"""
pre+=''.join('  u32 '+f+';\n' for f in sorted(fields))
pre+='  u32 rc_buf_thresh[14]; struct range rc_range_params[15];\n};\nstruct msm_display_dsc_info {struct drm_dsc_config config;};\n'
mp={
 'SliceCount':'slice_count','ChunkBytes':'slice_chunk_size','LineBufferDepth':'line_buf_depth',
 'InitialTransmitDelay':'initial_xmit_delay','InitialDecodeDelay':'initial_dec_delay','InitialScale':'initial_scale_value',
 'ScaleIncrement':'scale_increment_interval','ScaleDecrement':'scale_decrement_interval','FirstLineBpg':'first_line_bpg_offset',
 'NflBpg':'nfl_bpg_offset','SliceBpg':'slice_bpg_offset','InitialOffset':'initial_offset','FinalOffset':'final_offset',
 'FlatnessMinQp':'flatness_min_qp','FlatnessMaxQp':'flatness_max_qp','QuantLimit':'rc_quant_incr_limit0','RcModelSize':'rc_model_size'}
# slice_count isn't used by populate, derive it from independent panel geometry.
pre=pre.replace('struct drm_dsc_config {','struct drm_dsc_config {\n  u32 slice_count;')
wrap="""
static void Input(const CR_MDSS_DSC_PARAMS *P,struct drm_dsc_config *d) {
 memset(d,0,sizeof(*d));d->dsc_version_major=1;d->dsc_version_minor=P->MinorVersion;
 d->bits_per_component=P->BitsPerComponent;d->bits_per_pixel=P->BppQ4;
 d->pic_width=P->PictureWidth;d->pic_height=P->PictureHeight;
 d->slice_width=P->SliceWidth;d->slice_height=P->SliceHeight;
 d->block_pred_enable=P->BlockPrediction;d->slice_count=P->PictureWidth/P->SliceWidth;
}
int AndroidBuild(const CR_MDSS_DSC_PARAMS *P,CR_MDSS_DSC_CONFIG *C) {
 struct drm_dsc_config d;int i;Input(P,&d);
 int r=sde_dsc_populate_dsc_config(&d,P->ScrVersion);if(r)return r;
 memset(C,0,sizeof(*C));C->Params=*P;if(P->MinorVersion==2)C->Params.ScrVersion=0;
"""
wrap+=''.join(' C->'+c+'=d.'+d+';\n' for c,d in mp.items())
wrap+="""
 for(i=0;i<14;++i)C->Thresholds[i]=d.rc_buf_thresh[i];
 for(i=0;i<15;++i){C->Ranges[i].MinQp=d.rc_range_params[i].range_min_qp;
  C->Ranges[i].MaxQp=d.rc_range_params[i].range_max_qp;
  C->Ranges[i].BpgOffset=d.rc_range_params[i].range_bpg_offset;}return 0;
}
void AndroidPps(const CR_MDSS_DSC_CONFIG *C,UINT8 *P,UINT8 Id) {
 struct msm_display_dsc_info d;Input(&C->Params,&d.config);
 sde_dsc_populate_dsc_config(&d.config,C->Params.ScrVersion);
 sde_dsc_create_pps_buf_cmd(&d,(char *)P,Id,128);
}
"""
# Include fields only referenced by input/wrapper as well as the original body.
for f in ['block_pred_enable','pic_width','pic_height']:
 if f not in fields:pre=pre.replace('struct drm_dsc_config {','struct drm_dsc_config {\n  u32 '+f+';')
source=out/'android_oracle.c';source.write_text(pre+body+pps+wrap)
inputs=[pkg/'Tests/MdssDscTest.c',pkg/'Library/MdssDscCrLib/MdssDsc.c',pkg/'Include/Library/MdssDscCrLib.h',ref]
results=[]
for kernel in (False,True):
 flags=['clang','-std=c11','-g','-O1','-Wall','-Wextra','-Werror','-Wno-sign-compare','-Wno-unused-parameter','-fsanitize=address,undefined','-fno-sanitize-recover=all','-I'+str(pkg/'Include')]
 if kernel:flags+=['-D_KERNEL_MODE','-I'+str(pkg/'Tests/KernelMocks')]
 else:flags+=['-fshort-wchar','-I'+str(root/'MU_BASECORE/MdePkg/Include'),'-I'+str(root/'MU_BASECORE/MdePkg/Include/X64')]
 exe=out/('nt' if kernel else 'native');cmd=flags+[str(inputs[0]),str(inputs[1]),str(source),'-o',str(exe)]
 run=subprocess.run(cmd,text=True,capture_output=True);(out/(exe.name+'-compile.log')).write_text(run.stdout+run.stderr)
 if run.returncode:print(run.stderr);raise SystemExit(run.returncode)
 run=subprocess.run([str(exe)],text=True,capture_output=True,timeout=60);(out/(exe.name+'.log')).write_text(run.stdout+run.stderr)
 print(run.stdout+run.stderr,end='');run.check_returncode();results.append(exe.name)
(out/'artifacts.json').write_text(json.dumps({'status':'PASS','results':results,'inputs':{str(p.relative_to(root)):hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}},indent=2)+'\n')
