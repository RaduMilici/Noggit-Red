# Portable Creator packages (Windows x64 and Ubuntu x86_64)

End users extract one platform-specific ZIP and run `Noggit/noggit.exe` on Windows,
or `./start-creator` on Ubuntu. No MariaDB installation, database service, Python,
Git, Docker, or separate server installation is required. Game-client data is
optional at packaging time: without matching maps/dbc/vmaps/mmaps the world server
may not start. Add extracted data under `Runtime/mangosd/data` when available.

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

From this repository, on either platform:

```sh
python etc/creator-test/prepare_runtime.py --output build-runtime/Runtime
```

Use `python3` on Ubuntu. The tool downloads a checksum-verified portable MariaDB
11.4.5 archive, fetches a pinned Tortoise source revision, includes mod-creator-test,
builds the realm/world servers, and initializes an offline seed using the bundled
MariaDB. It does not register a service or modify a system database. Preparation
uses a temporary directory and publishes only after a clean database shutdown.
An existing output folder is rejected to protect data.

Options: `--server-source` selects an existing matching Tortoise checkout;
`--server-install` selects its already-built installation (must include the Creator
test module); `--generator` selects a CMake generator; `--data` includes extracted
game data. Downloads and server builds are cached in `build-runtime-cache`.

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
startup with the matching extracted data. Neither Python syntax checks nor a build
alone verifies the real server or database schema compatibility.

Offline preparation regression checks: `python test/runtime/PackagingTests.py`.
They verify interrupted-download recovery, checksum rejection and preservation of
existing output. Preparation logs remain under `build-runtime-cache/preparation-logs`
even when a failed staging directory is discarded.
