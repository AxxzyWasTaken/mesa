#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Package staged Mesa libraries into a relocatable, hashed overlay."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--stage', type=Path, required=True, help='DESTDIR or extracted candidate RPM root')
p.add_argument('--commit', required=True)
p.add_argument('--output', type=Path, required=True)
p.add_argument('--architecture', choices=['AArch64', 'Advanced Micro Devices X86-64'], required=True)
p.add_argument('--source', type=Path, default=Path(__file__).resolve().parents[4],
               help='matching public Mesa source tree, for license notices')
a = p.parse_args()
assert re.fullmatch('[0-9a-f]{40}', a.commit), 'full source commit required'
stage = a.stage.resolve(strict=True)
source = a.source.resolve(strict=True)
assert (source / 'docs/license.rst').is_file() and (source / 'licenses').is_dir(), 'Mesa license notices required'
assert not a.output.exists(), 'output already exists; preserve previous evidence'
choices = [stage / d for d in ['usr/lib64', 'usr/lib', 'lib64', 'lib']]
choices = [d for d in choices if (d / 'libEGL_mesa.so.0').is_file() or (d / 'libEGL_mesa.so.0').is_symlink()]
assert len(choices) == 1, 'ambiguous/missing staged Mesa'
src = choices[0]
out = a.output / 'usr/lib64'
out.mkdir(parents=True)
required = ['libEGL_mesa.so.0', 'libGLX_mesa.so.0', 'libgbm.so.1', 'libvulkan_asahi.so']
gallium = list(src.glob('libgallium-*.so'))
if not gallium:
    gallium = list((src / 'dri').glob('libgallium-*.so'))
assert len(gallium) == 1, 'missing/ambiguous Gallium'
files = [(src / name, Path()) for name in required] + [(gallium[0], Path()), (src / 'gbm/dri_gbm.so', Path('gbm'))]
seen = set()
manifest = dict(source_commit=a.commit, architecture=a.architecture, files=[])
for alias, subdir in files:
    # RPMs may use /usr/lib64 absolute links. Resolve those against DESTDIR.
    real = alias
    for _ in range(16):
        if not real.is_symlink():
            break
        link = real.readlink()
        real = stage / str(link).lstrip('/') if link.is_absolute() else real.parent / link
    real = real.resolve(strict=True)
    assert real.is_relative_to(stage), ('library escapes stage', alias)
    name = real.name
    dest = out / subdir
    dest.mkdir(exist_ok=True)
    key = str(subdir / name)
    if key not in seen:
        data = real.read_bytes()
        if alias in gallium or alias.name == 'libvulkan_asahi.so':
            assert ('git-' + a.commit[:10]).encode() in data, ('source identity', alias)
        header = subprocess.check_output(['readelf', '-h', str(real)], text=True)
        assert re.search(r'Machine:\s*' + re.escape(a.architecture) + r'\s*$', header, re.M), header
        dynamic = subprocess.check_output(['readelf', '-d', str(real)], text=True)
        assert not re.search(r'NEEDED.*(libLLVM|libSPIRV-Tools)', dynamic), ('compiler runtime dependency', alias)
        shutil.copy2(real, dest / name)
        manifest['files'].append(dict(path='usr/lib64/' + key,
             sha256=hashlib.sha256(data).hexdigest(), dynamic=dynamic))
        seen.add(key)
    if alias.name != name:
        (dest / alias.name).symlink_to(name)
(out / 'dri').mkdir()
for driver in ['asahi', 'zink']:
    (out / 'dri' / (driver + '_dri.so')).symlink_to('../' + gallium[0].name)
shutil.copytree(source / 'licenses', a.output / 'licenses')
shutil.copy2(source / 'docs/license.rst', a.output / 'LICENSE.rst')
for path in sorted((a.output / 'licenses').rglob('*')) + [a.output / 'LICENSE.rst']:
    if path.is_file():
        manifest['files'].append(dict(path=str(path.relative_to(a.output)),
             sha256=hashlib.sha256(path.read_bytes()).hexdigest()))
(a.output / 'MANIFEST.json').write_text(json.dumps(manifest, indent=2) + '\n')
