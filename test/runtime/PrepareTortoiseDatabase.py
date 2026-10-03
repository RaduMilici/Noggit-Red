#!/usr/bin/env python3
"""Developer-only bundle preparation using the matching server's own updater.
Runs only when explicitly invoked; normal Creator startup never runs updates.
Original databases remain untouched unless the server initializes and exits cleanly.
"""
import argparse
import datetime
import os
from pathlib import Path
import re
import shutil
import socket
import subprocess
import time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('creator', type=Path)
parser.add_argument('source', type=Path, help='Matching Tortoise source checkout')
args = parser.parse_args()
root, source = args.creator.resolve(), args.source.resolve()
workspace = root / 'Workspace'
stamp = datetime.datetime.now().strftime('%Y%m%d-%H%M%S')
lock = root / 'Database/runtime.lock'
fd = os.open(lock, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
process_name = Path('/proc/self/comm').read_text().strip()
os.write(fd, f'{os.getpid()}\n{process_name}\n{socket.gethostname()}\n'.encode())
os.close(fd)
db = world = None
log = None
client = [str(root / 'Runtime/MariaDB/bin/mariadb'),
          '--defaults-file=' + str(workspace / 'client.cnf')]
env = os.environ.copy()
env['LD_LIBRARY_PATH'] = str(root / 'Runtime/lib') + ':' + str(root / 'Runtime/MariaDB/lib')

def sql(statement):
    return subprocess.run(client + ['--connect-timeout=1', '--batch', '--execute=' + statement],
                          env=env, capture_output=True, timeout=10)

def setting(text, key, value):
    pattern = r'^\s*' + re.escape(key) + r'\s*=.*$'
    line = key + ' = ' + value
    return re.sub(pattern, lambda _: line, text, flags=re.M) if re.search(pattern, text, re.M) else text + '\n' + line + '\n'

try:
    # Fail before copying or launching anything if another service owns these ports.
    for port in (13306, 18085):
        with socket.socket() as check:
            check.bind(('127.0.0.1', port))
    stage = root / 'Database' / ('prepare-' + stamp)
    print('Copying the current database; originals will be preserved.', flush=True)
    shutil.copytree(root / 'Database/data', stage)
    updates = workspace / ('prepare-updates-' + stamp)
    shutil.copytree(source / 'sql/database_updates', updates)
    (updates / 'auth').mkdir(exist_ok=True)
    db_config = workspace / 'prepare-mariadb.cnf'
    db_config.write_text(setting((workspace / 'mariadb.cnf').read_text(), 'datadir', '"' + str(stage) + '"'))
    config = (workspace / 'mangosd.conf').read_text()
    config = setting(config, 'Database.AutoUpdate.Enabled', '1')
    config = setting(config, 'Database.AutoUpdate.Path', '"' + str(updates) + '"')
    world_config = workspace / 'prepare-mangosd.conf'
    world_config.write_text(config)
    logfile = workspace / 'logs' / ('prepare-' + stamp + '.log')
    log = logfile.open('w')
    print('Preparation log:', logfile, flush=True)
    db = subprocess.Popen([str(root / 'Runtime/MariaDB/bin/mariadbd'), '--defaults-file=' + str(db_config)],
                          cwd=workspace, env=env, stdout=log, stderr=log)
    for _ in range(120):
        if db.poll() is not None:
            raise RuntimeError('Preparation database exited; see preparation log.')
        if sql('SELECT 1').returncode == 0:
            break
        time.sleep(.25)
    else:
        raise RuntimeError('Preparation database did not become ready.')
    print('Running the native Tortoise updater and checking world initialization...', flush=True)
    world = subprocess.Popen([str(root / 'Runtime/mangosd/mangosd'), '-c', str(world_config)],
                             cwd=workspace, env=env, stdin=subprocess.PIPE, stdout=log, stderr=log)
    # Console is started after world initialization. Queue a clean exit for that point.
    world.communicate(b'server shutdown 0\n', timeout=900)
    if world.returncode != 0:
        raise RuntimeError(f'Tortoise exited with code {world.returncode}; originals are unchanged. See {logfile}')
    if sql('SELECT script_name FROM mangos.spell_template LIMIT 1').returncode != 0:
        raise RuntimeError('Updated schema check failed; originals are unchanged.')
    if sql('SHUTDOWN').returncode != 0:
        raise RuntimeError('Could not cleanly stop preparation database.')
    db.wait(timeout=30)
    if db.returncode != 0:
        raise RuntimeError('Preparation database did not exit cleanly.')
    # Prepare both copies before replacing either original.
    seed_stage = root / 'Database' / ('prepared-seed-' + stamp)
    shutil.copytree(stage, seed_stage)
    (root / 'Database/data').rename(root / 'Database' / ('data-before-' + stamp))
    stage.rename(root / 'Database/data')
    (root / 'Runtime/DatabaseSeed').rename(root / 'Database' / ('seed-before-' + stamp))
    seed_stage.rename(root / 'Runtime/DatabaseSeed')
    print('Database and first-run seed updated. Original copies are preserved in Database/.', flush=True)
finally:
    if world is not None and world.poll() is None:
        world.terminate()
        try:
            world.wait(timeout=15)
        except subprocess.TimeoutExpired:
            world.kill()
            world.wait()
    if db is not None and db.poll() is None:
        db.terminate()
        try:
            db.wait(timeout=30)
        except subprocess.TimeoutExpired:
            db.kill()
            db.wait()
    if log is not None:
        log.close()
    lock.unlink()
