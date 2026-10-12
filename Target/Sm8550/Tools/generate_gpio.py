#!/usr/bin/env python3
"""Extract public SM8550 TLMM hardware facts; no Linux driver code is copied.

SPDX-License-Identifier: MIT
"""
import argparse
import hashlib
import json
import re
import subprocess
from pathlib import Path

REVISION = 'a53261b60834ce617cc4f3c7723dda8285c59789'
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--linux', required=True, type=Path)
parser.add_argument('--check', action='store_true')
parser.add_argument('--receipt', type=Path)
args = parser.parse_args()
linux = args.linux.resolve()
revision = subprocess.check_output(['git', '-C', str(linux), 'rev-parse', 'HEAD'], text=True).strip()
assert revision == REVISION, 'Public source revision changed; re-audit hardware facts first'
relative = 'drivers/pinctrl/qcom/pinctrl-sm8550.c'
source = linux / relative
data = source.read_bytes()
assert data == subprocess.check_output(['git', '-C', str(linux), 'show', 'HEAD:' + relative]), \
    'Public source has unreviewed working-tree changes'
text = data.decode()
groups = re.findall(r'\[(\d+)\] = PINGROUP\((\d+), ([^)]+)\)', text)
assert [(int(a), int(b)) for a, b, _ in groups] == [(n, n) for n in range(210)]
for fact in ('REG_SIZE 0x1000', '.egpio_enable = 12', '.egpio_present = 11',
             '.i2c_pull_bit = 13', '.egpio_func = 9', '.ngpios = 211',
             'UFS_RESET(ufs_reset, 0xde000)',
             'SDC_QDSD_PINGROUP(sdc2_clk, 0xd6000, 14, 6)',
             'SDC_QDSD_PINGROUP(sdc2_cmd, 0xd6000, 11, 3)',
             'SDC_QDSD_PINGROUP(sdc2_data, 0xd6000, 9, 0)'):
    assert fact in text, 'SM8550 hardware layout changed: ' + fact
wake_section = text.split('sm8550_pdc_map[] = {', 1)[1].split('};', 1)[0]
pairs = [(int(a), int(b)) for a, b in re.findall(r'\{\s*(\d+),\s*(\d+)\s*\}', wake_section)]
wake = dict(pairs)
assert len(pairs) == len(wake) == 93 and len(set(wake.values())) == 93
assert all(pin < 210 and pdc < 143 for pin, pdc in pairs)
generated = '''/** SM8550 TLMM facts extracted by Tools/generate_gpio.py.
 * Public Linux a53261b60834ce617cc4f3c7723dda8285c59789, pinctrl-sm8550.c.
 * Strong-I2C pull bit13 remains outside the shared GPIO schema.
 * SPDX-License-Identifier: MIT
 */
#include <Library/CrTargetGpioLib.h>

STATIC GpioDeviceContext mGpio = {
  .TlmmBaseAddress = 0x0f100000,
  .PinCount = 214,
  .InterruptConfig = {.InterruptNumber = GIC_SPI (208),
                      .TriggerType = CR_INTERRUPT_TRIGGER_LEVEL_HIGH},
  .EgpioFunction = GPIO_FUNC_9,
  .Tiles = {[GPIO_TLMM_TILE_DEFAULT] = {0}},
  .RegCfg = {
    {.Index=0, .ControlRegOffset=0, .IoRegOffset=4, .IntCfgRegOffset=8,
     .IntStatusRegOffset=12, .IntTargetRegOffset=8, .PullMsk=3, .MuxSelMsk=0x3c,
     .DriveStrengthMsk=0x1c0, .OutputEnableMsk=0x200,
     .EgpioPresentMsk=0x800, .EgpioEnabledMsk=0x1000, .InputMsk=1, .OutputMsk=2,
     .InterruptEnMsk=1, .InterruptPolarityMsk=2, .InterruptDetectionMsk=0xc,
     .InterruptRawStatusMsk=0x10, .InterruptStatusMsk=1, .InterruptTargetMsk=0xe0,
     .TargetKpssVal=3, .InterruptDetectionWidth=2},
    {.Index=1, .ControlRegOffset=0xde000, .IoRegOffset=0xde004,
     .PullMsk=0x18, .DriveStrengthMsk=7, .OutputMsk=1},
    {.Index=2, .ControlRegOffset=0xd6000, .PullMsk=0xc000, .DriveStrengthMsk=0x1c0},
    {.Index=3, .ControlRegOffset=0xd6000, .PullMsk=0x1800, .DriveStrengthMsk=0x38},
    {.Index=4, .ControlRegOffset=0xd6000, .PullMsk=0x600, .DriveStrengthMsk=7},
  },
  .GpioPins = {
'''
for index, _, functions in groups:
    pin = int(index)
    names = [name.strip() for name in functions.split(',')]
    assert len(names) == 9
    mux = 1
    for bit, name in enumerate(names, 1):
        if name != '_':
            mux |= 1 << bit
    # Function9 selects remote EGPIO ownership when bit11 advertises it.
    # The shared core treats this selector as ownership, not a local mux.
    mux |= 1 << 9
    pdc = str(wake[pin]) if pin in wake else 'GPIO_PDC_PIN_NUMBER_INVALID'
    generated += (f'    [{pin}] = {{.WakeBit=MAX_UINT8, .PdcPinNumber={pdc}, '
                  f'.WakeRegOffset=MAX_UINT32, .pRegCfg=&mGpio.RegCfg[0], '
                  f'.pTileInfo=&mGpio.Tiles[0], .FuncMuxMap=0x{mux:x}}},\n')
for pin, config in ((210, 1), (211, 2), (212, 3), (213, 4)):
    generated += (f'    [{pin}] = {{.WakeBit=MAX_UINT8, '
                  f'.PdcPinNumber=GPIO_PDC_PIN_NUMBER_INVALID, .WakeRegOffset=MAX_UINT32, '
                  f'.pRegCfg=&mGpio.RegCfg[{config}], .pTileInfo=&mGpio.Tiles[0], .FuncMuxMap=1}},\n')
generated += '  },\n};\n\nGpioDeviceContext *CrTargetGetGpioContext (VOID) { return &mGpio; }\n'
output = Path(__file__).resolve().parents[1] / 'CrGpioTarget.c'
if args.check:
    assert output.read_text() == generated, 'SM8550 GPIO facts drifted'
else:
    output.write_text(generated)
if args.receipt:
    args.receipt.parent.mkdir(parents=True, exist_ok=True)
    args.receipt.write_text(json.dumps({'linux_revision': revision,
        'source': relative, 'source_sha256': hashlib.sha256(data).hexdigest(),
        'target_sha256': hashlib.sha256(output.read_bytes()).hexdigest(),
        'normal_pads': 210, 'special_pads': 4, 'wake_routes': len(wake)}, indent=2) + '\n')
print('SM8550 Linux parity: 210 normal pads, UFS reset, 3 SDC pads, 93 wake routes')
