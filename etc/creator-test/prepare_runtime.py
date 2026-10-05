#!/usr/bin/env python3
"""Build-time Windows/Ubuntu runtime preparation. End users need no Python or DB install."""
import argparse
import hashlib
import json
import os
import platform
import re
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import struct
import tarfile
import tempfile
import time
import urllib.request
import zipfile
import xml.etree.ElementTree as ET

MARIADB = {
    "win32": ("winx64-packages/mariadb-11.4.5-winx64.zip",
              "b7c11d38657f16b837e68199d73670510aadb78f42dfa5d5fdea31a7aab342e3"),
    "linux": ("bintar-linux-systemd-x86_64/mariadb-11.4.5-linux-systemd-x86_64.tar.gz",
              "2a44cb70a87dba7eb2cab3b5af2c0416a0204d93a8fda387b4b70c9f1bab7bd6"),
}
SCHEMAS = {"tw_logon": "realmd", "tw_world": "mangos", "tw_char": "characters", "tw_logs": "logs"}
SERVER_REVISION = "d94947b0db60c33e7248523ad0ba7f58af97fd09"
VCPKG_REVISION = "d5ec528843d29e3a52d745a64b469f810b2cedbf"

def prepare_legacy_crt(cache):
    """Extract Microsoft's VC90 runtime without installing its redistributable."""
    archive = cache / "vc90-redist.exe"
    if not archive.exists():
        urllib.request.urlretrieve("https://download.microsoft.com/download/5/D/8/"
                                   "5D8C65CB-C849-4025-8E95-C3966CAFD8AE/vcredist_x64.exe", archive)
    content = archive.read_bytes()
    if hashlib.sha256(content).hexdigest() != "c5e273a4a16ab4d5471e91c7477719a2f45ddadb76c7f98a38fa5074a6838654":
        raise RuntimeError("VC90 redistributable checksum mismatch")
    destination = cache / "vc90-crt"
    destination.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="vc90-", dir=cache) as temporary:
        temporary = Path(temporary)
        # The signed self-extracting executable contains a standard cabinet.
        for match in re.finditer(b"MSCF", content):
            offset = match.start()
            reserved, size = struct.unpack_from("<II", content, offset + 4)
            if reserved == 0 and 36 <= size <= len(content) - offset:
                (temporary / "outer.cab").write_bytes(content[offset:offset + size])
                break
        else:
            raise RuntimeError("VC90 redistributable cabinet was not found")
        run(["expand.exe", "-F:vc_red.cab", temporary / "outer.cab", temporary], stdout=subprocess.DEVNULL)
        for name in ("msvcr90.dll", "msvcp90.dll", "msvcm90.dll", "manifest"):
            packed = name + ".30729.6161.Microsoft_VC90_CRT_x64.QFE"
            run(["expand.exe", "-F:" + packed, temporary / "vc_red.cab", temporary], stdout=subprocess.DEVNULL)
            shutil.copy2(temporary / packed, destination / ("Microsoft.VC90.CRT.manifest" if name == "manifest" else name))
    return destination

def deploy_legacy_crt(mysql_dll, crt):
    """Bind the old MySQL DLL to the bundled, security-updated private assembly."""
    tools = list(Path(os.environ.get("ProgramFiles(x86)", "C:/Program Files (x86)"))
                 .glob("Windows Kits/10/bin/*/x64/mt.exe"))
    manifest_tool = shutil.which("mt.exe") or (sorted(tools)[-1] if tools else None)
    if not manifest_tool:
        raise RuntimeError("Windows SDK mt.exe is required to deploy the legacy MySQL runtime")
    with tempfile.TemporaryDirectory(prefix="mysql-manifest-") as temporary:
        manifest = Path(temporary) / "mysql.manifest"
        run([manifest_tool, "-nologo", "-inputresource:" + str(mysql_dll) + ";2", "-out:" + str(manifest)])
        tree = ET.parse(manifest)
        identity = tree.find(".//{urn:schemas-microsoft-com:asm.v1}dependentAssembly/{urn:schemas-microsoft-com:asm.v1}assemblyIdentity")
        if identity is None or identity.get("name") != "Microsoft.VC90.CRT":
            raise RuntimeError("Unexpected legacy MySQL manifest")
        identity.set("version", "9.0.30729.6161")
        tree.write(manifest, encoding="utf-8", xml_declaration=True)
        run([manifest_tool, "-nologo", "-manifest", manifest, "-outputresource:" + str(mysql_dll) + ";2"])
    for file in crt.iterdir():
        shutil.copy2(file, mysql_dll.parent / file.name)

def run(command, quiet=False, **kwargs):
    if not quiet:
        print("Running:", command[0], flush=True)
    return subprocess.run([str(x) for x in command], check=True, **kwargs)

def checkout(repository, revision, folder):
    if not (folder / ".git").exists():
        run(["git", "init", folder])
        run(["git", "-C", folder, "remote", "add", "origin", repository])
    actual = subprocess.run(["git", "-C", str(folder), "rev-parse", "HEAD"],
                            text=True, capture_output=True)
    if actual.returncode:
        for attempt in range(3):
            try:
                run(["git", "-c", "http.version=HTTP/1.1", "-C", folder,
                     "fetch", "--depth", "1", "origin", revision])
                break
            except subprocess.CalledProcessError:
                if attempt == 2:
                    raise
                time.sleep(1)
        run(["git", "-C", folder, "checkout", "--detach", "FETCH_HEAD"])
        actual = subprocess.run(["git", "-C", str(folder), "rev-parse", "HEAD"],
                                text=True, capture_output=True, check=True)
    if actual.stdout.strip() != revision:
        raise RuntimeError("Cached source has an unexpected revision: " + str(folder))

def prepare_ace(cache):
    """ACE is needed on the Windows builder; its DLLs ship in the runtime."""
    folder = cache / "vcpkg"
    checkout("https://github.com/microsoft/vcpkg.git", VCPKG_REVISION, folder)
    if not (folder / "vcpkg.exe").exists():
        run(["cmd.exe", "/c", str(folder / "bootstrap-vcpkg.bat"), "-disableMetrics"])
    # Use vcpkg's native pkgconf instead of obsolete MSYS mirror packages.
    run([folder / "vcpkg.exe", "install", "pkgconf:x64-windows", "--disable-metrics"])
    # Git for Windows already supplies Perl, which ACE uses to generate MSVC projects.
    # Prefer that available build tool over downloading another portable toolchain.
    git = shutil.which("git")
    perl = shutil.which("perl")
    if git and not perl:
        for parent in list(Path(git).resolve().parents)[:3]:
            perl_bin = parent / "usr/bin"
            if (perl_bin / "perl.exe").is_file():
                perl = str(perl_bin / "perl.exe")
                break
    command = [folder / "vcpkg.exe", "install", "ace:x64-windows", "--disable-metrics"]
    if perl:
        command.append("--x-cmake-args=-DPERL:FILEPATH=" + Path(perl).as_posix())
    command.append("--x-cmake-args=-DPKGCONFIG:FILEPATH="
                   + (folder / "installed/x64-windows/tools/pkgconf/pkgconf.exe").as_posix())
    run(command)
    return folder / "installed/x64-windows"

def download_database(cache):
    relative, expected = MARIADB[sys.platform]
    archive = cache / Path(relative).name
    if not archive.exists():
        print("Downloading portable MariaDB (no system service is installed)", flush=True)
        partial = archive.with_suffix(".download")
        url = "https://archive.mariadb.org/mariadb-11.4.5/" + relative
        failures = 0
        while True:
            offset = partial.stat().st_size if partial.exists() else 0
            request = urllib.request.Request(url + (f"?offset={offset}" if offset else ""),
                                             headers={"Range": f"bytes={offset}-{offset + 4 * 1024 * 1024 - 1}"})
            try:
                with urllib.request.urlopen(request, timeout=60) as response:
                    resume = response.status == 206
                    expected_length = int(response.headers.get("Content-Length", "0"))
                    received = 0
                    with partial.open("ab" if resume else "wb") as target:
                        while chunk := response.read(1024 * 1024):
                            target.write(chunk)
                            received += len(chunk)
                    if expected_length and received != expected_length:
                        raise OSError("Truncated MariaDB download")
                    total = int(response.headers["Content-Range"].split("/")[-1]) if resume else 0
                if total and partial.stat().st_size < total:
                    failures = 0
                    continue
                break
            except OSError:
                failures += 1
                if failures == 4:
                    raise
                time.sleep(1)
        partial.replace(archive)
    digest = hashlib.sha256()
    with archive.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    if digest.hexdigest() != expected:
        raise RuntimeError("MariaDB archive checksum mismatch; remove the cached archive and retry")
    extraction = cache / "mariadb-extracted"
    if not extraction.exists():
        with tempfile.TemporaryDirectory(prefix="mariadb-unpack-", dir=cache) as temporary:
            if sys.platform == "win32":
                with zipfile.ZipFile(archive) as package:
                    package.extractall(temporary)
            else:
                # Python 3.12+ protects against escaping paths and unsafe links.
                if not hasattr(tarfile, "data_filter"):
                    raise RuntimeError("Ubuntu runtime preparation requires Python 3.12+ (safe archive extraction)")
                with tarfile.open(archive) as package:
                    package.extractall(temporary, filter="data")
            Path(temporary).rename(extraction)
    return next(extraction.glob("mariadb-*"))

def prepare_seed(database, source, seed, workspace):
    if seed.exists():
        raise RuntimeError("Refusing to initialize an existing database seed: " + str(seed))
    suffix = ".exe" if sys.platform == "win32" else ""
    server = database / "bin" / ("mariadbd" + suffix)
    client = database / "bin" / ("mariadb" + suffix)
    if sys.platform == "win32":
        run([database / "bin/mysql_install_db.exe", "--datadir=" + str(seed), "--password=creator-build"])
    else:
        run([database / "scripts/mariadb-install-db", "--basedir=" + str(database),
             "--datadir=" + str(seed), "--auth-root-authentication-method=normal", "--skip-test-db"])
    with socket.socket() as reservation:
        reservation.bind(("127.0.0.1", 0))
        port = reservation.getsockname()[1]
    environment = os.environ.copy()
    if sys.platform == "linux":
        environment["LD_LIBRARY_PATH"] = str(database / "lib") + ":" + environment.get("LD_LIBRARY_PATH", "")
    config = workspace / "seed.cnf"
    config.write_text("[mysqld]\nbasedir=" + database.as_posix() + "\ndatadir=" + seed.as_posix()
                      + f"\nbind-address=127.0.0.1\nport={port}\nsocket=" + (workspace / "seed.sock").as_posix()
                      + "\npid-file=" + (workspace / "seed.pid").as_posix() + "\n", encoding="utf-8")
    auth = workspace / "client.cnf"
    auth.write_text(f"[client]\nprotocol=tcp\nhost=127.0.0.1\nport={port}\nuser=root\n"
                    + ("password=creator-build\n" if sys.platform == "win32" else ""), encoding="utf-8")
    auth.chmod(0o600)
    command = [client, "--defaults-file=" + str(auth), "--connect-timeout=1", "--batch"]
    with (workspace / "seed.log").open("wb") as log:
        process = subprocess.Popen([str(server), "--defaults-file=" + str(config)], env=environment,
                                   stdout=log, stderr=log)
        try:
            for _ in range(120):
                if process.poll() is not None:
                    raise RuntimeError("Portable MariaDB exited; see " + str(workspace / "seed.log"))
                result = subprocess.run([str(x) for x in command] + ["--execute=SELECT 1"],
                                        env=environment, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                if result.returncode == 0:
                    break
                time.sleep(0.5)
            else:
                raise RuntimeError("Portable MariaDB startup timed out")
            for sql in [source / "sql/create_databases.sql", *sorted((source / "sql/base").glob("*.sql"))]:
                log.write(("Importing " + sql.name + "\n").encode("utf-8"))
                log.flush()
                with tempfile.TemporaryFile() as transformed:
                    with sql.open("r", encoding="utf-8") as original:
                        for line in original:
                            for old, new in SCHEMAS.items():
                                line = line.replace("`" + old + "`", "`" + new + "`")
                            transformed.write(line.encode("utf-8"))
                    transformed.seek(0)
                    run(command + (["mangos"] if sql.parent.name == "base" else []), quiet=True,
                        stdin=transformed, env=environment, stdout=log, stderr=log)
            grants = "CREATE USER 'creator'@'127.0.0.1' IDENTIFIED BY 'creator-local';"
            for schema in SCHEMAS.values():
                grants += f"GRANT ALL ON `{schema}`.* TO 'creator'@'127.0.0.1';"
            grants += "GRANT SHUTDOWN ON *.* TO 'creator'@'127.0.0.1';"
            grants += "REPLACE INTO realmd.realmlist (id,name,address,port) VALUES (1,'Creator','127.0.0.1',18085);"
            run(command + ["--execute=" + grants], env=environment, stdout=log, stderr=log)
            run(command + ["--execute=SHUTDOWN"], env=environment, stdout=log, stderr=log)
            if process.wait(timeout=30) != 0:
                raise RuntimeError("MariaDB did not shut down cleanly")
            # The initializer's Windows config contains build paths; runtime uses Workspace/mariadb.cnf.
            if (seed / "my.ini").exists():
                (seed / "my.ini").unlink()
        finally:
            if process.poll() is None:
                process.terminate()
                process.wait(timeout=30)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server-source", type=Path, help="Matching Tortoise checkout; otherwise fetch pinned source")
    parser.add_argument("--output", type=Path, required=True, help="New Runtime folder (must not exist)")
    parser.add_argument("--cache", type=Path, default=Path("build-runtime-cache"))
    parser.add_argument("--server-install", type=Path, help="Already built server installation; otherwise build it")
    parser.add_argument("--ace-root", type=Path, help="Windows build dependency, not needed by end users")
    parser.add_argument("--generator", default="Visual Studio 16 2019" if sys.platform == "win32" else None)
    parser.add_argument("--data", type=Path, help="Optional extracted maps/dbc/vmaps/mmaps")
    args = parser.parse_args()
    if sys.platform not in MARIADB or platform.machine().lower() not in ("amd64", "x86_64"):
        parser.error("Only Windows x64 and Ubuntu x86_64 are supported")
    if sys.platform == "linux" and not hasattr(tarfile, "data_filter"):
        parser.error("Ubuntu runtime preparation requires Python 3.12+ for safe archive extraction")
    output, cache = args.output.resolve(), args.cache.resolve()
    if output.exists():
        parser.error("Output already exists; choose a new folder to preserve databases")
    cache.mkdir(parents=True, exist_ok=True)
    source = args.server_source.resolve() if args.server_source else cache / "tortoise-source"
    if not args.server_source:
        checkout("https://github.com/tortoise-wow/tortoise-wow.git", SERVER_REVISION, source)
    if not (source / "sql/create_databases.sql").is_file():
        parser.error("Server source must include sql/create_databases.sql; incomplete cached checkout")
    revision = subprocess.check_output(["git", "-C", str(source), "rev-parse", "HEAD"], text=True).strip()
    if not args.server_source and revision != SERVER_REVISION:
        parser.error("Cached server checkout does not match the pinned revision")
    database = download_database(cache)
    legacy_crt = prepare_legacy_crt(cache) if sys.platform == "win32" else None
    if sys.platform == "win32" and not args.ace_root and not args.server_install:
        args.ace_root = prepare_ace(cache)
    install = args.server_install.resolve() if args.server_install else cache / "server-install"
    if not args.server_install:
        module = source / "modules/mod-creator-test"
        shutil.copytree(Path(__file__).parent / "mod-creator-test", module, dirs_exist_ok=True)
        command = ["cmake", "-S", source, "-B", cache / "server-build", "-DCMAKE_BUILD_TYPE=Release",
                   "-DCMAKE_INSTALL_PREFIX=" + str(install), "-DMODULE_MOD_CREATOR_TEST=static", "-DUSE_STD_MALLOC=ON"]
        if args.generator:
            command.extend(["-G", args.generator])
            if args.generator.startswith("Visual Studio"):
                command.extend(["-A", "x64"])
        if args.ace_root:
            command.append("-DACE_ROOT=" + str(args.ace_root.resolve()))
        run(command)
        run(["cmake", "--build", cache / "server-build", "--config", "Release", "--parallel", "4"])
        run(["cmake", "--install", cache / "server-build", "--config", "Release"])
    # Keep failed preparation away from the requested output; publish only a complete seed.
    with tempfile.TemporaryDirectory(prefix="creator-", dir=cache) as temporary:
        stage = Path(temporary) / "Runtime"
        stage.mkdir()
        shutil.copytree(database, stage / "MariaDB", symlinks=True)
        suffix = ".exe" if sys.platform == "win32" else ""
        for name in ("realmd", "mangosd"):
            folder = stage / name
            folder.mkdir()
            executable = next((item for item in install.rglob(name + suffix) if item.is_file()), None)
            config = next(install.rglob(name + ".conf.dist"), None)
            if not executable or not config:
                raise RuntimeError("Server installation is missing " + name + " or its config")
            shutil.copy2(executable, folder / executable.name)
            shutil.copy2(config, folder / config.name)
            for dll in install.rglob("*.dll"):
                shutil.copy2(dll, folder / dll.name)
            if sys.platform == "win32":
                ace_folders = []
                if args.ace_root:
                    ace_folders = [args.ace_root / "bin", args.ace_root / "lib"]
                    if not any(item.exists() for item in ace_folders):
                        ace_folders = [args.ace_root]
                for dependency in [source / "dep/windows/lib/x64_release", *ace_folders]:
                    if dependency:
                        for dll in Path(dependency).rglob("*.dll"):
                            shutil.copy2(dll, folder / dll.name)
                deploy_legacy_crt(folder / "libmySQL.dll", legacy_crt)
        shutil.copytree(Path(__file__).parent / "mod-creator-test/conf", stage / "mangosd/modules")
        shutil.copytree(source / "sql/database_updates", stage / "database_updates")
        (stage / "database_updates/auth").mkdir(exist_ok=True)
        for sql in (stage / "database_updates").rglob("*.sql"):
            text = sql.read_text(encoding="utf-8")
            for old, new in SCHEMAS.items():
                text = text.replace("`" + old + "`", "`" + new + "`")
            sql.write_text(text, encoding="utf-8")
        if args.data:
            shutil.copytree(args.data, stage / "mangosd/data")
        seed = stage / "DatabaseSeed"
        workspace = cache / "preparation-logs" / str(time.time_ns())
        workspace.mkdir(parents=True)
        print("Initializing database seed; log:", workspace / "seed.log", flush=True)
        prepare_seed(stage / "MariaDB", source, seed, workspace)
        (stage / "creator-runtime.json").write_text(json.dumps({"format": 1, "platform": sys.platform,
            "mariadb": "11.4.5", "serverRevision": revision, "databaseUpdates": True}, indent=2), encoding="utf-8")
        licenses = stage / "licenses"
        licenses.mkdir()
        for name in ("COPYING", "LICENSE", "LICENSE.md"):
            if (source / name).is_file():
                shutil.copy2(source / name, licenses / ("Tortoise-" + name))
        output.parent.mkdir(parents=True, exist_ok=True)
        shutil.copytree(stage, output, symlinks=True)
    print("Prepared", output)
    print("Build Noggit with USE_SQL=ON and CREATOR_RUNTIME_BUNDLE=" + str(output))

if __name__ == "__main__":
    main()
