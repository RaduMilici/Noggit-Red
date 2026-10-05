# Portable Creator packages (Windows x64 and Ubuntu x86_64)

End users extract one platform-specific ZIP and run `Noggit/noggit.exe` on Windows,
or `./start-creator` on Ubuntu. No MariaDB installation, database service, Python,
Git, Docker, OpenSSH installation, or separate server installation is required.
The small ZIP includes a pinned game-data manifest, OpenSSH, and (on Ubuntu) a
portable Wine runtime. On first startup Noggit downloads the extracted server data
with progress and SHA-256 verification, then starts the local services. Users only
select their own matching game client; no extractors are needed. Completed files
are cached in `Workspace/GameData/<manifest-hash>` and reused offline. Cancel or
failure preserves completed files; click Start to retry. The current file restarts.
An optional offline release can still include all data with `--data`.

These commands are for the **release builder**, not the end user. The builder
needs CMake 3.21+, Git, Python, Qt5 (including Multimedia), and a C++ compiler.
Windows uses the MSVC x64 toolchain; the tool builds ACE through a pinned vcpkg
checkout, or accepts existing ACE development headers/library (`--ace-root`).
The Windows SDK manifest tool (`mt.exe`, included with the C++ build tools) is
used to bind the legacy MySQL DLL to an extracted, checksum-verified Microsoft
VC2008 SP1 runtime. Its private assembly ships beside the executables; the
redistributable installer is never run.
Ubuntu uses Python 3.12+ and development packages for ACE, MariaDB client, OpenSSL,
zlib, Qt5 and the libraries needed by the portable MariaDB executable (including
libaio and ncurses). Build on the oldest Ubuntu release the package supports;
glibc and the OS loader remain platform dependencies and are not bundled.

## Prepare the server and database

First generate and publish the game-data manifest as described below.

From this repository, on either platform:

```sh
python etc/creator-test/prepare_runtime.py --output build-runtime/Runtime \
  --data-manifest build-runtime/game-data.json --ssh /path/to/portable-openssh
# Ubuntu: also add --wine /path/to/portable-wine
```

Use `python3` on Ubuntu. The tool downloads a checksum-verified portable MariaDB
11.4.5 archive, fetches a pinned Tortoise source revision, includes mod-creator-test,
builds the realm/world servers, and initializes an offline seed using the bundled
MariaDB. It does not register a service or modify a system database. Preparation
uses a temporary directory and publishes only after a clean database shutdown.
An existing output folder is rejected to protect data.

Options: `--server-source` selects an existing matching Tortoise checkout;
`--server-install` selects its already-built installation (must include the Creator
test module); `--generator` selects a CMake generator. Choose `--data-manifest` or `--data`; `--ssh` is required;
Ubuntu also requires `--wine`. Downloads and server builds are cached in
`build-runtime-cache`.

## Publish the game data once

Generate the manifest (this reads all 3.9 GB to hash it):

```sh
python3 etc/creator-test/prepare_game_data.py \
  --data /home/radu/tortoise-extract/extracted-data \
  --base-url https://pub-61be48bf4c0f4d208f849304433124c9.r2.dev/ \
  --output build-runtime/game-data.json
```

Upload the four directories to the `wow-extracted-data` R2 bucket, preserving
paths such as `dbc/AreaTable.dbc` and `maps/0004331.map`. With AWS CLI installed
**on the release builder** and R2 credentials configured in its environment or
profile (never in Noggit or the manifest):

```sh
aws s3 sync /home/radu/tortoise-extract/extracted-data/ s3://wow-extracted-data/ \
  --endpoint-url https://c92fd44349d4326a3815748d293a669f.r2.cloudflarestorage.com \
  --exclude '*' --include 'dbc/*' --include 'maps/*' --include 'vmaps/*' --include 'mmaps/*'
```

The public URL serves downloads; the S3 endpoint is only for uploading. No upload
credentials ship to users. A public bucket root does not list objects; verify an
actual object URL after upload. `r2.dev` is rate-limited; configure an R2 custom
domain for public releases and pass that domain as `--base-url`.
For data updates, use a new bucket prefix in both the upload destination and
`--base-url` so older releases keep their matching files. Keep the generated
manifest with the release; `--data-manifest` copies only this small file into the ZIP.

Preparation validates manifest paths, file sizes, SHA-256 hashes and all required
data groups. Runtime streams each file into an atomic temporary file and verifies
its size/hash before committing it. The cache is separated by manifest hash;
local verified-file receipts avoid re-downloading on subsequent launches. No
archive utility, Python, credentials or browser is needed on the end user's PC.
These checks do not prove server/client version compatibility; verify world
startup and the game client before releasing.

For a fully offline ZIP use `--data /home/radu/tortoise-extract/extracted-data`
instead. Only the four extracted-data folders are copied, never client archives.

`--ssh` points to a portable OpenSSH directory with `ssh[.exe]`,
`ssh-keyscan[.exe]`, its supporting files, and licenses. It is copied intact to
`Runtime/OpenSSH`; Noggit prefers these executables over PATH. The package's
shared-library scan includes both executables.

`--wine` points to a complete **relocatable** Wine distribution with `bin/wine`,
`bin/wineserver`, its libraries, supporting files, and licenses. It must support
the 32-bit game client on the supported Ubuntu x86_64 system without installing
32-bit system packages (for example a tested WoW64 distribution). Copying only
`/usr/bin/wine` is insufficient. This distribution is copied intact to
`Runtime/Wine` and must be tested on a clean machine; the regular x86_64 server
library scan cannot validate Wine's dynamically loaded modules. Noggit already
prefers bundled Wine for launching the client.

The seed imports `create_databases.sql` and all `sql/base` files, maps the upstream
schema names to the names used by Noggit, and creates the local account and realm.
The matching SQL migrations ship in the runtime and the server's native updater
applies them when it starts. Existing prepared bundles without this manifest flag
continue to use disabled updates. This is not a claim of world-server acceptance:
the matching server must still initialize successfully with extracted game data.

## Build and package Noggit

Windows / Git Bash, with the default fetched server source:

```sh
cmake -S . -B build-creator -G "Visual Studio 16 2019" -A x64 \
  -DCMAKE_PREFIX_PATH="C:/Qt/5.15.2/msvc2019_64" \
  -DCMAKE_BUILD_TYPE=Release -DUSE_SQL=ON \
  -DMYSQL_INCLUDE_DIR="$PWD/build-runtime-cache/tortoise-source/dep/windows/include/mysql" \
  -DMYSQL_LIBRARY="$PWD/build-runtime-cache/tortoise-source/dep/windows/lib/x64_release/libmysql.lib" \
  -DCREATOR_RUNTIME_BUNDLE="$PWD/build-runtime/Runtime"
cmake --build build-creator --config Release --target noggit --parallel 4 -- /p:CL_MPCount=4
cpack --config build-creator/CPackConfig.cmake -C Release -B packages
```

Ubuntu (the system MariaDB **client development library** is a build dependency):

```sh
cmake -S . -B build-creator -DCMAKE_BUILD_TYPE=Release -DUSE_SQL=ON \
  -DMYSQL_INCLUDE_DIR=/usr/include/mariadb \
  -DCREATOR_RUNTIME_BUNDLE="$PWD/build-runtime/Runtime"
cmake --build build-creator --target noggit --parallel 4
cpack --config build-creator/CPackConfig.cmake -C Release -B packages
```

Packaging deploys Qt plugins, scans dependencies of Noggit, both servers, and the
MariaDB executables, and copies their shared libraries. Windows puts dependencies
beside each executable; Ubuntu uses `Runtime/lib` and the launcher. Packaging fails
on unresolved or conflicting dependencies rather than emitting an incomplete ZIP.
Third-party notices included in the distributions are retained; the server and
Noggit licenses are copied too.

Verify the resulting ZIP on a clean Windows/Ubuntu machine, including Start/Stop,
first-run seed copying, restart without data loss, and successful realm/world
startup with the bundled extracted data, production SSH, and game-client launch
without system Wine/OpenSSH. Neither Python syntax checks nor a build
alone verifies the real server or database schema compatibility.

Offline preparation regression checks: `python test/runtime/PackagingTests.py`.
They verify interrupted-download recovery, checksum rejection and preservation of
existing output. Preparation logs remain under `build-runtime-cache/preparation-logs`
even when a failed staging directory is discarded.
