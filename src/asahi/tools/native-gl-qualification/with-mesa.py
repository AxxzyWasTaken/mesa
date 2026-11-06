#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Run one command with a relocatable Mesa overlay; never install it."""
import argparse
import json
import os
from pathlib import Path
import tempfile

p = argparse.ArgumentParser(description=__doc__)
p.add_argument("--overlay", type=Path, required=True)
p.add_argument("--driver", choices=("native", "zink"), required=True)
p.add_argument("--cache", choices=("off", "on"), default="off")
p.add_argument("command", nargs=argparse.REMAINDER)
a = p.parse_args()
root = a.overlay.resolve(strict=True)
libs = [root / d for d in ("usr/lib64", "usr/lib", "lib64", "lib")]
libs = [d for d in libs if (d / "libEGL_mesa.so.0").is_file()]
if len(libs) != 1:
    p.error("overlay must have exactly one Mesa library directory")
lib = libs[0]
for name in ("libEGL_mesa.so.0", "libGLX_mesa.so.0", "libgbm.so.1",
             "libvulkan_asahi.so", "gbm/dri_gbm.so", "dri/asahi_dri.so", "dri/zink_dri.so"):
    path = lib / name
    if not path.is_file() or not path.resolve().is_relative_to(root):
        p.error(f"missing library or symlink escaping overlay: {path}")
state = root / "qualification-loader"
state.mkdir(exist_ok=True)
egl = state / "egl.json"
vk = state / "vk.json"
def loader_json(path, content):
    text = json.dumps(content) + "\n"
    # A greeter can reuse already-prepared, read-only loader manifests.
    if path.is_file() and path.read_text() == text:
        return
    with tempfile.NamedTemporaryFile(mode="w", dir=state, delete=False) as f:
        f.write(text)
    os.chmod(f.name, 0o644)
    os.replace(f.name, path)

loader_json(egl, {"file_format_version": "1.0.0", "ICD": {
    "library_path": str(lib / "libEGL_mesa.so.0")}})
loader_json(vk, {"file_format_version": "1.0.0", "ICD": {
    "library_path": str(lib / "libvulkan_asahi.so"), "api_version": "1.4.0"}})
env = dict(os.environ)
# Exclude selectors, injected layers and debugging paths from the comparison.
for name in ("MESA_LOADER_DRIVER_OVERRIDE", "GALLIUM_DRIVER", "LD_PRELOAD",
             "LIBGL_ALWAYS_SOFTWARE", "LIBGL_ALWAYS_INDIRECT", "MESA_GL_VERSION_OVERRIDE",
             "MESA_GLES_VERSION_OVERRIDE", "MESA_EXTENSION_OVERRIDE",
             "VK_INSTANCE_LAYERS", "VK_LAYER_PATH", "VK_ADD_LAYER_PATH",
             "VK_LOADER_DRIVERS_SELECT", "VK_LOADER_DRIVERS_DISABLE",
             "ZINK_DEBUG", "ZINK_INLINE_UNIFORMS", "ZINK_DESCRIPTORS", "AGX_MESA_DEBUG"):
    env.pop(name, None)
env.update(MESA_SHADER_CACHE_DISABLE="true" if a.cache == "off" else "false",
           LD_LIBRARY_PATH=str(lib), LIBGL_DRIVERS_PATH=str(lib / "dri"),
           GBM_BACKENDS_PATH=str(lib / "gbm"), __GLX_VENDOR_LIBRARY_NAME="mesa",
           __EGL_VENDOR_LIBRARY_FILENAMES=str(egl),
           VK_DRIVER_FILES=str(vk), VK_ICD_FILENAMES=str(vk))
if a.driver == "zink":
    env.update(MESA_LOADER_DRIVER_OVERRIDE="zink", GALLIUM_DRIVER="zink")
cmd = a.command
if cmd and cmd[0] == "--":
    cmd = cmd[1:]
if not cmd:
    p.error("a command is required after --")
os.execvpe(cmd[0], cmd, env)
