#!/bin/bash
#
# Sequential same-box perf controller: one runner per PG version runs
# 8 cells back-to-back so every comparison is same-machine.
#
# Driven entirely by env: PG_VERSION (+ optional PERF_SCALE/PERF_REPS,
# LOOP_DRY_RUN). Cell table is fixed inside this script.
#
# Order: bases -> mains -> flags -> base-repeat drift check.
#   1 io_layer/epoll/base
#   2 io_layer/io_uring/base
#   3 main/epoll/base
#   4 main/iouring/base
#   5 io_layer/io_uring/multishot
#   6 io_layer/io_uring/zerocopy
#   7 io_layer/io_uring/both
#   8 io_layer/io_uring/base REPEAT (drift check)
#
set -eo pipefail

PG="${PG_VERSION:-${TEST_PG_VERSION:-17}}"
SCRIPT_DIR="$(realpath "$(dirname "${BASH_SOURCE[0]}")")"
PROJECT_DIRECTORY="$(realpath "$SCRIPT_DIR/..")"
DRY="${LOOP_DRY_RUN:-0}"
export LOOP_DRY_RUN="$DRY"

# Always run cells from the repo root so ./test/check.sh resolves both
# locally and in CI (workflow does `cd $GITHUB_WORKSPACE` first anyway).
cd "$PROJECT_DIRECTORY" || exit 1

echo "=== sequential loop: PG=$PG DRY=$DRY ==="

# Fixed cell table (parallel arrays, 8 cells).
LABELS=("io_layer-epoll-base" "io_layer-io_uring-base" "main-epoll-base" "main-iouring-base" "io_layer-io_uring-multishot" "io_layer-io_uring-zerocopy" "io_layer-io_uring-both" "io_layer-io_uring-base-repeat")
BUILDS=("io_layer" "io_layer" "main" "main" "io_layer" "io_layer" "io_layer" "io_layer")
BACKENDS=("epoll" "io_uring" "epoll" "iouring" "io_uring" "io_uring" "io_uring" "io_uring")
FLAGS=("base" "base" "base" "base" "multishot" "zerocopy" "both" "base")
EVKEYS=("ev_backend" "ev_backend" "libev" "libev" "ev_backend" "ev_backend" "ev_backend" "ev_backend")
BINDIRS=("$PROJECT_DIRECTORY/build/src" "$PROJECT_DIRECTORY/build/src" "/tmp/pgmoneta-main/build/src" "/tmp/pgmoneta-main/build/src" "$PROJECT_DIRECTORY/build-f-multishot/src" "$PROJECT_DIRECTORY/build-f-zerocopy/src" "$PROJECT_DIRECTORY/build-f-both/src" "$PROJECT_DIRECTORY/build/src")

flags_to_cflags() {
  case "$1" in
    multishot) echo "-DEXPERIMENTAL_FEATURE_RECV_MULTISHOT_ENABLED=1" ;;
    zerocopy) echo "-DEXPERIMENTAL_FEATURE_ZERO_COPY_ENABLED=1" ;;
    both) echo "-DEXPERIMENTAL_FEATURE_RECV_MULTISHOT_ENABLED=1 -DEXPERIMENTAL_FEATURE_ZERO_COPY_ENABLED=1" ;;
    *) echo "" ;;
  esac
}

prebuild_binaries() {
  echo "=== prebuild: 5 binary sets (base + 3 flags + main) ==="
  # Idempotent: the workflow may have prebuilt already (same flags below);
  # skip the rebuild when all 5 sets are present to avoid double CI cost.
  # Standalone runs (binaries missing) still build fresh here.
  if [[ -x "$PROJECT_DIRECTORY/build/src/pgmoneta" ]] && \
     [[ -x "$PROJECT_DIRECTORY/build-f-multishot/src/pgmoneta" ]] && \
     [[ -x "$PROJECT_DIRECTORY/build-f-zerocopy/src/pgmoneta" ]] && \
     [[ -x "$PROJECT_DIRECTORY/build-f-both/src/pgmoneta" ]] && \
     [[ -x "/tmp/pgmoneta-main/build/src/pgmoneta" ]]; then
    echo "all 5 binary sets already present, skipping rebuild"
    if [[ ! -s /tmp/baseline-sha.txt ]] && [[ -d /tmp/pgmoneta-main ]]; then
      (cd /tmp/pgmoneta-main && git rev-parse HEAD > /tmp/baseline-sha.txt) || true
    fi
    cat /tmp/baseline-sha.txt || true
    echo "=== prebuild done (skipped) ==="
    return 0
  fi
  export CC="${CC:-/usr/bin/clang}"
  # 1) io_layer base -> build/
  mkdir -p "$PROJECT_DIRECTORY/build" || true
  (cd "$PROJECT_DIRECTORY/build" && cmake -DCMAKE_BUILD_TYPE=Debug -DDOCS=FALSE .. && make -j"$(nproc)") || { echo "ERROR: base build failed" >&2; exit 1; }
  # 2-4) flag builds -> build-f-<name>/
  for f in multishot zerocopy both; do
    cf="$(flags_to_cflags "$f")"
    echo "--- building flags=$f CFLAGS=$cf ---"
    mkdir -p "$PROJECT_DIRECTORY/build-f-$f" || true
    (cd "$PROJECT_DIRECTORY/build-f-$f" && cmake -DCMAKE_BUILD_TYPE=Debug -DDOCS=FALSE -DCMAKE_C_FLAGS="$cf" .. && make -j"$(nproc)") || { echo "ERROR: flags build $f failed" >&2; exit 1; }
  done
  # 5) main baseline: fresh clone like the baseline CI job, record SHA.
  rm -rf /tmp/pgmoneta-main || true
  if [[ -n "${GITHUB_TOKEN:-}" && -n "${GITHUB_REPOSITORY:-}" ]]; then
    git clone --branch main --single-branch --depth 1 "https://x-access-token:${GITHUB_TOKEN}@github.com/${GITHUB_REPOSITORY}.git" /tmp/pgmoneta-main || { echo "ERROR: main clone failed" >&2; exit 1; }
  else
    git clone --branch main --single-branch --depth 1 "https://github.com/pgmoneta/pgmoneta.git" /tmp/pgmoneta-main || { echo "ERROR: main clone failed" >&2; exit 1; }
  fi
  (cd /tmp/pgmoneta-main && git rev-parse HEAD > /tmp/baseline-sha.txt) || echo "unknown" > /tmp/baseline-sha.txt
  cat /tmp/baseline-sha.txt || true
  mkdir -p /tmp/pgmoneta-main/build || true
  (cd /tmp/pgmoneta-main/build && cmake -DCMAKE_BUILD_TYPE=Debug -DDOCS=FALSE .. && make -j"$(nproc)") || { echo "ERROR: main build failed" >&2; exit 1; }
  echo "=== prebuild done ==="
}

if [[ "$DRY" == "1" ]]; then
  echo "LOOP_DRY_RUN=1: skipping binary prebuilds and drop_caches"
else
  prebuild_binaries
fi

# Ensure PG bin dir on PATH for check.sh psql probes (best effort).
export PATH="/usr/pgsql-$PG/bin:$PATH" || true

FAILURES=""
NCELLS=${#LABELS[@]}
declare -a CELL_RC
for i in $(seq 0 $((NCELLS - 1))); do CELL_RC[$i]=0; done

n=0
for idx in $(seq 0 $((NCELLS - 1))); do
  n=$((n + 1))
  nn=$(printf "%02d" "$n")
  label="${LABELS[$idx]}"
  log="/tmp/check-seq-pg${PG}-${nn}-${label}.log"
  echo "=== cell $nn/08: $label (build=${BUILDS[$idx]} backend=${BACKENDS[$idx]} flags=${FLAGS[$idx]}) ==="
  if [[ "$DRY" != "1" ]]; then
    sync || true
    echo 3 > /proc/sys/vm/drop_caches || true
  fi
  # Pre-cell teardown: `timeout ... check.sh ci` on timeout kills only the
  # direct child; check.sh's own shutdown/pg-stop never runs, so a live
  # postmaster would race the next cell's fresh setup (port 5432) and stale
  # pgmoneta processes linger. Best-effort, never fails the loop.
  if [ "$(id -u)" -eq 0 ]; then
    runuser -m -u postgres -- /usr/pgsql-$PG/bin/pg_ctl -D /pgdata stop -m fast || true
  else
    sudo -E -u postgres /usr/pgsql-$PG/bin/pg_ctl -D /pgdata stop -m fast || true
  fi
  # Exact-name match (-x) ONLY. Never -f with these words: the workspace path
  # (/__w/pgmoneta/pgmoneta) appears in our own ancestors' cmdlines (su,
  # inner bash), so `pkill -f pgmoneta` SIGKILLs the very pipeline running
  # this script (proven: EPERM on root su + "Killed" + step exit 137 in CI).
  # -x matches comm, so su/bash/tee/scripts are immune, while the daemon
  # (comm "pgmoneta") and postmaster (comm "postgres") still match.
  pkill -9 -x pgmoneta || true
  # Postmaster runs as the postgres user, which the runner cannot signal, so
  # go through runuser/sudo like the pg_ctl stop above. Wrapper comms
  # ("runuser"/"sudo") never match -x postgres.
  if [ "$(id -u)" -eq 0 ]; then
    runuser -m -u postgres -- pkill -9 -x postgres || true
  else
    sudo -E -u postgres pkill -9 -x postgres || true
  fi
  # Memory observability: one line per cell, so any future resource failure
  # can be triaged from the log instead of theorized about afterwards.
  free -m | head -2 || true
  # Fresh root + unique WAL slot per cell; guaranteed-fresh env per cell.
  export PGMONETA_TEST_ROOT="/tmp/pgmoneta-test-seq-pg${PG}-${nn}"
  export PERF_WAL_SLOT="pgmoneta_seq_${n}"
  export FORCE_SETUP=1
  export TEST_PG_VERSION="$PG"
  export TEST_EVENT_BACKEND="${BACKENDS[$idx]}"
  export PERF_BUILD_LABEL="${BUILDS[$idx]}"
  export PERF_EV_KEY="${EVKEYS[$idx]}"
  export PERF_FLAGS="${FLAGS[$idx]}"
  export PERF_BIN_DIR="${BINDIRS[$idx]}"
  echo "root=$PGMONETA_TEST_ROOT slot=$PERF_WAL_SLOT bindir=$PERF_BIN_DIR -> $log"
  rc=0; tee_rc=0
  set +e
  timeout -k 2m 40m ./test/check.sh ci 2>&1 | tee "$log"
  rc=${PIPESTATUS[0]:-1} tee_rc=${PIPESTATUS[1]:-0}
  set -e
  # tee failure without check.sh failure still counts log as suspect.
  if [[ "$rc" -ne 0 || "$tee_rc" -ne 0 ]]; then
    echo "cell $label FAILED rc=$rc tee_rc=$tee_rc (124=timeout)"
    FAILURES="$FAILURES $label"
  else
    echo "cell $label PASS"
  fi
  CELL_RC[$idx]=$rc || true
  if [[ "$idx" -lt $((NCELLS - 1)) ]]; then
    if [[ "$DRY" == "1" ]]; then sleep 1 || true; else sleep 5 || true; fi
  fi
done

echo ""
echo "=== cell summary (pg $PG) ==="
fail_count=0
for idx in $(seq 0 $((NCELLS - 1))); do
  nn=$(printf "%02d" $((idx + 1)))
  if [[ "${CELL_RC[$idx]:-1}" -eq 0 ]]; then st="PASS"; else st="FAIL"; fail_count=$((fail_count + 1)) || true; fi
  echo "cell $nn ${LABELS[$idx]} -> $st (rc=${CELL_RC[$idx]:-?})"
done
if [[ -n "$FAILURES" ]]; then
  echo "FAILURES:$FAILURES"
  exit 1
fi
echo "ALL 8 CELLS PASSED"
exit 0
