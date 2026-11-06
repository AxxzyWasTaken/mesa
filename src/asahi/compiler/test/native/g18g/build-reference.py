#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Compile identical probe inputs for G17G and G18G with Apple's toolchain.

Two corpora are compiled for applegpu_g17g and applegpu_g18g:

  probe   the M5 instruction probes in ../g17g (<probe>.metal, .mtlp-json),
          one text per compute function, or per vertex and fragment
          function of a render pipeline
  oracle  the native Metal state oracle in oracle/ (oracle.metal plus one
          pipeline script per mode), one text per vertex, fragment or
          compute stage; these are the pipelines captured on the J873

Each compiled stage is split into its raw __TEXT,__text (the
_agc.main.constant_program at offset 0 followed by _agc.main) and written as
<corpus>.<name>.<stage-or-function>.<g17g|g18g>.bin.

  build-reference.py --output DIR            compile into a new DIR
  build-reference.py --output DIR --check    ... and compare with the tree
  build-reference.py --output DIR --update   ... and replace the tree copy
  build-reference.py --verify                check the committed texts only
  build-reference.py --verify --device-session SESSION ...
                                             also require the committed G18G
                                             oracle texts to equal the ones
                                             the J873 compiled itself

This creates native reference binaries, not Mesa runtime qualification.
Generated binaries and compiler logs belong outside the source checkout; only
the extracted texts and manifest.json are kept in the tree.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys

HERE = Path(__file__).resolve().parent
PROBES = HERE.parent/'g17g'
ORACLE = HERE/'oracle'
MANIFEST = HERE/'manifest.json'
ARCHES = {'applegpu_g17g': 'g17g', 'applegpu_g18g': 'g18g'}
STAGES = ('__vertex', '__fragment', '__compute')

# Mach-O constants for the MH_GPU_EXECUTE outer image and the MH_OBJECT
# nested in each stage section.
MH_MAGIC_64 = 0xfeedfacf
LC_SEGMENT_64 = 0x19
LC_SYMTAB = 0x2
GPU_FAT_MAGIC = 0xcbfebabe
CPU_APPLEGPU = 0x01000013
CPU_SUBTYPE_G18G = 451


def digest(data):
    return hashlib.sha256(data).hexdigest()


def cstr(raw):
    return raw.split(b'\0', 1)[0].decode('ascii', 'replace')


def macho(blob):
    """Sections and symbols of a little-endian 64-bit Mach-O."""
    magic, _cpu, _sub, _type, ncmds = struct.unpack_from('<IiiII', blob, 0)
    if magic != MH_MAGIC_64:
        raise ValueError(f'not a 64-bit Mach-O (magic {magic:#x})')
    sections, symbols, off = [], [], 32
    for _ in range(ncmds):
        cmd, size = struct.unpack_from('<II', blob, off)
        if cmd == LC_SEGMENT_64:
            for s in range(struct.unpack_from('<I', blob, off + 64)[0]):
                base = off + 72 + 80*s
                addr, ssize, soff = struct.unpack_from('<QQI', blob, base + 32)
                sections.append(dict(section=cstr(blob[base:base + 16]),
                                     segment=cstr(blob[base + 16:base + 32]),
                                     addr=addr, size=ssize, offset=soff))
        elif cmd == LC_SYMTAB:
            symoff, nsyms, stroff = struct.unpack_from('<III', blob, off + 8)
            for i in range(nsyms):
                strx, _t, nsect, _d, value = struct.unpack_from('<IBBHQ', blob, symoff + 16*i)
                symbols.append(dict(name=cstr(blob[stroff + strx:stroff + strx + 256]),
                                    section=nsect, value=value))
        off += size
    return sections, symbols


def stage_objects(blob, stage):
    """The MH_OBJECTs inside one outer stage section, in section order.

    A compute pipeline script with several functions yields one object per
    function, concatenated in the order of the script's compute_pipelines."""
    sections, _ = macho(blob)
    sect = next((s for s in sections
                 if s['segment'] == '__TEXT' and s['section'] == stage), None)
    if sect is None:
        return []
    data = blob[sect['offset']:sect['offset'] + sect['size']]
    starts = []
    for i in range(0, len(data) - 16, 4):
        magic, cpu, _sub, filetype = struct.unpack_from('<IiiI', data, i)
        if magic == MH_MAGIC_64 and cpu == CPU_APPLEGPU and filetype == 1:
            starts.append(i)
    return [data[a:b] for a, b in zip(starts, starts[1:] + [len(data)])]


def text_of(obj):
    """(__TEXT,__text bytes, {symbol: offset}) of one stage object."""
    sections, symbols = macho(obj)
    index, text = next((i + 1, s) for i, s in enumerate(sections)
                       if s['segment'] == '__TEXT' and s['section'] == '__text')
    code = obj[text['offset']:text['offset'] + text['size']]
    syms = {s['name']: s['value'] - text['addr'] for s in symbols if s['section'] == index}
    if syms.get('_agc.main.constant_program') != 0 or '_agc.main' not in syms:
        raise ValueError(f'unexpected text symbols {syms}')
    return code, dict(sorted(syms.items(), key=lambda kv: kv[1]))


def texts_of_gpubin(blob):
    """{stage: [(code, symbols), ...]} for every stage section of an image."""
    return {stage.strip('_'): [text_of(o) for o in stage_objects(blob, stage)]
            for stage in STAGES}


def g18g_slice(archive):
    """The applegpu_g18g slice of a serialized MTLBinaryArchive."""
    if struct.unpack_from('>I', archive, 0)[0] != GPU_FAT_MAGIC:
        raise ValueError('not an Apple GPU fat container')
    for i in range(struct.unpack_from('>I', archive, 4)[0]):
        cpu, sub, off, size = struct.unpack_from('>iiII', archive, 8 + 20*i)
        if cpu == CPU_APPLEGPU and sub == CPU_SUBTYPE_G18G:
            return archive[off:off + size]
    raise ValueError('no applegpu_g18g slice')


def text_name(corpus, name, part, arch):
    return f'{corpus}.{name}.{part}.{ARCHES[arch]}.bin'


def rel(path):
    return os.path.relpath(path, HERE)


def run(argv, cwd, log):
    result = subprocess.run(argv, cwd=cwd, text=True, capture_output=True)
    log.write_text(result.stdout + result.stderr)
    if result.returncode:
        raise RuntimeError(f'{argv[1]} failed with {result.returncode}: {log}')
    return result.stdout


def toolchain(out):
    metal = run(['xcrun', 'metal', '--version'], out, out/'metal-version.txt')
    asset = re.search(r'MetalToolchain-v([0-9.]+?)\.[A-Za-z0-9]+/', metal)
    nt = run(['xcrun', 'metal-nt', '--version'], out, out/'metal-nt-version.txt')
    os_ver = run(['sw_vers'], out, out/'host-os.txt')
    fields = dict(re.findall(r'^(\w+):\s*(.*)$', os_ver, re.M))
    return {
        'metal': metal.splitlines()[0].strip(),
        'metal_nt': nt.splitlines()[0].strip(),
        'metal_toolchain': asset.group(1) if asset else None,
        'host_os': f"{fields.get('ProductName')} {fields.get('ProductVersion')} "
                   f"({fields.get('BuildVersion')})",
    }


def compile_probes(names, out, texts):
    report = []
    for name in names:
        source, config = PROBES/(name + '.metal'), PROBES/(name + '.mtlp-json')
        assert source.is_file() and config.is_file() and Path(name).name == name
        pipelines = json.loads(config.read_text())['pipelines']
        functions = [p['compute_function'] for p in
                     pipelines.get('compute_pipelines', [])]
        # A render probe names one vertex and one fragment function per
        # pipeline; each stage section holds one object per pipeline.
        render = {'vertex': [p['vertex_function'] for p in
                             pipelines.get('render_pipelines', [])],
                  'fragment': [p['fragment_function'] for p in
                               pipelines.get('render_pipelines', [])]}
        folder = out/'probe'/name
        folder.mkdir(parents=True)
        air, lib = folder/'probe.air', folder/'probe.metallib'
        run(['xcrun', 'metal', '-std=metal3.0', '-mmacosx-version-min=27.0',
             '-c', str(source), '-o', str(air)], folder, folder/'metal.log')
        run(['xcrun', 'metallib', str(air), '-o', str(lib)], folder, folder/'metallib.log')
        probe = dict(name=name, source_sha256=digest(source.read_bytes()),
                     config_sha256=digest(config.read_bytes()),
                     air_sha256=digest(air.read_bytes()),
                     metallib_sha256=digest(lib.read_bytes()), targets={})
        for arch in ARCHES:
            target = folder/arch
            target.mkdir()
            binary = target/'probe.gpubin'
            command = ['xcrun', 'metal-nt', '-arch', arch, '-platform_version', 'macos',
                       '27.0', '27.0', '-N', str(config), str(lib), '-save-temps',
                       '-o', str(binary)]
            run(command, target, target/'metal-nt.log')
            blob = binary.read_bytes()
            assert blob
            probe['targets'][arch] = dict(sha256=digest(blob), size=len(blob),
                                          command=command,
                                          binary=str(binary.relative_to(out)))
            for stage, names in (('compute', functions), *render.items()):
                if not names:
                    continue
                objects = stage_objects(blob, '__' + stage)
                assert len(objects) == len(names), (name, arch, stage, len(objects))
                for function, obj in zip(names, objects):
                    # Each object names its own function (NUL-terminated) and
                    # no other, which confirms the pipeline-script order.
                    named = [f for f in names if f.encode() + b'\0' in obj]
                    assert named == [function], (name, arch, function, named)
                    code, syms = text_of(obj)
                    texts[text_name('probe', name, function, arch)] = dict(
                        corpus='probe', source=rel(source), config=rel(config),
                        function=function, stage=stage, arch=arch,
                        symbols=syms, code=code)
        report.append(probe)
        print('COMPILED probe', name, 'G17G and G18G', flush=True)
    return report


def compile_oracle(modes, out, texts):
    folder = out/'oracle'
    folder.mkdir()
    source = ORACLE/'oracle.metal'
    air, lib = folder/'oracle.air', folder/'oracle.metallib'
    # The recipe of the native capture oracle's build.sh (kept outside this tree).
    run(['xcrun', '--sdk', 'macosx', 'metal', '-std=metal3.1', '-mmacosx-version-min=14.0',
         '-c', str(source), '-o', str(air)], folder, folder/'metal.log')
    run(['xcrun', '--sdk', 'macosx', 'metallib', str(air), '-o', str(lib)],
        folder, folder/'metallib.log')
    lib_sha = digest(lib.read_bytes())
    report = dict(source_sha256=digest(source.read_bytes()), metallib_sha256=lib_sha,
                  modes=[])
    for mode in modes:
        config = ORACLE/(mode + '.mtlp-json')
        assert config.is_file() and Path(mode).name == mode
        script = json.loads(config.read_text())
        labels = [p['label'].lower() for p in script['libraries']['paths']]
        # The pipeline script names its library by hash; a different
        # metallib is a different oracle.
        label_ok = labels == [lib_sha]
        row = dict(mode=mode, config_sha256=digest(config.read_bytes()),
                   library_label_matches=label_ok, targets={})
        for arch in ARCHES:
            target = folder/mode/arch
            target.mkdir(parents=True)
            # metal-nt records the library path it is given, so compile from
            # a private copy by relative path, as extract_gpu_code.py does.
            shutil.copyfile(lib, target/'oracle.metallib')
            shutil.copyfile(config, target/'pipeline.mtlp-json')
            command = ['xcrun', 'metal-nt', '-arch', arch, '-platform_version', 'macos',
                       '27.0', '27.0', '-N', 'pipeline.mtlp-json', 'oracle.metallib',
                       '-o', 'pipeline.gpubin']
            run(command, target, target/'metal-nt.log')
            blob = (target/'pipeline.gpubin').read_bytes()
            row['targets'][arch] = dict(sha256=digest(blob), size=len(blob), command=command)
            for stage, objects in texts_of_gpubin(blob).items():
                if not objects:
                    continue
                assert len(objects) == 1, (mode, arch, stage, len(objects))
                code, syms = objects[0]
                texts[text_name('oracle', mode, stage, arch)] = dict(
                    corpus='oracle', source=rel(source), config=rel(config),
                    mode=mode, stage=stage, arch=arch, symbols=syms, code=code)
        report['modes'].append(row)
        print('COMPILED oracle', mode, 'G17G and G18G', flush=True)
    return report


def text_manifest(texts, tools, witnesses):
    entries = {}
    for name, t in sorted(texts.items()):
        entry = {k: v for k, v in t.items() if k != 'code'}
        entry.update(size=len(t['code']), sha256=digest(t['code']))
        if name in witnesses:
            entry['device_witness'] = witnesses[name]
        entries[name] = entry
    return dict(scope='Apple compiler references only; no GPU execution',
                hardware_execution=False, mesa_execution=False,
                toolchain=tools, texts=entries)


def device_witnesses(sessions, texts):
    """Compare the committed G18G oracle texts with the ones the J873's own
    driver compiled into each captured mode's MTLBinaryArchive."""
    found, bad = {}, []
    for session in sessions:
        label = '/'.join(Path(session).resolve().parts[-2:])
        for archive in sorted(Path(session).glob('*/oracle/*.binarchive')):
            mode = archive.stem
            if archive.parent.parent.name != mode:
                continue
            blob = g18g_slice(archive.read_bytes())
            for stage, objects in texts_of_gpubin(blob).items():
                for code, _syms in objects:
                    name = text_name('oracle', mode, stage, 'applegpu_g18g')
                    if name not in texts or texts[name]['sha256'] != digest(code):
                        bad.append(f'{label} {name}')
                    else:
                        found.setdefault(name, []).append(label)
    return found, bad


def load_committed():
    manifest = json.loads(MANIFEST.read_text())
    errors = []
    for name, entry in manifest['texts'].items():
        data = (HERE/name).read_bytes() if (HERE/name).is_file() else None
        if data is None:
            errors.append(f'{name}: missing')
        elif len(data) != entry['size'] or digest(data) != entry['sha256']:
            errors.append(f'{name}: does not match manifest.json')
    listed = set(manifest['texts'])
    for path in HERE.glob('*.bin'):
        if path.name not in listed:
            errors.append(f'{path.name}: not in manifest.json')
    return manifest, errors


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--output', type=Path, help='new directory to compile into')
    parser.add_argument('--probe', action='append', help='Existing g17g reference basename')
    parser.add_argument('--mode', action='append', help='Oracle mode (oracle/<mode>.mtlp-json)')
    parser.add_argument('--check', action='store_true',
                        help='fail unless the compiled texts equal the committed ones')
    parser.add_argument('--update', action='store_true',
                        help='replace the committed texts and manifest.json')
    parser.add_argument('--verify', action='store_true',
                        help='check the committed texts against manifest.json only')
    parser.add_argument('--device-session', action='append', default=[],
                        help='capture session whose <mode>/oracle/<mode>.binarchive '
                             'holds the J873 device-compiled G18G code')
    args = parser.parse_args()

    if args.verify:
        manifest, errors = load_committed()
        _, bad = device_witnesses(args.device_session, manifest['texts'])
        errors += [f'{b}: device text differs' for b in bad]
        for e in errors:
            print('MISMATCH', e)
        print('VERIFIED' if not errors else 'FAILED', len(manifest['texts']), 'texts',
              f'({len(args.device_session)} device sessions)')
        sys.exit(1 if errors else 0)

    if args.output is None:
        parser.error('--output is required unless --verify is given')
    everything = not args.probe and not args.mode
    probes = args.probe or ([p.stem for p in sorted(PROBES.glob('*.mtlp-json'))]
                            if everything else [])
    modes = args.mode or ([p.stem for p in sorted(ORACLE.glob('*.mtlp-json'))]
                          if everything else [])
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    report = dict(scope='Apple compiler references only; no GPU execution',
                  hardware_execution=False, mesa_execution=False)
    texts = {}
    try:
        report['toolchain'] = toolchain(out)
        assert {'applegpu_g17g', 'applegpu_g18g'} <= set(
            run(['xcrun', 'metal-nt', '-archs'], out, out/'architectures.txt').split())
        report['probes'] = compile_probes(probes, out, texts)
        report['oracle'] = compile_oracle(modes, out, texts)
    finally:
        (out/'manifest.json').write_text(json.dumps(report, indent=2) + '\n')

    text_dir = out/'text'
    text_dir.mkdir()
    for name, t in texts.items():
        (text_dir/name).write_bytes(t['code'])
    hashed = {n: dict(t, sha256=digest(t['code'])) for n, t in texts.items()}
    witnesses, bad = device_witnesses(args.device_session, hashed)
    manifest = text_manifest(texts, report['toolchain'], witnesses)
    (text_dir/'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print('REFERENCE_MANIFEST', out/'manifest.json', flush=True)

    errors = [f'{b}: device text differs' for b in bad]
    errors += [f"oracle library label mismatch ({m['mode']})"
               for m in report['oracle']['modes'] if not m['library_label_matches']]
    if args.check:
        committed, problems = load_committed()
        errors += problems
        for name, entry in committed['texts'].items():
            if name in texts and digest(texts[name]['code']) != entry['sha256']:
                errors.append(f'{name}: compiled text differs from the tree')
        if everything and set(committed['texts']) != set(texts):
            errors.append('compiled set differs from the committed set')
    if args.update and not errors:
        if not everything:
            sys.exit('--update needs the full corpus (no --probe/--mode)')
        for old in HERE.glob('*.bin'):
            old.unlink()
        for name, t in texts.items():
            (HERE/name).write_bytes(t['code'])
        MANIFEST.write_text(json.dumps(manifest, indent=2) + '\n')
    for e in errors:
        print('MISMATCH', e)
    if errors:
        sys.exit(1)
    print('TEXTS', len(texts), 'CHECKED' if args.check else '', flush=True)


if __name__ == '__main__':
    main()
