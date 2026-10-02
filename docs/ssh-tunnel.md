# Connecting Noggit to a remote database through an SSH tunnel

Noggit can reach a MySQL world database on a remote server (for example an AWS Lightsail
instance) **without opening port 3306 to the internet**. When the SSH tunnel is enabled for a
project, Noggit starts the system OpenSSH client in the background, forwards a random local port on
`127.0.0.1` to MySQL on the server, and sends every database action (UID storage, creature/gameobject
spawns, Creature Editor, SQL apply, DB reset) through it. The tunnel is closed when the project
closes or Noggit exits.

Settings are stored **per project**, like the rest of the MySQL settings.

## For users

You need from your administrator:

- the server's IP address or host name,
- your **own** SSH username and private key file (never a shared one),
- the MySQL username, password and world database name,
- ideally, the server's SSH host-key fingerprint (`SHA256:...`) so you can verify it.

Requirements: the OpenSSH client (`ssh`, `ssh-keyscan`). Ubuntu: `sudo apt install openssh-client`.
Windows 10/11: enable the optional feature "OpenSSH Client".

1. Open the project, then **Settings → MySQL**.
2. Tick the MySQL box and fill in **User**, **Password** and **World DB**.
   (**Server** and **Port** are for direct connections and are greyed out while tunneling.)
3. Tick **SSH tunnel** and fill in **SSH host**, **SSH user** and **Private key** (use *Browse...* and
   pick the file *without* `.pub`). Leave *SSH port* at 22 unless told otherwise.
4. Leave **Advanced** at its defaults (`127.0.0.1` / `3306`) unless your administrator says MySQL runs
   elsewhere. Paste the fingerprint into *Expected host key* if you were given one.
5. Click **Test SSH tunnel**. The first time, Noggit shows the server's fingerprint: compare it with
   the one from your administrator and only then click *trust and connect*.
6. Click **Test MySQL connection**, then **Save**.

Your key file must only be readable by you. If Noggit says otherwise, run:

```bash
chmod 600 ~/.ssh/your_key
```

**Passphrase-protected keys:** Noggit never asks for, stores or passes a passphrase. Unlock the key in
ssh-agent before starting Noggit (keep the matching `.pub` file next to the key):

```bash
ssh-add ~/.ssh/your_key
```

### Troubleshooting

| Message | What to do |
|---|---|
| OpenSSH client not found | Install `openssh-client` (see above). |
| Private key can be read by other users | `chmod 600` the key file. |
| Not an OpenSSH private key | You picked the `.pub` file or a PuTTY `.ppk`; pick the private key / export it to OpenSSH format in PuTTYgen. |
| Server rejected the key | Wrong SSH username, or your public key is not installed on the server. |
| Server's host key has CHANGED | **Stop.** Contact your administrator. Only after they confirm the server was rebuilt, use *Forget trusted host key* and verify the new fingerprint. |
| Could not reach the SSH server | Wrong address, server down, or its firewall does not allow SSH from your IP. |
| Tunnel works, but MySQL could not be reached | MySQL is not running on the server, or *Database host/port on server* is wrong. |
| MySQL rejected the login / World DB could not be opened | Check the MySQL user, password and database name. |

Trusted host keys are kept in Noggit's own `known_hosts` file (under the application data folder,
`ssh/known_hosts`), not in `~/.ssh/known_hosts`. Noggit ignores `~/.ssh/config` for this tunnel.

### Server helpers (tortoise-deploy)

For a server run with [tortoise-deploy](https://github.com/mserajnik/tortoise-deploy), Noggit can use
the same SSH connection for two more things. They run shell commands on the server, so they need a key
with shell access (the server owner's); the restricted per-user keys below only allow the tunnel.

- **Settings -> MySQL -> SSH tunnel -> Server (tortoise-deploy):** *tortoise-deploy folder on server* is
  the folder with `compose.yaml`, relative to the SSH user's home (default `tortoise-deploy`) or absolute.
- **Copy SQL exports to the server.** Every database write (spawns, NPCs, quests, items) keeps a copy in
  the project's `sql_exports/`. With *After saving to the database, copy sql_exports/ to the server*
  ticked, Noggit mirrors that whole folder to `<tortoise-deploy>/storage/database/custom-sql/noggit/`
  after each write; *Assist -> Copy SQL exports to server* does it on demand. tortoise-deploy replays
  that folder on every database start, so your changes survive the world database being re-created
  during an update. The folder is mirrored: a deleted NPC's file disappears on the server too. Your own
  files directly in `custom-sql/` are never touched. Files written by *Export SQL* without applying them
  are mirrored as well, so delete exports you do not want applied.
- **Assist -> Restart world server...** runs `docker compose restart mangosd` in the tortoise-deploy
  folder. The world server only loads spawns at startup, so new spawns appear after this. Everyone
  online is disconnected for about 2-3 minutes.

## For administrators

**Issue one SSH key per user**, and restrict it so it can do nothing except forward to MySQL.
Never hand out the Lightsail default key, your personal key, or one key shared by several people:
anyone holding an unrestricted key has a shell on the server, and a shared key cannot be revoked for
one person.

1. Have each user generate their own key pair on their computer and send you only the `.pub` file:

   ```bash
   ssh-keygen -t ed25519 -C "noggit-alice" -f ~/.ssh/noggit_alice
   ```

2. Create a dedicated, unprivileged account on the server (e.g. `noggit`) and add each user's public
   key to its `~/.ssh/authorized_keys` **with restrictions**, one line per user:

   ```
   restrict,port-forwarding,permitopen="127.0.0.1:3306",command="/usr/sbin/nologin" ssh-ed25519 AAAAC3Nza...user-key... noggit-alice
   ```

   - `restrict` disables PTY allocation, agent forwarding, X11 forwarding, user rc and all port
     forwarding; `port-forwarding` then re-enables only local forwarding, and `permitopen` limits it
     to MySQL on the server itself.
   - `command="/usr/sbin/nologin"` prevents a shell even if one is requested (Noggit runs `ssh -N`, so
     it never asks for one).
   - Remove a user's line to revoke their access.

   Optionally enforce the same in `/etc/ssh/sshd_config` for that account:

   ```
   Match User noggit
       AllowTcpForwarding local
       PermitOpen 127.0.0.1:3306
       X11Forwarding no
       AllowAgentForwarding no
       PermitTTY no
       ForceCommand /usr/sbin/nologin
   ```

3. Keep MySQL bound to `127.0.0.1` (`bind-address = 127.0.0.1`) and **do not** open port 3306 in the
   Lightsail/AWS firewall. Only SSH (22) needs to be reachable. Give each user a MySQL account with
   only the privileges they need on the world database.
4. Send users the host-key fingerprint over a trusted channel so they can verify it on first
   connect (run this on the server):

   ```bash
   ssh-keygen -lf /etc/ssh/ssh_host_ed25519_key.pub
   ```

Never commit private keys to this (or any) repository.

## For developers

- Code: `src/noggit/ssh/` (`SshTunnelConfig` = settings, validation, stderr classification,
  fingerprints; `SshTunnelManager` = the QProcess state machine; `SshTunnelGlobal.cpp` = app instance
  and host-key dialog; `SshRemote` = one-shot remote commands, tar builder). MySQL integration:
  `connect()` / `probeConnection()` in `src/mysql/mysql.cpp`. Server helpers UI:
  `src/noggit/ui/content/ServerSync.cpp`.
- Remote commands go through the remote shell, unlike the local argument list: every path placed in
  one is validated by `normalizeRemoteDir` (letters, digits, `._-/`, no `..`, no leading `-`) and
  single-quoted. The export files travel as a tar stream on stdin, never on the command line.
- ssh is started with an argument list (no shell); hosts/users are validated so they cannot be parsed
  as ssh options, and the destination follows `--`. `StrictHostKeyChecking=yes` against the dedicated
  `known_hosts`; `BatchMode=yes` so ssh never prompts.
- Tests (no server needed, fake `ssh` / `ssh-keyscan` scripts):

  ```bash
  cmake -S test/ssh_tunnel -B build-ssh-tests && cmake --build build-ssh-tests && ctest --test-dir build-ssh-tests --output-on-failure
  ```
