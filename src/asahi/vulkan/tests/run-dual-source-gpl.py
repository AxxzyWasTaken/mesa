#!/usr/bin/env python3
# Copyright 2026 Aurora Silicon
# SPDX-License-Identifier: MIT
"""Compile the CTS shaders and record exclusively through the no-op DRM shim."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    binary, driver, shim, glslang = (Path(arg).resolve() for arg in sys.argv[1:])
    source = Path(__file__).resolve().parent
    with tempfile.TemporaryDirectory(prefix="hk-dual-source-") as tmp:
        work = Path(tmp)
        for name in ("common.vert", "generic.frag", "dual.frag"):
            subprocess.run([
                str(glslang), "-V", "--target-env", "vulkan1.0",
                str(source / ("hk_dual_source_" + name)),
                "-o", str(work / (name + ".spv")),
            ], check=True)
        icd = work / "icd.json"
        icd.write_text(json.dumps({
            "file_format_version": "1.0.1",
            "ICD": {"library_path": str(driver), "api_version": "1.4.350"},
        }))
        env = os.environ.copy()
        for name in ("VK_ICD_FILENAMES", "VK_ADD_DRIVER_FILES",
                     "VK_LOADER_DRIVERS_SELECT", "VK_LOADER_DRIVERS_DISABLE"):
            env.pop(name, None)
        env.update(ASAHI_SHIM_CHIP="g13g", LD_PRELOAD=str(shim),
                   VK_DRIVER_FILES=str(icd), MESA_SHADER_CACHE_DISABLE="true")
        subprocess.run([str(binary), str(work)], env=env, check=True)


if __name__ == "__main__":
    main()
