#!/usr/bin/env python3
"""Actual new command DPU/legacy DSC units; exact target Android trace oracle."""
import argparse
import hashlib
import json
import re
import subprocess
from pathlib import Path

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--output', type=Path, required=True)
a = p.parse_args()
r = Path(__file__).resolve().parents[3]
c = r / 'Platforms/CranePkg'
o = a.output.resolve()
o.mkdir(parents=True, exist_ok=True)
(o / 'artifacts.json').unlink(missing_ok=True)
target = r / '.tmp/waipio-continuation/hdk8245-panel/android-sm8250/techpack/display/msm/sde'
extra = r / '.tmp/waipio-continuation/mdss-cmd-dpu77-sources'
dsc = target / 'sde_hw_dsc.c'
encoder = extra / 'sde_encoder.c'
s = dsc.read_text()
body = s[s.index('static void sde_hw_dsc_config('):s.index('static void sde_hw_dsc_bind_pingpong_blk(')]
t = encoder.read_text()
latency = t[t.index('static int _sde_encoder_dsc_initial_line_calc('):t.index('static bool _sde_encoder_dsc_ich_reset_override_needed(')]
fields = set(re.findall(r'dsc->(\w+)', body + latency))
arrays = {'range_min_qp', 'range_max_qp', 'range_bpg_offset', 'buf_thresh'}
pre = '''/* Generated test-only oracle: exact Qualcomm GPL-2.0-only functions. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#undef NULL
#include <Library/MdssDscCrLib.h>
typedef uint32_t u32;
#define DIV_ROUND_UP(x,d) (((x)+(d)-1)/(d))
struct msm_display_dsc_info {
'''
pre += ''.join(' u32 ' + f + ';\n' for f in sorted(fields - arrays))
pre += ' u32 buf_thresh[14]; char range_min_qp[15], range_max_qp[15], range_bpg_offset[15];\n};\n'
pre += '''struct sde_hw_blk_reg_map { int ignored; };
struct sde_hw_dsc { struct sde_hw_blk_reg_map hw; };
static UINT32 *Offsets, *Values, *Count;
#define SDE_REG_WRITE(c,o,v) do { (void)(c); assert(*Count<128); Offsets[*Count]=(o); Values[(*Count)++]=(v); } while(0)
'''
pre += ''.join(line + '\n' for line in s.splitlines() if line.startswith('#define DSC_') and not line.endswith('\\'))
mapping = {
    'bpc': 'Params.BitsPerComponent', 'bpp': 'Params.BppQ4 >> 4',
    'block_pred_enable': 'Params.BlockPrediction', 'line_buf_depth': 'LineBufferDepth',
    'pic_width': 'Params.PictureWidth', 'pic_height': 'Params.PictureHeight',
    'slice_width': 'Params.SliceWidth', 'slice_height': 'Params.SliceHeight',
    'chunk_size': 'ChunkBytes', 'initial_dec_delay': 'InitialDecodeDelay',
    'initial_xmit_delay': 'InitialTransmitDelay', 'initial_scale_value': 'InitialScale',
    'scale_decrement_interval': 'ScaleDecrement', 'scale_increment_interval': 'ScaleIncrement',
    'first_line_bpg_offset': 'FirstLineBpg', 'nfl_bpg_offset': 'NflBpg',
    'slice_bpg_offset': 'SliceBpg', 'initial_offset': 'InitialOffset', 'final_offset': 'FinalOffset',
    'max_qp_flatness': 'FlatnessMaxQp', 'min_qp_flatness': 'FlatnessMinQp',
    'rc_model_size': 'RcModelSize', 'quant_incr_limit0': 'QuantLimit', 'quant_incr_limit1': 'QuantLimit'
}
wrap = '''
void AndroidDscTrace(const CR_MDSS_DSC_CONFIG *C, UINT32 *O, UINT32 *V, UINT32 *N) {
 struct msm_display_dsc_info d; struct sde_hw_dsc hw={0}; memset(&d,0,sizeof(d));
 Offsets=O; Values=V; Count=N; *N=0;
'''
wrap += ''.join(' d.' + f + '=C->' + val + ';\n' for f, val in mapping.items())
wrap += '''
 d.convert_rgb=1; d.slice_last_group_size=2; d.det_thresh_flatness=2;
 d.tgt_offset_lo=3; d.tgt_offset_hi=3; d.edge_factor=6;
 for(int i=0;i<14;++i)d.buf_thresh[i]=C->Thresholds[i];
 for(int i=0;i<15;++i){d.range_min_qp[i]=C->Ranges[i].MinQp;
   d.range_max_qp[i]=C->Ranges[i].MaxQp;d.range_bpg_offset[i]=C->Ranges[i].BpgOffset;}
 _sde_encoder_dsc_initial_line_calc(&d,1080); assert(d.initial_lines==3);
 sde_hw_dsc_config(&hw,&d,0,false); sde_hw_dsc_config_thresh(&hw,&d);
}
'''
oracle = o / 'android_dsc_oracle.c'
oracle.write_text(pre + latency + body + wrap)
sources = [c / 'Tests/MdssCmdDpuTest.c'] + [c / 'Library/MdssDsiCmdCrLib' / n for n in ('Io.c','Plan.c','PanelR66451.c','DscLegacy.c','DpuCommand.c')]
sources += [c / 'Library/MdssDscCrLib/MdssDsc.c', oracle]
inputs = sources[:-1] + [c/'Include/Library/MdssDsiCmdCrLib.h', c/'Include/Library/MdssDscCrLib.h', c/'Library/MdssDsiCmdCrLib/Internal.h', Path(__file__)]
before = {str(f.relative_to(r)): hashlib.sha256(f.read_bytes()).hexdigest() for f in inputs}
commands = []
for nt in (False, True):
    flags = ['clang','-std=c11','-g','-O1','-funsigned-char','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-Wno-unused-parameter','-fsanitize=address,undefined','-fno-sanitize-recover=all','-I'+str(c/'Include')]
    flags += ['-D_KERNEL_MODE','-I'+str(c/'Tests/KernelMocks')] if nt else ['-fshort-wchar','-I'+str(r/'MU_BASECORE/MdePkg/Include'),'-I'+str(r/'MU_BASECORE/MdePkg/Include/X64')]
    exe = o / ('nt' if nt else 'native')
    cmd = flags + list(map(str,sources)) + ['-o',str(exe)]
    commands.append(cmd)
    x = subprocess.run(cmd,text=True,capture_output=True)
    (o/(exe.name+'-compile.log')).write_text(x.stdout+x.stderr)
    if x.returncode: print(x.stderr); x.check_returncode()
    x = subprocess.run([str(exe)],text=True,capture_output=True,timeout=60)
    (o/(exe.name+'.log')).write_text(x.stdout+x.stderr)
    print(x.stdout+x.stderr,end=''); x.check_returncode()
after = {str(f.relative_to(r)): hashlib.sha256(f.read_bytes()).hexdigest() for f in inputs}
if before != after: raise RuntimeError('test/compiler inputs changed during validation')
refs = [dsc, encoder, target/'sde_hw_intf.c',target/'sde_encoder_phys_cmd.c']
refs += [extra/n for n in ('manifest.json','sde_hw_vbif.c','sde_hw_ctl.c','sde_hw_interrupts.c','sde_hw_catalog.c')]
refs += [r/'linux/drivers/gpu/drm/msm/disp/dpu1'/n for n in ('catalog/dpu_6_0_sm8250.h','dpu_hw_sspp.c','dpu_hw_lm.c','dpu_hw_util.c')]
refs += [r/'.tmp/waipio-continuation/hdk8245-panel'/n for n in ('hdk8245.dts','selected-nodes.json','qualified-panel.json')]
(o/'artifacts.json').write_text(json.dumps({'status':'PASS','scope':'new unselected actual SM8250 units, native/NT sanitizer boundaries and exact target Android DSC trace; no hardware/native activation claim','inputs':after,'references':{str(f.relative_to(r)):hashlib.sha256(f.read_bytes()).hexdigest() for f in refs},'android_commit':'cad09e061ef6cf689a4a1e54d27562e0f042236a','commands':commands,'oracle_sha256':hashlib.sha256(oracle.read_bytes()).hexdigest()},indent=2)+'\n')
