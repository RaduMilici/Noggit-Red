# One-click local testing

## Designer workflow

Open **LOCAL SERVER → Client Profiles…** once. Choose the existing WoW executable,
enter the Lightsail hostname/IP for **Play Production**, and optionally enter the
name of an existing local test character. No passwords are stored. Client paths
are relative to the Creator installation; keeping the client inside that folder
makes the installation portable.

- Right-click terrain → **Test Here**: save pending NPC changes and test that point,
  using the current camera heading.
- Select/right-click an NPC → **Test NPC**: save and test about two meters in front
  of it, facing the NPC. This also works after a pending move or rotation.
- Quest Editor → **Test Quest**: save the quest and test beside its giver. If there
  are multiple placements, choose by NPC/map name and placement number.
- **Test Local**: start/restart the local runtime and launch the local client without
  a teleport request. Use this to create a local character the first time.
- **Play Production**: switch the same client to the configured Lightsail endpoint
  and launch it. This action has no database-save, server-start, restart, teleport,
  upload or synchronization path. Existing startup preferences still apply when
  Noggit itself opens; turn off **Start with Noggit** if that is unwanted.

Close WoW before starting a new test or switching profiles. Creator blocks another
launch while a client it launched is still detected. Independently launched clients
must also be closed. Log in normally with the indicated local account and character;
the server teleports that character at login. Other characters/accounts cannot consume
its request. Cancel Test or closing Noggit removes the pending request. Requests expire
15 minutes after recording; cancellation does not shut down an otherwise usable runtime.

The local endpoint is **127.0.0.1:13724**, preserving Stage 1's non-default realm port.
Production uses the configured hostname/IP and optional port (otherwise WoW's default).
`realmlist.wtf` is written atomically; the original is retained once as
`realmlist.wtf.creator-backup`. Other settings are preserved.

## Components and data

- `src/noggit/runtime/ClientManager.*`: shared client selection, profile settings,
  endpoint validation, realm-file updates, launch and launched-process checks.
- `src/noggit/creator/TestSessionService.*`: asynchronous save/start/request/restart/
  readiness/launch/login-result state machine. No process logic in button handlers.
- `RuntimeManager.*`: synchronous save hook, shutdown notification, generated local
  module configuration. The existing ordered runtime restart is reused.
- `MapView.cpp`, `AuthoringDialogs.cpp`, `LocalServerPanel.cpp`: context actions,
  Test Quest and profile/status/cancellation controls.
- `mod-creator-test`: small static Tortoise module using PlayerScript::OnLogin and
  Player::TeleportTo. No direct edits to character position columns.
- `CreatorTestProtocol.hpp`: shared versioned request serialization/validation.
- `test/local-testing`: protocol and client profile tests. Runtime regression target
  now links the services used by the panel.
- `cmake/CreatorRuntime.cmake`: packaged runtime must contain module configuration;
  per-request acknowledgement also detects an old/missing server module at runtime.

New state is local files under `Workspace`: `creator-test.request`, `.ready`, `.result`,
and temporary/claim files; preferences are in `runtime.ini`. Request contents include
an opaque token, character/account identifiers, destination and expiry. The module
matches identity, validates the destination through the server, consumes the request
once before teleporting, and reports success/rejection for that token. A fresh ready
acknowledgement from the restarted module is required before the client launches.
Normal server teleport/save behavior handles persistence.

Read-only queries resolve characters/accounts and giver placements from
`characters.characters`, `realmd.account`, `creature`, `creature_template`,
`creature_questrelation`, and `map_template`. Existing Stage 2 services save content.
There are no new database tables, migrations, or remote database operations.

The module defaults to disabled. Noggit generates its enabled configuration only for
the managed runtime. The module additionally requires loopback binding and loopback
world/character/login DB endpoints. It has no network command receiver. Do not enable
or distribute this development module on production; Play Production does not install
anything there.

## Developer build / packaging

No full editor build, server build, or live runtime was run by the agent. Targeted
syntax checks passed against the existing editor and Tortoise compiler flags. Isolated
protocol and realm-file tests passed without launching WoW or any server.

Close Noggit, WoW and the local servers before replacing binaries. From the Noggit
repository, build the module into the **local** Tortoise worldserver:

```bash
cd /home/radu/Documents/Noggit-Red
mkdir -p /home/radu/Documents/WoW/tortoise-wow/modules/mod-creator-test
cp -a etc/creator-test/mod-creator-test/. /home/radu/Documents/WoW/tortoise-wow/modules/mod-creator-test/
cmake -S /home/radu/Documents/WoW/tortoise-wow -B /home/radu/Documents/WoW/tortoise-wow/build -DMODULE_MOD_CREATOR_TEST=static
cmake --build /home/radu/Documents/WoW/tortoise-wow/build --target mangosd -j2
cp /home/radu/Documents/WoW/tortoise-wow/build/src/mangosd/mangosd build/Creator/Runtime/mangosd/mangosd
mkdir -p build/Creator/Runtime/mangosd/modules
cp etc/creator-test/mod-creator-test/conf/mod-creator-test.conf.dist build/Creator/Runtime/mangosd/modules/
```

Build Noggit and copy it into the existing bundle:

```bash
cmake -S . -B build
cmake --build build -j2
cp build/bin/noggit build/Creator/Noggit/noggit
(cd build/Creator/Noggit && ./noggit)
```

Optional automated checks:

```bash
cmake -S test/local-testing -B build-local-testing
cmake --build build-local-testing -j2
ctest --test-dir build-local-testing --output-on-failure
cmake -S test/runtime -B build-runtime-tests
cmake --build build-runtime-tests -j2
ctest --test-dir build-runtime-tests --output-on-failure
```

The local-testing suite uses temporary files only; the runtime suite uses fake servers
on the same local ports and must run with the real runtime stopped.

## Manual acceptance and limits

Verify Test Here, a moved/rotated NPC, and a newly saved quest giver in the real client.
Repeat from a stopped runtime. Log in first with a different character and confirm
it stays put; then use the configured test character and confirm teleport. Log out/in
again to confirm the consumed request does not teleport repeatedly. Cancel a request
before login and confirm it is ignored. Check module-missing/disabled errors, then
restore the packaged module. Switch to production only after closing WoW and confirm
that the button changes the realm without local saves/restarts.

The agent did not perform these live acceptance checks. Existing limitations:

- A matching WoW client and local account/character must already exist. No automatic
  authentication or character creation is added. First-run selection is expected.
- Linux Windows-client launching requires a working portable Wine launcher at
  `Runtime/Wine/bin/wine`. This repository does not supply Wine or client assets;
  the current development bundle lacks Wine. Windows launches the client directly.
- Client process tracking covers Creator launches; Wine launchers that fork away
  and externally launched clients may require the designer to close WoW manually.
- NPC offsets use the saved position plus a small offset; near walls, ledges, or
  stairs use Test Here to pick a clear landing point. Server map/instance access
  checks remain in force. No terrain synchronization is performed.
- Quest progress is preserved; this stage does not reset completed quests. Use a
  fresh local character or a new quest when testing initial acceptance again.
- Restart currently cycles all three bundled services, intentionally reusing Stage 1.
  Full builds, linkage, real login timing and in-game teleport remain to be verified.
