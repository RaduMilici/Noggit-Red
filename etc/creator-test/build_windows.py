#!/usr/bin/env python3
"""Prepare and package Windows Creator. Run via build-windows.cmd."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parents[2]
SSH_URL = 'https://github.com/PowerShell/Win32-OpenSSH/releases/download/10.0.0.0p2-Preview/OpenSSH-Win64.zip'
SSH_SHA256 = '23f50f3458c4c5d0b12217c6a5ddfde0137210a30fa870e98b29827f7b43aba5'


STEPS = 7
_step = 0


def step(title, location=None):
    # Numbered progress lines, so a long build shows where it is and where things go.
    global _step
    _step += 1
    print('\n' + '=' * 72 + f'\n[{_step}/{STEPS}] {title}', flush=True)
    if location:
        print('      ' + str(location), flush=True)


def version():
    # The release name: the tag on this commit ("v1.2.0"), else "<last tag>-<commits since>-g<hash>",
    # plus "-dirty" when there are uncommitted changes. Without any tag: the short commit hash.
    try:
        described = subprocess.check_output(['git', 'describe', '--tags', '--always', '--dirty'],
                                            cwd=ROOT, text=True).strip()
    except (OSError, subprocess.CalledProcessError):
        return 'untagged'
    return re.sub(r'[^A-Za-z0-9._-]', '-', described) or 'untagged'


def run(*command):
    print('\n>', ' '.join(str(x) for x in command), flush=True)
    subprocess.run([str(x) for x in command], check=True, cwd=ROOT)


def visual_studio():
    vswhere = Path(os.environ.get('ProgramFiles(x86)', 'C:/Program Files (x86)')) / 'Microsoft Visual Studio/Installer/vswhere.exe'
    if not vswhere.is_file():
        raise RuntimeError('Install Visual Studio 2019 or 2022 with Desktop development with C++ and the Windows SDK.')
    installations = json.loads(subprocess.check_output([
        str(vswhere), '-products', '*', '-requires', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64',
        '-format', 'json'], text=True, encoding='utf-8-sig'))
    for installation in installations:
        major = installation['installationVersion'].split('.')[0]
        if major in ('16', '17'):
            return Path(installation['installationPath']), {'16': 'Visual Studio 16 2019', '17': 'Visual Studio 17 2022'}[major]
    raise RuntimeError('No supported Visual Studio C++ toolchain found (2019 or 2022).')


def developer_environment(installation):
    # Import the compiler environment without requiring a special terminal.
    with tempfile.TemporaryDirectory(prefix='creator-vs-') as temporary:
        script = Path(temporary) / 'environment.cmd'
        script.write_text('@echo off\ncall "' + str(installation / 'Common7/Tools/VsDevCmd.bat')
                          + '" -arch=x64 -host_arch=x64 >nul\nif errorlevel 1 exit /b 1\nset\n')
        output = subprocess.check_output(['cmd.exe', '/d', '/c', str(script)], text=True, errors='replace')
    for line in output.splitlines():
        key, separator, value = line.partition('=')
        if separator and key and not key.startswith('='):
            os.environ[key] = value


def qt_directory(explicit):
    candidates = [explicit] if explicit else []
    candidates += [os.environ.get('QTDIR'), os.environ.get('Qt5_DIR')]
    candidates += sorted(Path('C:/Qt').glob('5.*/msvc*_64'), reverse=True)
    for candidate in candidates:
        if not candidate:
            continue
        path = Path(candidate)
        if (path / 'bin/qmake.exe').is_file() and (path / 'lib/cmake/Qt5Multimedia/Qt5MultimediaConfig.cmake').is_file():
            return path.resolve()
    if explicit:
        raise RuntimeError('The supplied Qt directory needs Qt5 x64 MSVC, including Multimedia: ' + explicit)
    value = input('Qt5 x64 MSVC folder (for example C:/Qt/5.15.2/msvc2019_64): ').strip().strip('"')
    if not value:
        raise RuntimeError('Qt5 path is required. Install Qt5 with Multimedia on the build machine.')
    return qt_directory(value)


def openssh(cache):
    destination = cache / 'OpenSSH-Win64'
    marker = destination / '.creator-sha256'
    if marker.is_file() and marker.read_text() == SSH_SHA256 and all(
            (destination / name).is_file() for name in ('ssh.exe', 'ssh-keyscan.exe')):
        return destination
    if destination.exists():
        raise RuntimeError('Unverified OpenSSH cache exists; rename it and retry: ' + str(destination))
    archive = cache / 'OpenSSH-Win64.zip'
    if not archive.exists():
        print('Downloading portable OpenSSH...', flush=True)
        partial = archive.with_suffix('.download')
        urllib.request.urlretrieve(SSH_URL, partial)
        if hashlib.sha256(partial.read_bytes()).hexdigest() != SSH_SHA256:
            raise RuntimeError('OpenSSH download checksum mismatch; retry the script.')
        partial.replace(archive)
    if hashlib.sha256(archive.read_bytes()).hexdigest() != SSH_SHA256:
        raise RuntimeError('OpenSSH cache checksum mismatch; remove ' + str(archive))
    with tempfile.TemporaryDirectory(prefix='openssh-', dir=cache) as temporary:
        with zipfile.ZipFile(archive) as package:
            package.extractall(temporary)
        source = Path(temporary) / 'OpenSSH-Win64'
        for name in ('ssh.exe', 'ssh-keyscan.exe'):
            if not (source / name).is_file():
                raise RuntimeError('Unexpected OpenSSH archive layout')
        (source / '.creator-sha256').write_text(SSH_SHA256)
        source.rename(destination)
    return destination


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--qt', help='Qt5 x64 MSVC installation directory; otherwise auto-detect or prompt')
    parser.add_argument('--manifest', type=Path, default=ROOT / 'etc/creator-test/game-data.json')
    parser.add_argument('--jobs', type=int, default=4)
    args = parser.parse_args()
    if sys.platform != 'win32':
        parser.error('Run this script on Windows.')
    if args.jobs < 1:
        parser.error('--jobs must be positive')
    os.chdir(ROOT)
    release = version()
    package_name = 'Noggit-Creator-' + release + '-Windows-x64'
    print('Building Noggit Creator', release, 'from', ROOT, flush=True)
    if release.endswith('-dirty'):
        print('WARNING: uncommitted changes are included, so the name ends in -dirty.', flush=True)
    step('Checking the game-data manifest')
    manifest = args.manifest.resolve()
    if not manifest.is_file():
        raise RuntimeError('Game-data manifest is missing: ' + str(manifest)
                           + '. Update your checkout or supply --manifest with a valid file.')
    from prepare_game_data import validate_manifest
    validate_manifest(json.loads(manifest.read_text(encoding='utf-8')))
    print('      Manifest:', manifest, flush=True)
    step('Finding Visual Studio, Qt and build tools')
    installation, generator = visual_studio()
    print('      Visual Studio:', installation, '(' + generator + ')', flush=True)
    developer_environment(installation)
    for tool in ('git', 'cmake', 'cpack'):
        if not shutil.which(tool):
            raise RuntimeError('Install ' + tool + ' and make it available on PATH, then rerun.')
    qt = qt_directory(args.qt)
    print('      Qt:', qt, flush=True)
    cache = ROOT / ('build-runtime-cache/windows-vs' + generator.split()[2])
    cache.mkdir(parents=True, exist_ok=True)
    step('Getting portable OpenSSH', cache / 'OpenSSH-Win64')
    ssh = openssh(cache)
    # New inputs get a separate runtime; never replace a database or reuse a stale seed.
    digest = hashlib.sha256(manifest.read_bytes() + generator.encode() + SSH_SHA256.encode())
    for path in [Path(__file__), ROOT / 'etc/creator-test/prepare_runtime.py',
                 *sorted((ROOT / 'etc/creator-test/mod-creator-test').rglob('*'))]:
        if path.is_file():
            digest.update(path.read_bytes())
    runtime = ROOT / 'build-runtime' / ('windows-' + digest.hexdigest()[:16]) / 'Runtime'
    stamp = runtime.parent / 'prepared.ok'
    step('Updating Git submodules')
    run('git', 'submodule', 'update', '--init', '--recursive')
    step('Preparing the local server and database', runtime)
    if not stamp.is_file() or not (runtime / 'creator-runtime.json').is_file():
        if runtime.exists():
            raise RuntimeError('Runtime preparation is incomplete; rename this directory and rerun: ' + str(runtime))
        run(sys.executable, ROOT / 'etc/creator-test/prepare_runtime.py', '--output', runtime,
            '--cache', cache, '--data-manifest', manifest, '--ssh', ssh, '--generator', generator)
        stamp.write_text('Prepared successfully\n')
    else:
        print('Reusing prepared Windows server and database:', runtime)
    build = ROOT / ('build-creator-windows-vs' + generator.split()[2])
    server = cache / 'tortoise-source'
    step('Building Noggit (Release, ' + str(args.jobs) + ' parallel jobs)', build)
    run('cmake', '-S', ROOT, '-B', build, '-G', generator, '-A', 'x64',
        '-DCMAKE_PREFIX_PATH=' + qt.as_posix(), '-DCMAKE_BUILD_TYPE=Release', '-DUSE_SQL=ON',
        '-DMYSQL_INCLUDE_DIR=' + (server / 'dep/windows/include/mysql').as_posix(),
        '-DMYSQL_LIBRARY=' + (server / 'dep/windows/lib/x64_release/libmysql.lib').as_posix(),
        '-DCREATOR_RUNTIME_BUNDLE=' + runtime.as_posix())
    run('cmake', '--build', build, '--config', 'Release', '--target', 'noggit', '--parallel', args.jobs,
        '--', '/p:CL_MPCount=' + str(args.jobs))
    packages = ROOT / 'packages'
    package = packages / (package_name + '.zip')
    step('Zipping the release', package)
    run('cpack', '--config', build / 'CPackConfig.cmake', '-C', 'Release', '-B', packages,
        '-D', 'CPACK_PACKAGE_FILE_NAME=' + package_name)
    if not package.is_file():
        raise RuntimeError('CPack finished but the ZIP is missing: ' + str(package))
    size = package.stat().st_size / (1024 * 1024)
    print('\n' + '=' * 72)
    print('Done:', release)
    print('  Release ZIP (upload this):', package, f'({size:.0f} MB)')
    print('  Noggit build folder:      ', build)
    print('  Prepared server/database: ', runtime)
    print('  Download cache:           ', cache)
    print('Extract the ZIP into a fresh folder and run Noggit/noggit.exe to verify first-run setup.')


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError) as error:
        print('\nBUILD FAILED:', error, file=sys.stderr)
        sys.exit(1)
