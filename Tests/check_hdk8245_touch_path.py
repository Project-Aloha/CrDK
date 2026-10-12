#!/usr/bin/env python3
"""Source qualification: HDK8245 must have a native FTS pointer implementation."""
from pathlib import Path

p = Path(__file__).resolve().parents[1]
driver = p / 'Driver/FocalTechTouchCrDxe/FocalTechTouchCrDxe.c'
assert driver.is_file(), 'HDK8245 active focaltech,fts_ts has no native AbsolutePointer driver'
text = driver.read_text()
assert 'gEfiAbsolutePointerProtocolGuid' in text
assert 'gEfiI2cCrProtocolGuid' in text
assert 'gEfiEventExitBootServicesGuid' in text
assert 'CrFtsStart' in text and 'CrFtsStop' in text
print('HDK8245 native FocalTech source path PASS')
