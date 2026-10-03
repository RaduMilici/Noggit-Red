# Local Creator runtime

`RuntimeManager` owns QProcess children and dependency ordering. `LocalServerPanel`
is a view shared by the chooser and editor. `ApplicationEntry` constructs the
manager after command-line probes, schedules autostart, and connects aboutToQuit
to bounded shutdown. Its destructor is a second cleanup path.

Installation layout (all paths are resolved from the executable, not the shell):

```
Creator/
  Noggit/noggit[.exe]
  Runtime/
    creator-runtime.json
    MariaDB/bin/{mariadbd,mariadb}[.exe]
    MariaDB/{lib,share}/... (matching portable distribution and dependencies)
    realmd/{realmd[.exe],realmd.conf.dist}
    mangosd/{mangosd[.exe],mangosd.conf.dist,data/{dbc,maps,vmaps,mmaps}}
    DatabaseSeed/... (cleanly shut down, initialized MariaDB data directory)
  Database/data/... (mutable copy; never overwritten on restart)
  Workspace/{runtime.ini,mariadb.cnf,client.cnf,realmd.conf,mangosd.conf,logs/}
```

Release packagers must supply matching server binaries, templates, extracted data,
and an offline database seed for the exact bundled MariaDB version and platform.
The seed contains `realmd`, `mangos`, `characters`, `logs`, with the matching server
schema/content, and `creator`@`127.0.0.1` / `creator-local`. Grant that account
permissions on these four schemas and SHUTDOWN for graceful MariaDB termination.
The realm row has id 1, address 127.0.0.1, port 18085. Remove personal accounts and
production data before distributing a seed. Package upstream licenses alongside
binaries. `creator-runtime.json` identifies a managed installation; its presence
prevents falling back to saved external database connections while stopped.

Set CMake `CREATOR_RUNTIME_BUNDLE` to the prepared Runtime folder for installation.
CMake rejects missing required payloads. Executables are launched directly with
argument lists; no shell, system service, Git, Docker, SSH, or PATH search is used.
Linux bundles must include compatible shared libraries in `Runtime/lib`; OS ABI compatibility is
still a release packaging requirement. Windows bundles include matching DLLs.

The database listens on loopback port 13306, realm on 13724 and world on 18085.
First run copies the seed through a staging directory and an atomic rename.
An installation lock protects the data directory. Configs are regenerated from
matching server templates. Persist only the autostart preference in runtime.ini;
no absolute installation paths are stored in generated configs. Existing Noggit
project DB settings are preserved; the managed local connection is session-only.
Readiness requires a successful authenticated SQL query, then realm/world listener
availability. Child exit rolls back all children. Startup timeout is two minutes.
Stop and Restart are asynchronous; application exit waits at most 15 seconds per
service before killing an unresponsive child. World shutdown uses its console;
MariaDB uses SQL SHUTDOWN. An OS-level crash or power failure cannot run Qt cleanup.

Developer verification (Linux fixture programs require Python only in tests):

```
cmake -S test/runtime -B build-runtime-tests
cmake --build build-runtime-tests
ctest --test-dir build-runtime-tests --output-on-failure
```

Tests use disposable fake services on the same local ports, covering missing
payload, occupied port, startup, repeated Start, Restart, data preservation, Stop,
shutdown, installation locking, child failure rollback, and cancellation during startup. Do not run concurrently with a real
Creator runtime. Real runtime acceptance additionally requires all three services
to reach Running and exit cleanly with the packaged data.

For a prepared bundle, run `build-runtime-tests/runtime_smoke /absolute/path/to/Creator`
to exercise real processes without loading the editor. It prints service states,
waits for all three listeners, requests an ordered stop, and exits nonzero on
failure or timeout. Logs are retained in `Creator/Workspace/logs`.

The local development bundle prepared during this change is `build/Creator`.
MariaDB 11.8.6 was extracted from Ubuntu packages (not installed as a service).
Tortoise source is `/home/radu/Documents/WoW/tortoise-wow`; world data was copied
from `/mnt/data/tortoise-extract/extracted-data`. The seed uses the source tree's
`sql/create_databases.sql` and `sql/base/*.sql`, mapping `tw_logon`, `tw_world`,
`tw_char`, `tw_logs` to the four schemas above. Runtime database auto-updates are
disabled: release seeds must already match their server revision. This local
bundle is a development artifact, not a cross-platform release package.

Compiled-in Tortoise modules must ship their matching `*.conf.dist` files in
`Runtime/mangosd/modules/`. The manager seeds them into `Workspace/modules/`
without the `.dist` suffix and preserves existing local module settings.

The base SQL alone is insufficient for the current Tortoise server revision.
Run `python3 test/runtime/PrepareTortoiseDatabase.py <Creator> <Tortoise-source>`
as an explicit developer packaging step with Noggit closed. It updates an isolated
copy using Tortoise's native updater, checks world initialization and clean shutdown,
then promotes the prepared data and seed while retaining the originals. Failure
leaves the original databases unchanged and preserves the preparation log. This
can take several minutes and is never launched automatically by Noggit.
