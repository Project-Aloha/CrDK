#!/usr/bin/env python3
"""Generate HDK PCIe register/resource facts. SPDX-License-Identifier: MIT.
Uses board Android DTBs and pinned public Linux clock/PHY definitions only.
Android status is deliberately not an enable policy: every RC is attempted.
"""
import argparse, hashlib, re, subprocess
import struct,json
from pathlib import Path
class Fdt:
 def __init__(self,p):
  b=Path(p).read_bytes();_,_,o,st=struct.unpack_from('>4I',b);stack=[];self.n={}
  while True:
   t=struct.unpack_from('>I',b,o)[0];o+=4
   if t==1:
    e=b.index(0,o);stack.append(b[o:e].decode());o=(e+4)&~3;self.n['/'.join(stack) or '/']={}
   elif t==2:stack.pop()
   elif t==3:
    z,k=struct.unpack_from('>2I',b,o);o+=8;e=b.index(0,st+k);self.n['/'.join(stack) or '/'][b[st+k:e].decode()]=b[o:o+z];o=(o+z+3)&~3
   elif t==9:break
   elif t!=4:raise ValueError(t)
  self.ph={self.cells(v['phandle'])[0]:k for k,v in self.n.items() if 'phandle' in v}
 def cells(self,b):return list(struct.unpack('>'+str(len(b)//4)+'I',b))
 def strings(self,b):return b.rstrip(b'\0').decode().split('\0')
 def supply(self,p,k):
  q=self.ph[self.cells(self.n[p][k])[0]];v=self.n[q];up=q.rsplit('/',1)[0];r=self.n[up]
  return dict(path=q,resource=self.strings(r.get('qcom,resource-name',b'\0')),
    **{k:self.strings(v[k]) if k in ['regulator-name','compatible'] else self.cells(v[k]) for k in v if k.startswith('regulator-') or k in ['compatible','gpio']})
root=Path(__file__).resolve().parents[3]
parser=argparse.ArgumentParser();parser.add_argument('--check',action='store_true');args=parser.parse_args()
commit='a53261b60834ce617cc4f3c7723dda8285c59789'
refs={}
def linux(path):
 b=subprocess.check_output(['git','-C',str(root/'.vscode/linux'),'show',commit+':'+path])
 refs[path]=hashlib.sha256(b).hexdigest();return b.decode()
def node(text,label):
 m=re.search(r'\b'+re.escape(label)+r': [^{]+\{',text);assert m,label
 p=m.end();i=p;d=1
 while d:d+=(text[i]=='{')-(text[i]=='}');i+=1
 return text[p:i-1]
def field(text,key):
 m=re.search(r'\b'+re.escape(key)+r'\s*=\s*(.*?);',text,re.S);assert m,key
 return m[1]
def save(path,data):
 p=root/path
 if args.check:assert p.read_text()==data,str(p)
 else:p.write_text(data)
phy_source=linux('drivers/phy/qualcomm/phy-qcom-qmp-pcie.c')
for soc,pkg,board,count in [('8250','KonaCrPkg','8245',3),('8350','LahainaCrPkg','8350',2)]:
 dtpath=f'Platforms/{pkg}/Device/qcom-hdk{board}/DeviceTreeBlob/'+('Source/hdk8245-runtime.dtb' if soc=='8250' else 'Android/android-hdk8350.dtb')
 dt=Fdt(root/dtpath);dtsi=linux(f'arch/arm64/boot/dts/qcom/sm{soc}.dtsi')
 pinctrl=linux(f'drivers/pinctrl/qcom/pinctrl-sm{soc}.c')
 arrays={n:[] for n in ['Regions','Ranges','Clocks','Resets','Supplies','Gpios','IommuMaps','Phys','PhyInit','Controllers']}
 defs=[];facts={'dtb':dtpath,'dtb_sha256':hashlib.sha256((root/dtpath).read_bytes()).hexdigest(),'linux_commit':commit,'ports':[]}
 def add(name,row):
  i=len(arrays[name]);arrays[name].append('  '+row+',');return i
 def gpio(role,pin,pol='HIGH',mux=0,drive=2,pull=2):
  return add('Gpios',f'{{"{role}","tlmm",{pin},"GPIO_ACTIVE_{pol}",{mux},{drive},{pull}}}')
 def supply(name,rid,mv,mode=7,provider='rpmh'):
  return add('Supplies',f'{{"{name}","{provider}","{rid}",{mv},0,{mode}}}')
 def power_supply(rows,name,rid,mv,mode=7,provider='rpmh'):
  i=supply(name,rid,mv,mode,provider);rows.append(f'{{PCIE_POWER_SUPPLY,{i},0}}')
 for port in range(count):
  p=f'/soc/qcom,pcie@{0x1c00000+port*0x8000:x}';v=dt.n[p]
  regs=dict(zip(dt.strings(v['reg-names']),zip(*[iter(dt.cells(v['reg']))]*2)))
  ro=len(arrays['Regions'])
  for dst,src in [('parf','parf'),('dbi','dm_core'),('elbi','elbi'),('atu','iatu'),('config','conf')]:
   base,size=regs[src];add('Regions',f'{{"{dst}",{base:#x}ULL,{size:#x}}}')
  ra=len(arrays['Ranges']);cells=dt.cells(v['ranges']);assert len(cells)==12
  for j in (0,6):
   typ,hi,lo,cpu,szhi,szlo=cells[j:j+6];assert not hi and not szhi
   add('Ranges',f'{{PCIE_RANGE_{"IO" if typ==0x1000000 else "MEM32"},{lo:#x}ULL,{cpu:#x}ULL,{szlo:#x}ULL}}')
  cs=[]
  for which,label in enumerate((f'pcie{port}',f'pcie{port}_phy')):
   b=node(dtsi,label);ids=re.findall(r'<&gcc (\w+)>',field(b,'clocks'));names=re.findall(r'"([^"]+)"',field(b,'clock-names'));assert len(ids)==len(names)
   start=len(arrays['Clocks'])
   for name,ident in zip(names,ids):
    rate=19200000 if name=='aux' else 100000000 if name in ('refgen','rchng') else 0
    add('Clocks',f'{{"{name}","gcc","{ident.lower()}",{rate},PCIE_CLOCK_PROVIDER_GCC}}')
   cs.append((start,len(ids)))
  reset=len(arrays['Resets'])
  for suffix in ('','_phy'):add('Resets',f'{{"pci{suffix}","gcc","gcc_pcie_{port}{suffix}_bcr",0,PCIE_CLOCK_PROVIDER_GCC}}')
  so=len(arrays['Supplies'])
  for prop,name in [('vreg-0p9','vdda-phy'),('vreg-1p8','vdda-pll')]:
   r=dt.supply(p,prop+'-supply')['resource'][0];mv=dt.cells(v['qcom,'+prop+'-voltage-level'])[0]//1000
   supply(name,r,mv)
  go=len(arrays['Gpios']);perst=dt.cells(v['perst-gpio'])[1];wake=dt.cells(v['wake-gpio'])[1];clkreq=perst+1
  m=re.search(r'\['+str(clkreq)+r'\]\s*=\s*PINGROUP\(\s*'+str(clkreq)+(r',\s*[^,]+,([^\n]+)\)' if soc=='8250' else r',([^\n]+)\)'),pinctrl)
  assert m,(soc,clkreq)
  funcs=[x.strip() for x in m[1].split(',')];mux=1+next(i for i,x in enumerate(funcs) if x in [f'pci_e{port}',f'pcie{port}_clkreqn'])
  gpio('perst',perst,'LOW',pull=1);gpio('wake',wake);gpio('clkreq',clkreq,mux=mux)
  io=len(arrays['IommuMaps']);maps=dt.cells(v['iommu-map']);assert len(maps)==8
  for j in (0,4):
   bdf,ph,sid,n=maps[j:j+4];assert n==1
   add('IommuMaps',f'{{{bdf:#x},"apps_smmu",{sid:#x},1}}')
  # Board PHY sequence is already flattened across lanes. Keep exact order,
  # exclude only lifecycle writes owned by PcieLib (power/reset/start).
  pcs=0x800 if soc=='8250' and port==0 else 0xa00 if soc=='8250' else 0x200
  raw=dt.cells(v['qcom,phy-sequence']);assert len(raw)%3==0
  trip=list(zip(*[iter(raw)]*3));assert all(delay==0 for _,_,delay in trip)
  assert trip[0]==(pcs+0x40,3,0) and trip[-2:]==[(pcs,0,0),(pcs+0x44,3,0)]
  po=len(arrays['PhyInit'])
  for off,val,_ in trip[1:-2]:
   assert off not in (pcs,pcs+0x40,pcs+0x44) and off+4<=regs['phy'][1]
   add('PhyInit',f'{{PCIE_PHY_PHASE_BASE,PCIE_PHY_BLOCK_SERDES,{off:#x},{val:#x},1}}')
  lanes=1 if port==0 else 2
  offname='qmp_pcie_offsets_v4x1' if soc=='8250' and port==0 else 'qmp_pcie_offsets_v4x2' if soc=='8250' else 'qmp_pcie_offsets_v5'
  offsets=re.search(r'struct qmp_pcie_offsets '+offname+r' = \{(.*?)\n\};',phy_source,re.S)[1]
  od={k:int(val,0) for k,val in re.findall(r'\.(\w+)\s*=\s*(0x[0-9a-f]+)',offsets)}
  assert od['pcs']==pcs
  phybase,physize=regs['phy'];cstart,cn=cs[1]
  add('Phys',f'''{{.Label="pcie{port}_phy",.Compatible="qcom,sm{soc}-qmp-gen3x{lanes}-pcie-phy",
    .Base={phybase:#x},.Size={physize:#x},.Lanes={lanes},
    .BlockOffsets={{{','.join(hex(od.get(k,0)) for k in ['serdes','tx','rx','tx2','rx2','pcs','pcs_misc'])}}},
    .SwReset=0,.StartControl=0x44,.Status=0x14,.PowerDownControl=0x40,
    .PowerDownControlValue=3,.StatusMask=0x40,.InitOffset={po},.InitCount={len(trip)-3},
    .ClockOffset={cstart},.ClockCount={cn},.ResetOffset={reset+1},.ResetCount=1}}''')
  rows=[]
  if port==0:
   wlan=f'/soc/qcom,cnss-qca{"6390" if soc=="8250" else "6490"}@b0000000';w=dt.n[wlan]
   # Voltage selectors agree with existing shared rail owners. QCA6490's
   # S12B is already an enable-only retained UFS supplier; keep that policy.
   policy={'8250':[("smpa6",952,7),("smpf2",952,7),("smpa4",1800,7),("smpa5",1904,7),("smpc8",1352,7),("ldoc5",1800,7)],
           '8350':[("smpe2",952,6),("smpb11",952,6),("smpb10",1800,6),("smpc1",1880,6),("smpb12",0,0),("ldoe7",2800,7)]}[soc]
   for (rid,mv,mode),prop in zip(policy,['vdd-wlan-aon','vdd-wlan-dig','vdd-wlan-io','vdd-wlan-rfa1','vdd-wlan-rfa2','wlan-ant-switch']):
    assert dt.supply(wlan,prop+'-supply')['resource']==[rid]
    power_supply(rows,prop,rid,mv,mode,'rpmh' if mv else 'rpmh-enable')
   if soc=='8350':rows.append('{PCIE_POWER_DELAY,0,100000}')
   i=gpio('wlan-enable',dt.cells(w['wlan-en-gpio'])[1],drive=16);rows.append(f'{{PCIE_POWER_GPIO,{i},1000}}')
  elif port==1:
   # The WiGig platform's default pinctrl only owns mux/pull/drive.
   # Public msm_11ad drives only separately named enable GPIOs.
   wigig=dt.n['/soc/qcom,wil6210'];pinpath=dt.ph[dt.cells(wigig['pinctrl-0'])[0]]
   refpin=14 if soc=='8250' else 67
   assert dt.strings(dt.n[pinpath+'/mux']['pins'])==[f'gpio{refpin}']
   assert dt.strings(dt.n[pinpath+'/mux']['function'])==['gpio']
   assert 'bias-pull-down' in dt.n[pinpath+'/config']
   assert dt.cells(dt.n[pinpath+'/config']['drive-strength'])==[2]
   i=gpio('wigig-refclk-pinctrl',refpin,pull=1);rows.append(f'{{PCIE_POWER_PINCTRL,{i},0}}')
   if soc=='8250':
    power_supply(rows,'wigig-io','smpa5',1904)
    power_supply(rows,'wigig-ldo','ldoa15',1800)
   else:power_supply(rows,'wigig-io','smpc1',1880,6)
   power_supply(rows,'wigig-rf-reference','rfclka1' if soc=='8250' else 'rfclka5',0,0,'rpmh-enable')
   if 'qcom,wigig-en' in wigig:
    # msm_11ad.c WIGIG_ENABLE_DELAY=50ms follows its active-high enable.
    spec=dt.cells(wigig['qcom,wigig-en']);assert spec[2]==0
    i=gpio('wigig-enable',spec[1],pull=0);rows.append(f'{{PCIE_POWER_GPIO,{i},50000}}')
   if 'qcom,ep-gpio' in v:
    i=gpio('endpoint-enable',dt.cells(v['qcom,ep-gpio'])[1]);rows.append(f'{{PCIE_POWER_GPIO,{i},10000}}')
  else:
   fixed=dt.supply(p,'vreg-pcievbus-supply');assert fixed['compatible']==['regulator-fixed']
   i=gpio('pcievbus',fixed['gpio'][1],pull=0);rows.append(f'{{PCIE_POWER_GPIO,{i},1000}}')
   i=gpio('ponrst',dt.cells(v['qcom,ponrst'])[1]);rows.append(f'{{PCIE_POWER_GPIO,{i},{dt.cells(v["qcom,ponrst-delay-us-min"])[0]}}}')
  defs += [f'STATIC CONST PcieTargetPowerStep mPower{port}Steps[] = {{'+','.join(rows)+'};',f'STATIC CONST PcieTargetPowerSequence mPower{port} = {{mPower{port}Steps,ARRAY_SIZE(mPower{port}Steps)}};']
  speed=dt.cells(v.get('qcom,target-link-speed',b'\0\0\0\3'))[0]
  cstart,cn=cs[0];ib=0x1000+port*4
  add('Controllers',f'''{{.Label="pcie{port}",.Compatible="qcom,pcie-sm{soc}",.LinuxConfig="cfg_1_9_0",.LinuxOps="ops_1_9_0",
    .Enabled=TRUE,.DmaAbove4G=TRUE,.Domain={port},.BusStart=0,.BusEnd=1,.Lanes={lanes},.MaxLinkSpeed={speed},
    .RegionOffset={ro},.RegionCount=5,.RangeOffset={ra},.RangeCount=2,
    .ClockOffset={cstart},.ClockCount={cn},.ResetOffset={reset},.ResetCount=1,
    .SupplyOffset={so},.SupplyCount=2,.PowerDomainController="gcc",.PowerDomainId="pcie_{port}_gdsc",
    .GpioOffset={go},.GpioCount=3,.IommuMapOffset={io},.IommuMapCount=2,.PhyIndex={port},
    .Interconnect={{"qcom,sm{soc}-pcie",{ib:#x},{ib+1:#x},{ib+2:#x},{ib+3:#x},0,2500000,0,1000}},
    .ParfSysControlSetMask=0x365e,.PowerSequence=&mPower{port}}}''')
  facts['ports'].append({'domain':port,'enabled':True,'android_status':dt.strings(v.get('status',b'ok\0'))[0], 'phy_writes':len(trip)-3,'pins':[perst,clkreq,wake],'clock_slices':cs,'max_speed':speed})
 types={'Regions':'Region','Ranges':'Range','Clocks':'Clock','Resets':'Reset','Supplies':'Supply','Gpios':'Gpio','IommuMaps':'IommuMap','Phys':'Phy','PhyInit':'PhyInitEntry','Controllers':'Controller'}
 out=[f'''/* Generated HDK{board} PCIe facts; SPDX-License-Identifier: MIT.
 * DTB SHA256 {facts['dtb_sha256']}
 * Linux {commit}; public Android PHY register facts, not driver code.
 * Regenerate: python3 Platforms/CranePkg/Tools/generate_hdk_pcie.py
 * Every RC is attempted, independently of Android node status.
 */
#include <Library/CrTargetPcieLib.h>
#include <oskal/common.h>''',*defs]
 for name,rows in arrays.items():
  typ='PciePhyInitEntry' if name=='PhyInit' else 'PcieTarget'+types[name]
  out+=[f'STATIC CONST {typ} m{name}[] = {{',*rows,'};']
 out+=['STATIC PcieTargetContext mTarget = {']
 for name in arrays:
  countfield={'Regions':'Region','Ranges':'Range','Clocks':'Clock','Resets':'Reset','Supplies':'Supply','Gpios':'Gpio','IommuMaps':'IommuMap','Phys':'Phy','PhyInit':'PhyInit','Controllers':'Controller'}[name]
  out.append(f'  .{name}=m{name},.{countfield}Count=ARRAY_SIZE(m{name}),')
 out+=['};','PcieTargetContext *CrTargetGetPcieContext(VOID) { return &mTarget; }','']
 save(f'Platforms/CranePkg/Target/Sm{soc}/CrPcieTarget.c','\n'.join(out))
 facts['linux_sources']=refs
 save(f'Platforms/{pkg}/Tests/sm{soc}-pcie-facts.json',json.dumps(facts,indent=2)+'\n')
 print(f'HDK{board}: {count} RCs, {len(arrays["PhyInit"])} PHY writes, all interface power sequences')
