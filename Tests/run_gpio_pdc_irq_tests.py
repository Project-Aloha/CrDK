#!/usr/bin/env python3
"""Actual native GPIO/PDC helpers, shared cores and OSKAL failure injection."""
import argparse
from pathlib import Path
import subprocess
import tempfile
p=Path(__file__).resolve().parents[1]
parser=argparse.ArgumentParser()
parser.add_argument('--package-root',type=Path,default=p)
parser.add_argument('--oskal',type=Path)
parser.add_argument('--include',type=Path,action='append',default=[])
parser.add_argument('--test-source',type=Path)
parser.add_argument('--pdc-irq',type=Path)
parser.add_argument('--gpio-irq',type=Path)
args=parser.parse_args()
p=args.package_root.resolve()
args.oskal=args.oskal or p/'Library/OskInterruptLib/oskinterrupt.c'
args.pdc_irq=args.pdc_irq or p/'Library/PdcLib/pdc_irq.c'
args.gpio_irq=args.gpio_irq or p/'Library/GpioLib/gpio_irq.c'
args.test_source=args.test_source or Path(__file__).with_name('GpioPdcIrqTest.c')
with tempfile.TemporaryDirectory(prefix='cr-native-gpio-pdc-') as name:
    exe=Path(name)/'irq'
    subprocess.run(['clang','-std=c11','-fshort-wchar','-DMDEPKG_NDEBUG','-g','-O1',
        '-Wall','-Wextra','-Werror','-Wno-unused-parameter','-fsanitize=address,undefined',
        '-fno-sanitize-recover=all','-ffunction-sections','-fdata-sections','-Wl,--gc-sections',
        *['-I'+str(i) for i in args.include],
        '-DCR_OSKAL_INTERRUPT_IMPLEMENTATION=\"'+str(args.oskal.resolve())+'\"',
        '-I'+str(p/'Include'),'-I'+str(p/'Library/PdcLib'),'-I'+str(p/'Library/GpioLib'),
        '-I'+str(p.parents[1]/'MU_BASECORE/MdePkg/Include'),
        '-I'+str(p.parents[1]/'MU_BASECORE/MdePkg/Include/X64'),
        '-I'+str(p.parents[1]/'MU_BASECORE/EmbeddedPkg/Include'),
        str(args.test_source),str(p/'Library/PdcLib/pdc.c'),str(args.pdc_irq),
        str(p/'Library/GpioLib/gpio.c'),str(args.gpio_irq),
        str(p/'Target/Sm8450/CrGpioTarget.c'),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True,timeout=30)
