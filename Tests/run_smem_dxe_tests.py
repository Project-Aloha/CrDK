#!/usr/bin/env python3
"""Link the production SMEM core and DXE protocol against hardware-lock mocks."""
from pathlib import Path
import subprocess
import tempfile
crane=Path(__file__).resolve().parents[1]
root=crane.parents[1]
includes=[crane/'Include',root/'Platforms/MUCommonCrPkg/Include',
          root/'MU_BASECORE/MdePkg/Include',root/'MU_BASECORE/MdePkg/Include/X64',
          root/'MU_BASECORE/ArmPkg/Include',root/'MU_BASECORE/ArmPlatformPkg/Include']
with tempfile.TemporaryDirectory(prefix='crane-smem-dxe-') as tmp:
    exe=str(Path(tmp)/'test')
    subprocess.run(['clang','-std=c11','-fshort-wchar','-g','-O1','-Wall','-Wextra','-Werror',
        '-fsanitize=address,undefined','-fno-sanitize-recover=all',
        *['-I'+str(p) for p in includes],str(crane/'Tests/SmemCrDxeTest.c'),
        str(crane/'Library/SmemLib/smem.c'),
        str(crane/'Library/SmemLib/smem_mutex.c'),'-o',exe],check=True)
    subprocess.run([exe],check=True)
