"""Extract pinned inputs and package corresponding sources without binary test data."""
import json
import hashlib
import copy
import re
import shutil
import sys
import zipfile
from pathlib import Path

operation, root_arg = sys.argv[1:]
root = Path(root_arg).resolve()
deps = root / '.deps'
sources = deps / 'minimal-sources'
lock = root / 'third_party/ffmpeg/dependency.json'
manifest = json.loads(lock.read_text(encoding='utf-8-sig'))

if operation in ('install-headers', 'package'):
    # Native Make's long install command exceeds Windows command-line limits
    # for libavutil. Copy exactly the public/build-generated header lists.
    ffmpeg = sources / manifest['sources'][0]['directory']
    build = deps / 'ffmpeg-minimal-build'
    sdk = deps / 'ffmpeg-minimal-sdk'
    for component in ('avcodec', 'avformat', 'avutil', 'swscale', 'swresample'):
        subdir = 'lib' + component
        makefile = (ffmpeg / subdir / 'Makefile').read_text().replace('\\\n', ' ')
        destination = sdk / 'include' / subdir
        destination.mkdir(parents=True, exist_ok=True)
        for variable in ('HEADERS', 'BUILT_HEADERS'):
            match = re.search(r'^' + variable + r'\s*=\s*(.*)$', makefile, re.MULTILINE)
            if match:
                origin = build if variable == 'BUILT_HEADERS' else ffmpeg
                for name in match.group(1).split():
                    shutil.copy2(origin / subdir / name, destination / name)
        shutil.copy2(sdk / 'bin' / (component + '.lib'), sdk / 'lib' / (component + '.lib'))


def included(name, source):
    parts = Path(name).parts
    if source['name'] == 'amf.zip':
        # Only these MIT SDK headers are compiled into FFmpeg.
        return '/amf/public/include/' in name or name.endswith('/LICENSE.txt')
    if source['name'] == 'openh264-source.zip':
        return len(parts) < 2 or parts[1] != 'res'
    return True


if operation == 'install-headers':
    pass
elif operation == 'extract-toolchain':
    with zipfile.ZipFile(deps / 'w64devkit.zip') as archive:
        archive.extractall(deps / 'toolchain')
elif operation == 'extract':
    for source in manifest['sources']:
        marker = sources / source['directory'] / '.kirakara-extracted'
        if marker.exists() and marker.read_text(encoding='ascii') == source['sha256']:
            continue
        with zipfile.ZipFile(sources / source['name']) as archive:
            for item in archive.infolist():
                if included(item.filename, source):
                    archive.extract(item, sources)
        marker.write_text(source['sha256'], encoding='ascii')
elif operation == 'package':
    notices = deps / 'ffmpeg-minimal-sdk/third_party/ffmpeg'
    output = notices / 'sources'
    output.mkdir(parents=True, exist_ok=True)
    shutil.copy2(lock, notices)
    shutil.copy2(root / 'third_party/ffmpeg/README.md', notices)
    shutil.copy2(deps / 'GCC-COPYING.RUNTIME', notices)
    shutil.copy2(deps / 'toolchain/w64devkit/COPYING.MinGW-w64-runtime.txt', notices)
    for name in ('build_ffmpeg_minimal.sh', 'prepare_ffmpeg.ps1', 'ffmpeg_source_materials.py'):
        shutil.copy2(root / 'tools' / name, notices)
    shutil.copy2(deps / 'ffmpeg-minimal-build/config.h', notices)
    shutil.copy2(deps / 'ffmpeg-minimal-build/ffbuild/config.mak', notices)
    licenses = {
        'ffmpeg-source.zip': ['COPYING.LGPLv3', 'COPYING.GPLv3'],
        'openh264-source.zip': ['LICENSE'],
        'dav1d.zip': ['COPYING'],
        'amf.zip': ['LICENSE.txt'],
        'libvpl.zip': ['LICENSE'],
        'nv-codec-headers.zip': ['include/ffnvcodec/nvEncodeAPI.h'],
    }
    for source in manifest['sources']:
        with zipfile.ZipFile(sources / source['name']) as original:
            with zipfile.ZipFile(output / source['name'], 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as packed:
                for item in original.infolist():
                    if not item.is_dir() and included(item.filename, source):
                        packed.writestr(copy.copy(item), original.read(item), compress_type=zipfile.ZIP_DEFLATED, compresslevel=9)
            for name in licenses[source['name']]:
                data = original.read(f"{source['directory']}/{name}")
                (notices / (source['name'].replace('.zip', '') + '-' + Path(name).name)).write_bytes(data)
    (notices / 'source-hashes.json').write_text(json.dumps({
        file.name: hashlib.file_digest(file.open('rb'), 'sha256').hexdigest()
        for file in sorted(output.glob('*.zip'))
    }, indent=2), encoding='utf-8')
else:
    raise SystemExit(f'Unknown operation: {operation}')
