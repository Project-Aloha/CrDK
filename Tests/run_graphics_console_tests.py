#!/usr/bin/env python3
"""Exercise production GOP selection and delayed SWM/OSK binding."""
from pathlib import Path
import subprocess
import tempfile

crane = Path(__file__).resolve().parents[1]
root = crane.parents[1]
graphics = crane / "Override/MU_PLUS/MsGraphicsPkg"
graphics_headers = root / "Common/MU/MsGraphicsPkg/Include"


def extract(source, signature):
    start = source.index(signature)
    end = source.index("\n}\n", start) + 3
    return source[start:end]


with tempfile.TemporaryDirectory(prefix="crane-graphics-console-") as tmp:
    directory = Path(tmp)
    window = (graphics / "SimpleWindowManagerDxe/WindowManager.c").read_text()
    directory.joinpath("WindowInit.inc").write_text("\n".join(
        extract(window, signature) for signature in (
            "STATIC EFI_STATUS\nLocateRenderingDisplay (",
            "VOID\nEFIAPI\nGopRegisteredCallback (",
            "EFI_STATUS\nEFIAPI\nDriverInit (")))
    keyboard = (graphics / "OnScreenKeyboardDxe/OnScreenKeyboardDriver.c").read_text()
    directory.joinpath("KeyboardBind.inc").write_text("\n".join(
        extract(keyboard, signature) for signature in (
            "STATIC EFI_STATUS\nOSKLocateRenderingGop (",
            "STATIC VOID\nEFIAPI\nOSKWindowManagerReady (",
            "STATIC EFI_STATUS\nOSKRegisterWindowManagerNotify (",
            "EFI_STATUS\nEFIAPI\nOSKDriverBindingStart (")))
    executable = directory / "graphics-console-test"
    includes = [root / "MU_BASECORE/MdePkg/Include",
                root / "MU_BASECORE/MdePkg/Include/X64",
                graphics_headers, directory]
    subprocess.run([
        "clang", "-std=c11", "-fshort-wchar", "-g", "-O1", "-Wall", "-Wextra",
        "-Werror", "-Wno-unused-parameter", "-Wno-unused-function",
        "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
        "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
        *["-I" + str(path) for path in includes],
        str(crane / "Tests/GraphicsConsoleTest.c"), "-o", str(executable),
    ], check=True)
    subprocess.run([str(executable)], check=True)
