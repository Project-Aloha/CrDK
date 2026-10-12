#!/usr/bin/env python3
"""Actual HFI engine + unmodified extracted Linux wire functions; frozen closure."""
import argparse
import datetime
import hashlib
import json
import re
import shlex
import subprocess
import tarfile
from pathlib import Path

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--output', required=True, type=Path)
p.add_argument('--arm64-clang', type=Path)
p.add_argument('--wdk', type=Path)
p.add_argument('--sdk', type=Path)
p.add_argument('--trace-dir', type=Path)
a = p.parse_args()
c = Path(__file__).resolve().parents[1]
r = c.parents[1]
o = a.output.resolve()
o.mkdir(parents=True, exist_ok=True)
if (o / 'artifacts.json').exists():
    raise RuntimeError('Use fresh output; preserve prior evidence')
sha = lambda f: hashlib.sha256(f.read_bytes()).hexdigest()
linux = r / '.vscode/linux/drivers/gpu/drm/msm/adreno'
original = linux / 'a6xx_hfi.c'
header = linux / 'a6xx_hfi.h'
text = original.read_text()
names = ['a6xx_hfi_queue_read', 'a6xx_hfi_queue_write', 'a6xx_hfi_send_msg', 'a6xx_hfi_send_perf_table',
         'a730_build_bw_table', 'a6xx_hfi_send_start', 'a6xx_hfi_send_core_fw_start',
         'a6xx_hfi_queue_init', 'a6xx_hfi_init']
parts = []
facts = []
for name in names:
    match = re.search(r'^(?:static )?(?:int|void) ' + name + r'\(', text, re.M)
    if not match:
        raise RuntimeError('Missing original function ' + name)
    start = match.start()
    brace = text.index('{', match.end())
    depth, end = 1, brace + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    part = text[start:end]
    parts.append(part)
    facts.append({'function': name, 'line': text.count('\n', 0, start) + 1,
                  'sha256': hashlib.sha256(part.encode()).hexdigest()})
(o / 'OriginalHfi.h').write_bytes(header.read_bytes())
prefix = r'''
/* Only OS/model scaffolding; all wire functions below are verbatim original. */
#include <Library/AdrenoHfiCrLib.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
typedef uint32_t u32;typedef uint64_t u64;typedef uint16_t u16;typedef uint8_t u8;
typedef int32_t s32;typedef unsigned atomic_t;typedef int spinlock_t;
#define __packed __attribute__((packed))
#define SZ_4K 4096U
#ifndef ARRAY_SIZE
#define ARRAY_SIZE(A) (sizeof(A)/sizeof((A)[0]))
#endif
#define WARN_ON(V) ((void)(V))
#define BUG_ON(V) do {if(V)abort();}while(0)
#ifndef ALIGN
#define ALIGN(V,A) (((V)+(A)-1)&~((A)-1))
#endif
#define DRM_DEV_ERROR(...) ((void)0)
#define CIRC_SPACE(H,T,S) (((T)-(H)-1)&((S)-1))
#define READ_ONCE(X) (X)
#define WRITE_ONCE(X,V) ((X)=(V))
#define spin_lock_init(P) ((void)(P))
#define spin_lock(P) ((void)(P))
#define spin_unlock(P) ((void)(P))
#define atomic_set(P,V) (*(P)=(V))
#define atomic_inc_return(P) (++*(P))
#define dma_mb() ((void)0)
#define container_of(P,T,M) ((T *)((char *)(P)-offsetof(T,M)))
#define REG_A6XX_GMU_HOST2GMU_INTR_SET 0
#include "OriginalHfi.h"
struct a6xx_gmu_bo {void *virt;u64 iova;};
struct a6xx_gmu {void *dev;int legacy;struct a6xx_gmu_bo hfi;struct a6xx_hfi_queue queues[3];
 int nr_gpu_freqs,nr_gmu_freqs;u32 gx_arc_votes[16],cx_arc_votes[4];u64 gpu_freqs[16],gmu_freqs[4];};
struct adreno_gpu {int unused;};struct a6xx_gpu {struct adreno_gpu base;struct a6xx_gmu gmu;};
static int adreno_is_a8xx(struct adreno_gpu *a){(void)a;return 0;}
static int a8xx_hfi_send_perf_table(struct a6xx_gmu *g){(void)g;abort();}
static void gmu_write(struct a6xx_gmu *g,unsigned reg,unsigned val){(void)g;(void)reg;(void)val;}
static int a6xx_hfi_wait_for_ack(struct a6xx_gmu *g,u32 id,u32 seq,u32 *p,u32 n){
 (void)g;(void)id;(void)seq;(void)p;(void)n;return 0;}
static const CR_ADRENO_HFI_BOOT_PLAN *SourcePlan;
static u32 cmd_db_read_addr(const char *name){
 const char *names[]={"SH0","MC0","ACV","CN0"};
 for(unsigned n=0;n<4;++n)if(!strcmp(names[n],name))return SourcePlan->BcmAddress[n];abort();}
'''
suffix = r'''
void OracleQueues(void *memory,UINT32 iova){
 struct a6xx_gpu gpu={0};gpu.gmu.hfi.virt=memory;gpu.gmu.hfi.iova=iova;a6xx_hfi_init(&gpu.gmu);}
UINT32 OracleEmptyRequest(void){
 u32 memory[4096]={0},data[19]={0};struct a6xx_gpu gpu={0};gpu.gmu.hfi.virt=memory;gpu.gmu.hfi.iova=0x60001000;
 a6xx_hfi_init(&gpu.gmu);gpu.gmu.queues[1].header->rx_request=0;
 if(a6xx_hfi_queue_read(&gpu.gmu,&gpu.gmu.queues[1],data,19)!=0)abort();
 return gpu.gmu.queues[1].header->rx_request;}
void OracleBoot(const CR_ADRENO_HFI_BOOT_PLAN *p,UINT32 index,UINT32 *words,UINT32 *count,UINT32 *id){
 u32 memory[4096]={0};struct a6xx_gpu gpu={0};struct a6xx_gmu *g=&gpu.gmu;
 g->hfi.virt=memory;g->hfi.iova=0x60001000;a6xx_hfi_init(g);SourcePlan=p;
 g->nr_gpu_freqs=p->Power.GxCount;g->nr_gmu_freqs=p->Power.CxCount;
 for(unsigned n=0;n<16;++n){g->gx_arc_votes[n]=p->Power.Gx[n].Vote;g->gpu_freqs[n]=(u64)p->Power.Gx[n].FrequencyKHz*1000;}
 for(unsigned n=0;n<4;++n){g->cx_arc_votes[n]=p->Power.Cx[n].Vote;g->gmu_freqs[n]=(u64)p->Power.Cx[n].FrequencyKHz*1000;}
 if(index==0)a6xx_hfi_send_perf_table(g);
 else if(index==1){struct a6xx_hfi_msg_bw_table m={0};a730_build_bw_table(&m);
   a6xx_hfi_send_msg(g,HFI_H2F_MSG_BW_TABLE,&m,sizeof(m),NULL,0);}
 else if(index==2)a6xx_hfi_send_core_fw_start(g);else if(index==3)a6xx_hfi_send_start(g);else abort();
 *count=HFI_HEADER_SIZE(memory[1024]);*id=HFI_HEADER_ID(memory[1024]);
 memcpy(words,&memory[1024],*count*4);
}
'''
oracle = o / 'OriginalHfiOracle.c'
oracle.write_text(prefix + '\n\n'.join(parts) + suffix)
(o / 'source-oracle.json').write_text(json.dumps({'source': str(original), 'sha256': sha(original),
    'header': str(header), 'header_sha256': sha(header), 'functions': facts,
    'limits': 'Original wire/queue write construction; modeled OS, immediate oracle ACK only.'}, indent=2) + '\n')
core = [c / 'Library/AdrenoHfiCrLib/AdrenoHfi.c', c / 'Library/AdrenoHfiCrLib/Sm8450Hfi.c']
sources = [c / 'Tests/AdrenoHfiCrTest.c', *core, c / 'Library/AdrenoPowerCrLib/AdrenoPower.c',
           c / 'Library/AdrenoPowerCrLib/Sm8450Power.c', oracle]
inputs = {*sources, original, header, o / 'OriginalHfi.h', o / 'source-oracle.json', Path(__file__).resolve(),
          c / 'Include/Library/AdrenoHfiCrLib.h', *list((c / 'Library/AdrenoHfiCrLib').iterdir())}
commands = []
for nt in (False, True):
    for release in (False, True):
        tag = ('nt' if nt else 'native') + ('-release' if release else '-debug')
        flags = ['-std=c17', '-pthread', '-g', '-O2' if release else '-O1', '-Wall', '-Wextra', '-Werror',
                 '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                 '-I' + str(c / 'Include'), '-I' + str(o)]
        if release:
            flags += ['-DNDEBUG', '-DMDEPKG_NDEBUG']
        if nt:
            flags += ['-D_KERNEL_MODE', '-I' + str(c / 'Tests/KernelMocks')]
        else:
            flags += ['-fshort-wchar', '-I' + str(r / 'MU_BASECORE/MdePkg/Include'),
                      '-I' + str(r / 'MU_BASECORE/MdePkg/Include/X64')]
        # Original Linux functions retain their signed loop counters and packed
        # header API. Only this original-source TU has the compatibility flags.
        for s in sources:
            sf = flags + (['-Wno-sign-compare', '-Wno-address-of-packed-member'] if s == oracle else [])
            commands.append(('object', tag+'-'+s.stem, 'clang', sf, s, o/(tag+'-'+s.stem+'.o')))
        commands.append(('host', tag, 'clang', flags, None, o/('hfi-'+tag)))
if a.arm64_clang:
    if not all([a.wdk, a.sdk, a.trace_dir]):
        raise RuntimeError('ARM64 NT compile needs wdk/sdk/trace-dir')
    kit = '10.0.26100.0'
    dirs = [a.wdk.resolve() / 'c' / f'Include/{kit}' / n for n in ('km', 'km/crt', 'shared')]
    dirs += [a.sdk.resolve() / 'c' / f'Include/{kit}/shared']
    overlay = o / 'case-overlay.json'
    overlay.write_text(json.dumps({'version': 0, 'case-sensitive': False, 'roots': [
        {'type': 'directory', 'name': str(d), 'contents': [
            {'type': 'file', 'name': f.name, 'external-contents': str(f)}
            for f in d.iterdir() if f.is_file()]} for d in dirs]}))
    inputs.add(overlay)
    for nt in (False, True):
        flags = ['-ffreestanding', '-std=c17', '-Wall', '-Wextra', '-Werror', '-fstack-usage', '-I'+str(c/'Include')]
        if nt:
            flags += ['--target=aarch64-pc-windows-msvc', '-fms-extensions', '-Wno-microsoft-anon-tag',
                      '-D_ARM64_', '-D_KERNEL_MODE', '-D_WIN32_WINNT=0x0A00', '-DNTDDI_VERSION=0x0A00000A',
                      '-ivfsoverlay', str(overlay), '-I'+str(a.trace_dir.resolve())]
            for d in dirs:
                flags += ['-isystem', str(d)]
        else:
            flags += ['--target=aarch64-none-elf', '-fshort-wchar', '-DMDEPKG_NDEBUG',
                      '-I'+str(r/'MU_BASECORE/MdePkg/Include'), '-I'+str(r/'MU_BASECORE/MdePkg/Include/AArch64')]
        for s in core:
            tag = s.stem + ('-nt' if nt else '-efi')
            commands.append(('object', tag, str(a.arm64_clang.resolve()), flags, s, o/(tag+'.o')))
for kind, tag, compiler, flags, s, _ in commands:
    if kind == 'host':
        continue
    dep = subprocess.check_output([compiler, *flags, '-M', str(s)], text=True)
    (o/(tag+'.d')).write_text(dep)
    inputs.update(Path(x).resolve() for x in shlex.split(dep.replace('\\\n', '').split(':', 1)[1]))
before = {str(f): sha(f) for f in sorted(inputs)}
(o/'inputs-before.json').write_text(json.dumps(before, indent=2)+'\n')
runs = []
for kind, tag, compiler, flags, s, artifact in commands:
    cmd = [compiler, *flags]
    if kind == 'object':
        cmd += ['-c', str(s)]
    else:
        cmd += [str(o/(tag+'-'+f.stem+'.o')) for f in sources]
    cmd += ['-o', str(artifact)]
    x = subprocess.run(cmd, text=True, capture_output=True)
    (o/(tag+'-build.log')).write_text(x.stdout+x.stderr)
    if x.returncode:
        print(x.stdout+x.stderr);x.check_returncode()
    rec = {'kind':kind,'tag':tag,'command':cmd,'artifact':str(artifact),'sha256':sha(artifact)}
    if kind == 'host':
        x = subprocess.run([str(artifact)],text=True,capture_output=True)
        log=o/(tag+'.log');log.write_text(x.stdout+x.stderr)
        if x.returncode:
            print(x.stdout+x.stderr);x.check_returncode()
        print(tag+': '+x.stdout,end='');rec.update(log=str(log),log_sha256=sha(log))
    runs.append(rec)
after={str(f):sha(f) for f in sorted(inputs)}
if before!=after:
    raise RuntimeError('Actual compiled/source-oracle closure drifted during checks')
archive=o/'sources.tar.gz'
with tarfile.open(archive,'w:gz') as t:
    for f in sorted(inputs):t.add(f,arcname=str(f).lstrip('/'),recursive=False)
result={'status':'PASS','checked_utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),
        'scope':'Actual retained HFI transport/boot engine + original Linux queue/message functions',
        'inputs':before,'runs':runs,'archive':str(archive),'archive_sha256':sha(archive),
        'generated':{str(f):sha(f) for f in sorted(o.iterdir()) if f.is_file() and f.name!='sources.tar.gz'},
        'limits':['No live Windows backend, HAL/domain/IRQ admission or selected firmware.',
                  'Host backend models owner and DMA visibility; hardware ordering untested.',
                  'No shared catalog/SDK/native selection or DOD changes.']}
(o/'artifacts.json').write_text(json.dumps(result,indent=2)+'\n')
print('AdrenoHfi PASS;',len(inputs),'inputs;',len(runs),'runs/objects')
