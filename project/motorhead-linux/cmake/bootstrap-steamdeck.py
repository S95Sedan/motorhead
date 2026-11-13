"""Download a pinned cross compiler and Debian X11/audio development files."""
from pathlib import Path
import gzip
import hashlib
import io
import json
import shutil
import tarfile
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parents[3]
TOOLS = ROOT / '.tools/steamdeck-native'


def download(url, path, sha=None):
    if not path.exists():
        print(f'Downloading {url}', flush=True)
        with urllib.request.urlopen(url, timeout=120) as src, path.open('wb') as dst:
            shutil.copyfileobj(src, dst)
    if sha and hashlib.sha256(path.read_bytes()).hexdigest() != sha:
        raise RuntimeError(f'Hash mismatch: {path}')


def deb_data(blob):
    offset = 8
    while offset + 60 <= len(blob):
        header = blob[offset:offset+60]
        size = int(header[48:58])
        name = header[:16].decode().strip().rstrip('/')
        if name.startswith('data.tar'):
            return blob[offset+60:offset+60+size]
        offset += 60 + size + size % 2
    raise RuntimeError('No data archive in Debian package')


def main():
    TOOLS.mkdir(parents=True, exist_ok=True)
    index = json.load(urllib.request.urlopen('https://ziglang.org/download/index.json'))
    zig = index['0.15.2']['x86_64-windows']
    archive = TOOLS / 'zig.zip'
    download(zig['tarball'], archive, zig['shasum'])
    zigroot = TOOLS / 'zig-x86_64-windows-0.15.2'
    if not (zigroot / 'zig.exe').exists():
        with zipfile.ZipFile(archive) as z:
            z.extractall(TOOLS)
    package_index = TOOLS / 'Packages.gz'
    mirror = 'https://archive.debian.org/debian/'
    download(mirror + 'dists/bullseye/main/binary-amd64/Packages.gz', package_index)
    wanted = {'libx11-dev','libx11-6','x11proto-dev','xtrans-dev','libxext-dev','libxext6',
              'libxcursor-dev','libxcursor1','libxfixes-dev','libxfixes3','libxrandr-dev',
              'libxrandr2','libxrender-dev','libxrender1','libxi-dev','libxi6',
              'libxss-dev','libxss1','libasound2-dev','libasound2','libpulse-dev','libpulse0',
              'libudev-dev','libudev1','libxkbcommon-dev','libxkbcommon0','libxcb1-dev','libxcb1',
              'libgl-dev','libgl1','libglx-dev','libglx0','libopengl-dev','libopengl0'}
    sysroot = TOOLS / 'sysroot'
    links = []
    records = []
    for paragraph in gzip.decompress(package_index.read_bytes()).decode().split('\n\n'):
        fields = dict(line.split(': ', 1) for line in paragraph.splitlines() if ': ' in line and not line.startswith(' '))
        if fields.get('Package') not in wanted or fields.get('Architecture') not in ('amd64', 'all'):
            continue
        wanted.remove(fields['Package'])
        dest = TOOLS / Path(fields['Filename']).name
        download(mirror + fields['Filename'], dest, fields['SHA256'])
        records.append({k:fields[k] for k in ('Package','Version','Filename','SHA256')})
        with tarfile.open(fileobj=io.BytesIO(deb_data(dest.read_bytes()))) as t:
            for member in t.getmembers():
                relative = Path(member.name.removeprefix('./'))
                if relative.is_absolute() or '..' in relative.parts:
                    raise RuntimeError(f'Unsafe package path: {relative}')
                target = sysroot / relative
                if member.isfile():
                    target.parent.mkdir(parents=True, exist_ok=True)
                    with t.extractfile(member) as src, target.open('wb') as dst:
                        shutil.copyfileobj(src, dst)
                elif member.issym() or member.islnk():
                    links.append((target, member.linkname, member.islnk()))
    if wanted:
        raise RuntimeError(f'Missing packages: {wanted}')
    # Materialize linker symlinks as copies on Windows (no admin rights needed).
    for _ in range(5):
        pending = []
        for target, link, hard in links:
            source = sysroot / link.lstrip('/') if hard or link.startswith('/') else target.parent / link
            if source.is_file():
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(source, target)
            elif not target.exists():
                pending.append((target,link,hard))
        links = pending
    (TOOLS / 'dependencies.json').write_text(json.dumps({'zig':zig,'debian':records}, indent=2))
    print(f'Native Linux cross compiler and headers ready: {TOOLS}', flush=True)


if __name__ == '__main__':
    main()
