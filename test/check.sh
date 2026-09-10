#!/bin/bash
#
# Copyright (C) 2026 The pgmoneta community
#
# Redistribution and use in source and binary forms, with or without modification,
# are permitted provided that the following conditions are met:
#
# 1. Redistributions of source code must retain the above copyright notice, this list
# of conditions and the following disclaimer.
#
# 2. Redistributions in binary form must reproduce the above copyright notice, this
# list of conditions and the following disclaimer in the documentation and/or other
# materials provided with the distribution.
#
# 3. Neither the name of the copyright holder nor the names of its contributors may
# be used to endorse or promote products derived from this software without specific
# prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY
# EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
# OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
# THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
# SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT
# OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
# HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR
# TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
# SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
#
# Shell-only backup throughput harness (MCTF removed): sets up PostgreSQL +
# pgmoneta, then times FULL backups and emits machine-readable lines:
#   PERF_RESULT build=<label> backend=<backend> pg=<version> rep=<n> ms=<ms> bytes=<bytes>
#
# No cleanup on exit: CI instances are ephemeral, data/logs are left in place.
#
set -eo pipefail

# Variables
PG_VERSION="${TEST_PG_VERSION:-17}"
export TEST_PG_VERSION="${TEST_PG_VERSION:-17}"

IMAGE_NAME="pgmoneta-test-postgresql$PG_VERSION-rocky10"
CONTAINER_NAME="pgmoneta-test-postgresql$PG_VERSION"

SCRIPT_DIR="$(realpath "$(dirname "${BASH_SOURCE[0]}")")"
PROJECT_DIRECTORY=$(realpath "$SCRIPT_DIR/..")
# PERF_BIN_DIR lets check.sh run binaries built from another tree
# (e.g. a main-branch worktree) while reusing this harness end to end.
EXECUTABLE_DIRECTORY=${PERF_BIN_DIR:-$PROJECT_DIRECTORY/build/src}
TEST_PG_DIRECTORY="$PROJECT_DIRECTORY/test/postgresql/src/postgresql$PG_VERSION"

PGMONETA_ROOT_DIR="/tmp/pgmoneta-test"
BASE_DIR="$PGMONETA_ROOT_DIR/base"
LOG_DIR="$PGMONETA_ROOT_DIR/log"
PG_LOG_DIR="$PGMONETA_ROOT_DIR/pg_log"
RETROSPECT_DIR="$PGMONETA_ROOT_DIR/retrospect"
HOT_STANDBY_DIRECTORY="$PGMONETA_ROOT_DIR/standby"
TABLESPACE_DIR="/tmp/pgmoneta_tblspc"

# BASE DIR holds all the run time data
WORKSPACE_DIRECTORY="$BASE_DIR/pgmoneta-workspace/"
CONFIGURATION_DIRECTORY=$BASE_DIR/conf
RESTORE_DIRECTORY=$BASE_DIR/restore
BACKUP_DIRECTORY=$BASE_DIR/backup
RESOURCE_DIRECTORY=$BASE_DIR/resource
PGCONF_DIRECTORY=$BASE_DIR/pg_conf

CLI_CONF="$CONFIGURATION_DIRECTORY/pgmoneta_cli.conf"

PG_DATABASE=mydb
PG_USER_NAME=myuser
PG_USER_PASSWORD=mypass
PG_REPL_USER_NAME=repl
PG_REPL_PASSWORD=replpass
USER=$(whoami)
MODE="dev"
PORT=6432

# Event loop backend under test (io_uring, epoll, kqueue, auto).
# CI sets TEST_EVENT_BACKEND per matrix job; defaults to auto.
EVENT_BACKEND="${TEST_EVENT_BACKEND:-auto}"

# Use sudo only when not running as root (CI containers run as root)
if [ "$(id -u)" -eq 0 ]; then
  SUDO=""
else
  SUDO="sudo"
fi

# Detect container engine: Docker or Podman
# Called lazily since CI mode does not need containers
detect_container_engine() {
  if command -v podman &> /dev/null; then
    CONTAINER_ENGINE="podman"
    export TMPDIR="${TMPDIR:-$HOME/.local/share/containers/tmp}"
    mkdir -p "$TMPDIR"
  elif command -v docker &> /dev/null; then
    CONTAINER_ENGINE="$SUDO docker"
  else
    echo "Neither Docker nor Podman is installed. Please install one to proceed."
    exit 1
  fi
}

if [ -n "$PGMONETA_TEST_PORT" ]; then
    PORT=$PGMONETA_TEST_PORT
fi
echo "Container port is set to: $PORT"

# Explicit cleanup for the `clean` subcommand only. There is intentionally
# NO trap on EXIT: CI instances are ephemeral, data/logs stay in place.
cleanup() {
   echo "Clean up"
   set +e
   echo "Shutdown pgmoneta"
   if [[ -f "/tmp/pgmoneta.localhost.pid" ]]; then
     if [[ -f "$CLI_CONF" ]]; then
       $EXECUTABLE_DIRECTORY/pgmoneta-cli -c $CLI_CONF shutdown
       sleep 5
     fi
     if [[ -f "/tmp/pgmoneta.localhost.pid" ]]; then
       echo "Force stop pgmoneta"
       kill -9 $(pgrep pgmoneta) 2>/dev/null || true
       rm -f "/tmp/pgmoneta.localhost.pid"
     fi
   fi

   echo "Cleaning up shared memory segments"
   ipcs -m 2>/dev/null | awk '/^0x0/ {print $2}' | xargs -r -I {} ipcrm -m {} 2>/dev/null || true

   echo "Clean Test Resources"
   if [[ -d $PGMONETA_ROOT_DIR ]]; then
      if [[ -d $BASE_DIR ]]; then
        chmod -R u+rwx "$BASE_DIR" 2>/dev/null || true
        rm -Rf "$BASE_DIR"
      fi

      echo "Logs --> $LOG_DIR, $PG_LOG_DIR"
   else
     echo "$PGMONETA_ROOT_DIR not present ... ok"
   fi

   if [[ $MODE != "ci" ]]; then
     echo "Removing postgres $PG_VERSION container"
     remove_postgresql_container
   fi

   if [[ -d "$TABLESPACE_DIR" ]]; then
     chmod -R u+rwx "$TABLESPACE_DIR" 2>/dev/null || true
     rm -Rf "$TABLESPACE_DIR"
   fi

   set -e
}

build_postgresql_image() {
  echo "Building the PostgreSQL $PG_VERSION image $IMAGE_NAME"
  CUR_DIR=$(pwd)
  cd $TEST_PG_DIRECTORY
  set +e
  make clean
  set -e
  make build
  cd $CUR_DIR
}

cleanup_postgresql_image() {
  set +e
  echo "Cleanup of the PostgreSQL $PG_VERSION image $IMAGE_NAME"
  CUR_DIR=$(pwd)
  cd $TEST_PG_DIRECTORY
  make clean
  cd $CUR_DIR
  set -e
}

find_free_port() {
  local p=$1
  while ss -tlnp 2>/dev/null | grep -q ":$p "; do
    p=$((p + 1))
  done
  echo $p
}

start_postgresql_container() {
  # Remove existing container so we can reuse the name
  remove_postgresql_container
  # If the requested port is still occupied by a non-container process, pick a free one
  if ss -tlnp 2>/dev/null | grep -q ":$PORT "; then
    local free_port
    free_port=$(find_free_port $((PORT + 1)))
    echo "Port $PORT is already in use; using port $free_port instead"
    PORT=$free_port
  fi
  $CONTAINER_ENGINE run -p $PORT:5432 -v "$PG_LOG_DIR:/pglog:z" -v "$PGCONF_DIRECTORY:/conf:z"\
  --name $CONTAINER_NAME -d \
  -e PG_DATABASE=$PG_DATABASE \
  -e PG_USER_NAME=$PG_USER_NAME \
  -e PG_USER_PASSWORD=$PG_USER_PASSWORD \
  -e PG_REPL_USER_NAME=$PG_REPL_USER_NAME \
  -e PG_REPL_PASSWORD=$PG_REPL_PASSWORD \
  -e PG_LOG_LEVEL=debug5 \
  $IMAGE_NAME

  echo "Checking PostgreSQL $PG_VERSION container readiness"
  sleep 3
  if $CONTAINER_ENGINE exec $CONTAINER_NAME /usr/pgsql-$PG_VERSION/bin/pg_isready -h localhost -p 5432 >/dev/null 2>&1; then
    echo "PostgreSQL $PG_VERSION is ready!"
  else
    echo "Wait for 10 seconds and retry"
    sleep 10
    if $CONTAINER_ENGINE exec $CONTAINER_NAME /usr/pgsql-$PG_VERSION/bin/pg_isready -h localhost -p 5432 >/dev/null 2>&1; then
      echo "PostgreSQL $PG_VERSION is ready!"
    else
      echo "Printing container logs..."
      $CONTAINER_ENGINE logs $CONTAINER_NAME
      echo ""
      echo "PostgreSQL $PG_VERSION is not ready, exiting"
      cleanup_postgresql_image
      exit 1
    fi
  fi
}

start_postgresql() {
  echo "Setting up PostgreSQL $PG_VERSION directory"
  set +e
  $SUDO rm -Rf /conf /pgconf /pgdata /pgwal
  $SUDO cp -R $TEST_PG_DIRECTORY/root /
  $SUDO ls /root
  $SUDO mkdir -p /conf /pgconf /pgdata /pgwal /pglog
  $SUDO cp -R $TEST_PG_DIRECTORY/conf/* /conf/
  $SUDO ls /conf
  $SUDO chown -R postgres:postgres /conf /pgconf /pgdata /pgwal /pglog
  $SUDO chmod -R 777 /conf /pgconf /pgdata /pgwal /pglog /root
  $SUDO chmod +x /root/usr/bin/run-postgresql-local
  $SUDO mkdir -p /root/usr/local/bin

  if [[ "$PG_VERSION" == "17" ]]; then
    echo "Setting up tablespace location directories (postgres-owned)"
    $SUDO rm -Rf "$TABLESPACE_DIR"
    $SUDO mkdir -p "$TABLESPACE_DIR/ts1" "$TABLESPACE_DIR/ts2"
    $SUDO chown -R postgres:postgres "$TABLESPACE_DIR"
    $SUDO chmod 777 "$TABLESPACE_DIR"
    $SUDO chmod 700 "$TABLESPACE_DIR/ts1" "$TABLESPACE_DIR/ts2"
  fi

  echo "Setting up env variables"
  export PG_DATABASE=${PG_DATABASE}
  export PG_USER_NAME=${PG_USER_NAME}
  export PG_USER_PASSWORD=${PG_USER_PASSWORD}
  export PG_REPL_USER_NAME=${PG_REPL_USER_NAME}
  export PG_REPL_PASSWORD=${PG_REPL_PASSWORD}

  if [ "$(id -u)" -eq 0 ]; then
    runuser -m -u postgres -- /root/usr/bin/run-postgresql-local
  else
    sudo -E -u postgres /root/usr/bin/run-postgresql-local
  fi
  set -e
}

remove_postgresql_container() {
  $CONTAINER_ENGINE stop $CONTAINER_NAME 2>/dev/null || true
  $CONTAINER_ENGINE rm -f $CONTAINER_NAME 2>/dev/null || true
}

pgmoneta_initialize_configuration() {
  touch $CONFIGURATION_DIRECTORY/pgmoneta.conf $CONFIGURATION_DIRECTORY/pgmoneta_users.conf $CONFIGURATION_DIRECTORY/pgmoneta_cli.conf
  echo "Creating pgmoneta.conf, pgmoneta_users.conf and pgmoneta_cli.conf inside $CONFIGURATION_DIRECTORY ... ok"
  cat <<EOF >$CONFIGURATION_DIRECTORY/pgmoneta_cli.conf
# CLI configuration
unix_socket_dir = /tmp/
log_type = file
log_level = info
log_path = $LOG_DIR/pgmoneta-cli.log
EOF
   cat <<EOF >$CONFIGURATION_DIRECTORY/pgmoneta.conf
# Main configuration
[pgmoneta]
host = localhost
metrics = 5001

base_dir = $BACKUP_DIRECTORY

compression = zstd

encryption = aes-256-gcm

retention = 7
retention_interval = 3600 # 1h

log_type = file
log_level = debug5
log_path = $LOG_DIR/pgmoneta.log

unix_socket_dir = /tmp/
create_slot = yes
workspace = $WORKSPACE_DIRECTORY
${PERF_EV_KEY:-ev_backend} = $EVENT_BACKEND

# primary configuration
[primary]
host = localhost
port = $PORT
user = $PG_REPL_USER_NAME
wal_slot = repl
workers = 4
hot_standby = $HOT_STANDBY_DIRECTORY
hot_standby_overrides = $HOT_STANDBY_DIRECTORY/overrides
EOF
   echo "Add test configuration to pgmoneta.conf ... ok"
   if [[ ! -e $HOME/.pgmoneta/master.key ]]; then
     $EXECUTABLE_DIRECTORY/pgmoneta-admin master-key -P $PG_REPL_PASSWORD
   fi
   $EXECUTABLE_DIRECTORY/pgmoneta-admin -f $CONFIGURATION_DIRECTORY/pgmoneta_users.conf -U $PG_REPL_USER_NAME -P $PG_REPL_PASSWORD user add
   echo "Add user $PG_REPL_USER_NAME to pgmoneta_users.conf file ... ok"
   echo "Keep a sample pgmoneta configuration"
   cp $CONFIGURATION_DIRECTORY/pgmoneta.conf $CONFIGURATION_DIRECTORY/pgmoneta.conf.sample
   echo ""
}

# Binary-compatibility gate for mixed trees: the harness in this tree may run
# server binaries from $PERF_BIN_DIR (a main-branch worktree). Refuse to run
# mixed protocol peers whose versions differ. `pgmoneta -V` prints
# "pgmoneta <VERSION>" and exits non-zero by design, hence `|| true`.
# Tolerant when the branch binary is absent (e.g. test-main-baseline CI job
# only builds /tmp/pgmoneta-main): warn with the PERF_BIN_DIR version and
# continue; only fail on mismatch when BOTH binaries exist, or when the
# PERF_BIN_DIR binary is missing.
assert_perf_bin_compat() {
  if [[ -n "${PERF_BIN_DIR:-}" ]]; then
    local branch_ver bin_ver
    if [[ ! -x "$PERF_BIN_DIR/pgmoneta" ]]; then
      echo "ERROR: PERF_BIN_DIR=$PERF_BIN_DIR does not contain an executable pgmoneta" >&2
      exit 1
    fi
    bin_ver=$("$PERF_BIN_DIR/pgmoneta" -V 2>&1 || true)
    if [[ ! -x "$PROJECT_DIRECTORY/build/src/pgmoneta" ]]; then
      echo "WARNING: branch build pgmoneta missing; skipping mixed-tree compatibility gate (PERF_BIN_DIR version: $bin_ver)"
      return 0
    fi
    branch_ver=$("$PROJECT_DIRECTORY/build/src/pgmoneta" -V 2>&1 || true)
    echo "Branch build version:   $branch_ver"
    echo "PERF_BIN_DIR version:   $bin_ver"
    if [[ -z "$bin_ver" ]]; then
      echo "ERROR: PERF_BIN_DIR=$PERF_BIN_DIR does not contain an executable pgmoneta" >&2
      exit 1
    fi
    if [[ -z "$branch_ver" ]]; then
      echo "WARNING: branch build pgmoneta missing; skipping mixed-tree compatibility gate (PERF_BIN_DIR version: $bin_ver)"
      return 0
    fi
    if [[ "$branch_ver" != "$bin_ver" ]]; then
      echo "ERROR: binary mismatch: branch '$branch_ver' vs PERF_BIN_DIR '$bin_ver'; refusing mixed-tree run" >&2
      exit 1
    fi
    echo "Binary compatibility gate passed ($branch_ver)"
  fi
}

# Assert the effective event backend matches the request.
# The marker differs per branch family: io_layer logs
# "Selected backend '<name>'" (src/libpgmoneta/configuration.c), while main
# has no such line and instead logs "libev engine: <name>" from src/main.c
# (engine names like epoll/iouring, see pgmoneta_libev_engine). Both are
# pgmoneta_log_debug, present because the harness sets log_level=debug5.
assert_effective_backend() {
  local key="${PERF_EV_KEY:-ev_backend}"
  local expect=""

  echo "Asserting effective event backend (requested: $EVENT_BACKEND, key family: $key)"
  if [[ ! -f "$LOG_DIR/pgmoneta.log" ]]; then
    echo "ERROR: server log $LOG_DIR/pgmoneta.log not found; cannot verify backend" >&2
    exit 1
  fi
  if [[ "$key" == "ev_backend" ]]; then
    if [[ "$EVENT_BACKEND" == "auto" ]]; then
      expect="Selected backend '"
    else
      expect="Selected backend '$EVENT_BACKEND'"
    fi
  else
    # libev family (main branch): exact assertion where possible; with
    # "auto" the engine is chosen at runtime, so only require the marker.
    if [[ "$EVENT_BACKEND" == "auto" ]]; then
      expect="libev engine: "
    else
      expect="libev engine: $EVENT_BACKEND"
    fi
  fi
  if ! grep -Fq "$expect" "$LOG_DIR/pgmoneta.log"; then
    echo "ERROR: effective backend mismatch: marker '$expect' not found in $LOG_DIR/pgmoneta.log" >&2
    grep -F "Selected backend" "$LOG_DIR/pgmoneta.log" | tail -3 || true
    grep -F "libev engine:" "$LOG_DIR/pgmoneta.log" | tail -3 || true
    exit 1
  fi
  echo "Effective backend verified: $(grep -F "$expect" "$LOG_DIR/pgmoneta.log" | tail -1)"
}

# Returns 0 if the server binaries are stale and need a rebuild, 1 otherwise.
# Never used in PERF_BIN_DIR mode: prebuilt server binaries are inputs.
binaries_stale() {
  if [[ ! -x "$EXECUTABLE_DIRECTORY/pgmoneta" ]]; then
    return 0
  fi
  if [[ -n "$(find "$PROJECT_DIRECTORY/src" "$PROJECT_DIRECTORY/cmake" "$PROJECT_DIRECTORY/CMakeLists.txt" \
                   \( -name '*.c' -o -name '*.h' -o -name 'CMakeLists.txt' -o -name '*.cmake' \) \
                   -newer "$EXECUTABLE_DIRECTORY/pgmoneta" -print -quit 2>/dev/null)" ]]; then
    return 0
  fi
  return 1
}

do_setup() {
  if [[ $MODE != "ci" ]]; then
    echo "Building PostgreSQL $PG_VERSION image if necessary"
    if $CONTAINER_ENGINE image inspect "$IMAGE_NAME" >/dev/null 2>&1; then
      echo "Image $IMAGE_NAME exists, skip building"
    else
      build_postgresql_image
    fi
  fi

   echo "Preparing the pgmoneta directory"
   chmod -R u+rwx "$PGMONETA_ROOT_DIR" 2>/dev/null || true
   rm -Rf "$PGMONETA_ROOT_DIR"
   mkdir -p "$PGMONETA_ROOT_DIR"
   mkdir -p "$LOG_DIR" "$PG_LOG_DIR" "$BASE_DIR" "$RETROSPECT_DIR" "$HOT_STANDBY_DIRECTORY"
  mkdir -p "$RESTORE_DIRECTORY" "$BACKUP_DIRECTORY" "$CONFIGURATION_DIRECTORY" "$WORKSPACE_DIRECTORY" "$RESOURCE_DIRECTORY" "$PGCONF_DIRECTORY"
  cp -R "$PROJECT_DIRECTORY/test/resource" $BASE_DIR
  cp -R $TEST_PG_DIRECTORY/conf/* $PGCONF_DIRECTORY/
  chmod -R 777 $PG_LOG_DIR
  chmod -R 777 $PGCONF_DIRECTORY

  if [[ -n "${PERF_BIN_DIR:-}" ]]; then
    # Baseline mode: server binaries are prebuilt inputs from another tree —
    # never rebuild them, just verify they exist (fail loud, no build).
    for bin in pgmoneta pgmoneta-cli pgmoneta-admin; do
      if [[ ! -x "$EXECUTABLE_DIRECTORY/$bin" ]]; then
        echo "ERROR: PERF_BIN_DIR=$PERF_BIN_DIR does not contain an executable $bin" >&2
        exit 1
      fi
    done
    echo "PERF_BIN_DIR mode: using prebuilt server binaries from $EXECUTABLE_DIRECTORY"
  elif binaries_stale; then
    echo "Building pgmoneta (binaries missing or sources changed)"
    mkdir -p "$PROJECT_DIRECTORY/build"
    cd "$PROJECT_DIRECTORY/build"
    export CC=$(which clang)
    cmake -DCMAKE_C_COMPILER=clang -DCMAKE_BUILD_TYPE=Debug -DDOCS=FALSE ..
    make -j$(nproc)
    cd ..
  else
    echo "pgmoneta binaries up to date, skipping build"
  fi

  if [[ $MODE == "ci" ]]; then
    echo "Start PostgreSQL $PG_VERSION locally"
    start_postgresql
  else
    echo "Start PostgreSQL $PG_VERSION container"
    start_postgresql_container
    if [[ "$PG_VERSION" == "17" ]]; then
      echo "Preparing host tablespace directory for hot standby copies"
      chmod -R u+rwx "$TABLESPACE_DIR" 2>/dev/null || true
      rm -Rf "$TABLESPACE_DIR"
      mkdir -p "$TABLESPACE_DIR"
      chmod 777 "$TABLESPACE_DIR"
    fi
  fi

  echo "Initialize pgmoneta"
  pgmoneta_initialize_configuration
}

start_pgmoneta_server() {
   echo "=== pgmoneta server start ==="

   if pgrep -f pgmoneta >/dev/null 2>&1 || [[ -f "/tmp/pgmoneta.localhost.pid" ]]; then
      echo "Cleaning up any existing pgmoneta processes"
      if [[ -f "/tmp/pgmoneta.localhost.pid" ]]; then
         $EXECUTABLE_DIRECTORY/pgmoneta-cli -c $CLI_CONF shutdown 2>/dev/null || true
         sleep 3
      fi
      if pgrep -f pgmoneta >/dev/null 2>&1; then
         echo "Killing existing pgmoneta processes"
         pkill -9 -f "${EXECUTABLE_DIRECTORY}/pgmoneta " || true
         pkill -9 -f "${EXECUTABLE_DIRECTORY}/pgmoneta-cli " || true
         sleep 2
      fi
      rm -f "/tmp/pgmoneta.localhost.pid"

      echo "Cleaning up shared memory segments"
      ipcs -m 2>/dev/null | awk '/^0x0/ {print $2}' | xargs -r -I {} ipcrm -m {} 2>/dev/null || true
      sleep 1
   fi

   echo "=== pgmoneta.conf ==="
   cat $CONFIGURATION_DIRECTORY/pgmoneta.conf

   echo "=== pgmoneta version ==="
   $EXECUTABLE_DIRECTORY/pgmoneta -V 2>&1 || true

   echo "=== kernel io_uring state ==="
   cat /proc/sys/kernel/io_uring_disabled 2>/dev/null || echo "(no /proc/sys/kernel/io_uring_disabled; io_uring not restricted by sysctl)"

   echo "Starting pgmoneta server in daemon mode"
   $EXECUTABLE_DIRECTORY/pgmoneta -c $CONFIGURATION_DIRECTORY/pgmoneta.conf -u $CONFIGURATION_DIRECTORY/pgmoneta_users.conf -d
   echo "Wait for pgmoneta to be ready"

   for i in 1 2 3 4 5; do
      echo "--- start attempt $i ---"
      echo "pid file: $(cat /tmp/pgmoneta.localhost.pid 2>/dev/null || echo MISSING)"
      ps aux | grep -F "$EXECUTABLE_DIRECTORY/pgmoneta" | grep -v grep || echo "(pgmoneta process not listed)"
      if [[ -f "/tmp/pgmoneta.localhost.pid" ]]; then
         if $EXECUTABLE_DIRECTORY/pgmoneta-cli -c $CLI_CONF status details; then
            echo "pgmoneta server started ... ok"
            break
         fi
      fi
      if [[ $i -eq 5 ]]; then
         echo "pgmoneta server not started ... not ok"
         echo "Checking logs:"
         tail -n 50 $LOG_DIR/pgmoneta.log 2>/dev/null || echo "Log file not found"
         exit 1
      fi
      tail -n 50 $LOG_DIR/pgmoneta.log 2>/dev/null || echo "Log file not found yet"
      sleep 2
   done

   echo "Wait for WAL streaming to be ready"
   for i in 1 2 3 4 5; do
      if $EXECUTABLE_DIRECTORY/pgmoneta-cli -c $CLI_CONF status details -F json 2>/dev/null | \
         grep -q '"WalStreaming": true'; then
         echo "WAL streaming ready ... ok"
         break
      fi
      if [[ $i -eq 5 ]]; then
         echo "WAL streaming not ready ... not ok"
         echo "Checking logs:"
         tail -n 50 $LOG_DIR/pgmoneta.log 2>/dev/null || echo "Log file not found"
         exit 1
      fi
      echo "Waiting for WAL streaming to be ready"
      sleep 2
   done

   assert_effective_backend
}

# Wait until the WAL streamer is quiescent: entry count AND total size stable
# across consecutive 2s polls. A missing dir counts as "not yet" (the streamer
# may not have created it right after server start), never as success.
# Returns 0 on quiescence, 1 on timeout (fail loud at the call site).
perf_wait_wal_quiescent() {
   local waldir=$1
   local timeout_secs=$2
   local waited=0
   local last_count=-1
   local last_size=0
   local stable=0
   local count size

   echo "Waiting for WAL streamer quiescence in $waldir (timeout ${timeout_secs}s) ..."
   while [[ $waited -lt $timeout_secs ]]; do
      if [[ -d "$waldir" ]]; then
         count=$(find "$waldir" -mindepth 1 2>/dev/null | wc -l) || count=-1
         size=$(du -sb "$waldir" 2>/dev/null | cut -f1) || size=-1
         if [[ "$count" -ge 0 && "$count" -eq "$last_count" && "$size" -eq "$last_size" ]]; then
            stable=$((stable + 1))
            if [[ $stable -ge 2 ]]; then
               echo "WAL streamer quiescent (entries=$count size=$size) ... ok"
               return 0
            fi
         else
            stable=0
            if [[ "$count" -ge 0 ]]; then
               last_count=$count
               last_size=$size
            fi
         fi
      else
         echo "WAL dir $waldir not present yet, waiting ..."
      fi
      sleep 2
      waited=$((waited + 2))
   done
   echo "ERROR: WAL streamer never quiesced in $waldir (timeout ${timeout_secs}s) - infra failure, not speed" >&2
   return 1
}

# Number of backups for primary, parsed from list-backup JSON
# ({ "Response": { "NumberOfBackups": N, ... } }). Fails loud on parse errors.
perf_backup_count() {
   $EXECUTABLE_DIRECTORY/pgmoneta-cli -c $CLI_CONF list-backup primary -s asc -F json \
      | python3 -c 'import json,sys; print(json.load(sys.stdin)["Response"]["NumberOfBackups"])'
}

# Delete every backup so each rep is FULL (max 64 deletes, 1s apart).
# Fails loud when backups remain afterwards.
perf_clear_backups() {
   local rep=$1
   local i count
   for i in $(seq 1 64); do
      count=$(perf_backup_count) || { echo "ERROR: could not list backups before rep $rep" >&2; exit 1; }
      if [[ -z "$count" ]]; then
         echo "ERROR: could not list backups before rep $rep" >&2
         exit 1
      fi
      if [[ "$count" -eq 0 ]]; then
         return 0
      fi
      $EXECUTABLE_DIRECTORY/pgmoneta-cli -c $CLI_CONF delete --force primary oldest
      sleep 1
   done
   count=$(perf_backup_count) || { echo "ERROR: could not list backups before rep $rep" >&2; exit 1; }
   if [[ -z "$count" ]]; then
      echo "ERROR: could not list backups before rep $rep" >&2
      exit 1
   fi
   if [[ "$count" -ne 0 ]]; then
      echo "ERROR: $count backup(s) retained/undeletable after 64 delete attempts before rep $rep" >&2
      exit 1
   fi
}

# Backup size in bytes: recursive BackupSize search in the info JSON, then in
# the list-backup JSON, then du -sb of the newest backup dir as last resort.
perf_backup_bytes() {
   local info_json="$1"
   local list_json="$2"
   local bytes newest_dir
   bytes=$(echo "$info_json" | python3 -c '
import json,sys
def find(o):
    if isinstance(o, dict):
        for k, v in o.items():
            if k == "BackupSize":
                return v
            r = find(v)
            if r is not None:
                return r
    elif isinstance(o, list):
        for v in o:
            r = find(v)
            if r is not None:
                return r
    return None
print(find(json.load(sys.stdin)) or 0)
' 2>/dev/null) || bytes=0
   if [[ -z "$bytes" || "$bytes" -eq 0 ]]; then
      bytes=$(echo "$list_json" | python3 -c '
import json,sys
def find(o):
    if isinstance(o, dict):
        for k, v in o.items():
            if k == "BackupSize":
                return v
            r = find(v)
            if r is not None:
                return r
    elif isinstance(o, list):
        for v in o:
            r = find(v)
            if r is not None:
                return r
    return None
doc = json.load(sys.stdin)["Response"]["Backups"]
print(max((find(b) or 0 for b in doc), default=0))
' 2>/dev/null) || bytes=0
   fi
   if [[ -z "$bytes" || "$bytes" -eq 0 ]]; then
      echo "JSON byte parse failed, falling back to du -sb"
      newest_dir=$(find "$BACKUP_DIRECTORY/primary" -mindepth 1 -maxdepth 1 ! -name wal -type d -printf '%T@ %p\n' 2>/dev/null | sort -n | tail -1 | cut -d' ' -f2-)
      if [[ -n "$newest_dir" && -d "$newest_dir" ]]; then
         bytes=$(du -sb "$newest_dir" | cut -f1)
      fi
   fi
   echo "$bytes"
}

run_perf_shell() {
   local build_label="${PERF_BUILD_LABEL:-unknown}"
   local ev_key="${PERF_EV_KEY:-ev_backend}"
   local scale_raw="${PERF_SCALE:-50}"
   local reps_raw="${PERF_REPS:-3}"
   local scale reps rows r
   local seed_bytes server_version waldir
   local start_ms end_ms ms list_json info_json count bytes mb mbs

   echo "=== perf env ==="
   echo "PERF_BUILD_LABEL=$build_label"
   echo "EVENT_BACKEND=$EVENT_BACKEND"
   echo "PG_VERSION=$PG_VERSION"
   echo "PERF_EV_KEY=$ev_key"
   echo "EXECUTABLE_DIRECTORY=$EXECUTABLE_DIRECTORY"

   case "$scale_raw" in
      ''|*[!0-9]*)
         echo "ERROR: PERF_SCALE is not numeric: '$scale_raw'" >&2
         exit 1
         ;;
   esac
   scale=$scale_raw
   if [[ $scale -lt 1 ]]; then scale=1; fi
   if [[ $scale -gt 200 ]]; then scale=200; fi
   case "$reps_raw" in
      ''|*[!0-9]*)
         echo "ERROR: PERF_REPS is not numeric: '$reps_raw'" >&2
         exit 1
         ;;
   esac
   reps=$reps_raw
   if [[ $reps -lt 1 ]]; then reps=1; fi
   if [[ $reps -gt 10 ]]; then reps=10; fi
   echo "SCALE=$scale"
   echo "REPS=$reps"

   rows=$((scale * 100000))

   echo "=== perf seed: DROP + CREATE perf_data ($rows rows) ==="
   psql -h localhost -p "$PORT" -U "$PG_USER_NAME" -d "$PG_DATABASE" -v ON_ERROR_STOP=1 -tA \
      -c "DROP TABLE IF EXISTS perf_data; CREATE TABLE perf_data AS SELECT g, repeat(md5(g::text),4) FROM generate_series(1, $rows) g;"

   echo "=== perf probe: pg_database_size ==="
   seed_bytes=$(psql -h localhost -p "$PORT" -U "$PG_USER_NAME" -d "$PG_DATABASE" -v ON_ERROR_STOP=1 -tA \
      -c "SELECT pg_database_size('mydb');") || { echo "ERROR: pg_database_size probe failed" >&2; exit 1; }
   echo "seed bytes=$seed_bytes"

   echo "=== perf probe: server_version_num ==="
   server_version=$(psql -h localhost -p "$PORT" -U "$PG_USER_NAME" -d "$PG_DATABASE" -v ON_ERROR_STOP=1 -tA \
      -c "SELECT current_setting('server_version_num');") || { echo "ERROR: server version probe failed" >&2; exit 1; }
   echo "server_version_num=$server_version"

   if [[ "$server_version" -ge 150000 ]]; then
      echo "=== perf seed checkpoint ==="
      psql -h localhost -p "$PORT" -U "$PG_USER_NAME" -d "$PG_DATABASE" -v ON_ERROR_STOP=1 -tA -c "CHECKPOINT;"
   else
      echo "PERF_HUMAN note: PG < 15 has no pg_checkpoint role; skipping seed checkpoint, relying on quiesce drain"
   fi

   waldir="$BACKUP_DIRECTORY/primary/wal"
   echo "=== perf WAL quiesce after seed (timeout 300s) ==="
   perf_wait_wal_quiescent "$waldir" 300 || exit 1

   for r in $(seq 1 "$reps"); do
      echo "=== perf rep $r/$reps: WAL quiesce (timeout 60s) ==="
      perf_wait_wal_quiescent "$waldir" 60 || exit 1

      echo "=== perf rep $r/$reps: clear prior backups ==="
      perf_clear_backups "$r"

      echo "=== perf rep $r/$reps: backup primary ==="
      start_ms=$(date +%s%3N)
      $EXECUTABLE_DIRECTORY/pgmoneta-cli -c $CLI_CONF backup primary
      end_ms=$(date +%s%3N)
      ms=$((end_ms - start_ms))
      echo "rep $r backup ms=$ms"
      if [[ "$ms" -le 0 ]]; then
         echo "ERROR: non-positive backup latency on rep $r (ms=$ms)" >&2
         exit 1
      fi

      echo "=== perf rep $r/$reps: list-backup ==="
      list_json=$($EXECUTABLE_DIRECTORY/pgmoneta-cli -c $CLI_CONF list-backup primary -s asc -F json) \
         || { echo "ERROR: list-backup failed on rep $r" >&2; exit 1; }
      echo "$list_json"
      count=$(echo "$list_json" | python3 -c 'import json,sys; print(json.load(sys.stdin)["Response"]["NumberOfBackups"])') \
         || { echo "ERROR: could not parse backup count on rep $r" >&2; exit 1; }
      if [[ -z "$count" ]]; then
         echo "ERROR: could not parse backup count on rep $r" >&2
         exit 1
      fi
      if [[ "$count" -ne 1 ]]; then
         echo "ERROR: expected exactly 1 backup after rep $r, got $count" >&2
         exit 1
      fi

      echo "=== perf rep $r/$reps: info primary newest ==="
      info_json=$($EXECUTABLE_DIRECTORY/pgmoneta-cli -c $CLI_CONF info primary newest -F json) \
         || { echo "ERROR: info failed on rep $r" >&2; exit 1; }
      echo "$info_json"
      bytes=$(perf_backup_bytes "$info_json" "$list_json")
      echo "rep $r backup bytes=$bytes"
      if [[ -z "$bytes" || "$bytes" -eq 0 ]]; then
         echo "ERROR: backup size anomaly on rep $r (bytes=$bytes)" >&2
         exit 1
      fi

      echo "PERF_RESULT build=$build_label backend=$EVENT_BACKEND pg=$PG_VERSION rep=$r ms=$ms bytes=$bytes"
      mb=$(awk "BEGIN {printf \"%.1f\", $bytes/1048576.0}")
      mbs=$(awk "BEGIN {printf \"%.1f\", ($bytes/1048576.0)/($ms/1000.0)}")
      echo "PERF_HUMAN rep $r: $ms ms, $mb MB ($mbs MB/s)"
   done
}

usage() {
   echo "Usage: $0 [sub-command]"
   echo "Subcommands:"
   echo " build          Set up environment (image, build, PostgreSQL, pgmoneta) without running perf"
   echo " clean          Clean up test suite environment and remove PostgreSQL image"
   echo " setup          Install dependencies and build PostgreSQL image"
   echo " ci             Run backup perf in CI mode (local PostgreSQL)"
   echo "Note: PGMONETA_TEST_PORT overrides the default container port (6432)"
   echo "Examples:"
   echo "  $0                           Run backup perf (dev, container PostgreSQL)"
   echo "  $0 build                     Set up environment only"
   echo "  PGMONETA_TEST_PORT=6433 $0   Use port 6433 for the PostgreSQL container"
   exit 1
}

run_perf() {
  if [[ ! -f "$CONFIGURATION_DIRECTORY/pgmoneta.conf" ]] || [[ ! -x "$EXECUTABLE_DIRECTORY/pgmoneta" ]]; then
    echo "Environment incomplete, running build"
    do_setup
  elif [[ -z "${PERF_BIN_DIR:-}" ]] && binaries_stale; then
    echo "Sources changed, running build"
    do_setup
  else
    echo "Environment already ready, skipping build"
  fi
  assert_perf_bin_compat
  start_pgmoneta_server
  run_perf_shell
}

SUBCOMMAND=""
while [[ $# -gt 0 ]]; do
   case "$1" in
      build)
         [[ -n "$SUBCOMMAND" ]] && usage
         SUBCOMMAND="build"
         shift
         ;;
      setup)
         [[ -n "$SUBCOMMAND" ]] && usage
         SUBCOMMAND="setup"
         shift
         ;;
      clean)
         [[ -n "$SUBCOMMAND" ]] && usage
         SUBCOMMAND="clean"
         shift
         ;;
      ci)
         [[ -n "$SUBCOMMAND" ]] && usage
         SUBCOMMAND="ci"
         shift
         ;;
      -h|--help)
         usage
         ;;
      -*)
         echo "Invalid option: $1"
         usage
         ;;
      *)
         echo "Invalid parameter: $1"
         usage
         ;;
   esac
done

if [[ "$SUBCOMMAND" == "build" ]]; then
   detect_container_engine
   do_setup
   exit 0
fi
if [[ "$SUBCOMMAND" == "setup" ]]; then
   detect_container_engine
   build_postgresql_image
   $SUDO dnf install -y \
      clang \
      clang-analyzer \
      cmake \
      make \
      liburing-devel pkgconf-pkg-config \
      openssl openssl-devel \
      systemd systemd-devel \
      zlib zlib-devel \
      libzstd libzstd-devel \
      lz4 lz4-devel \
      libssh libssh-devel \
      libatomic \
      bzip2 bzip2-devel \
      libarchive libarchive-devel \
      libasan libasan-static \
       libyaml-devel \
       ncurses-devel \
       check check-devel check-static
   exit 0
fi
if [[ "$SUBCOMMAND" == "clean" ]]; then
   detect_container_engine
   cleanup
   cleanup_postgresql_image
   rm -Rf $PGMONETA_ROOT_DIR
   exit 0
fi
if [[ "$SUBCOMMAND" == "ci" ]]; then
   MODE="ci"
   PORT=5432
   run_perf
   exit 0
fi
# Default: run backup perf (dev, container PostgreSQL)
detect_container_engine
run_perf
