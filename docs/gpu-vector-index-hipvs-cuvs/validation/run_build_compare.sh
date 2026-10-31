#!/usr/bin/env bash

set -Eeuo pipefail

usage()
{
  cat <<'EOF'
Usage: run_build_compare.sh --baseline-observer PATH --candidate-observer PATH
                            --baseline-bridge PATH --candidate-bridge PATH --base-dir PATH
                            --baseline-port PORT --candidate-port PORT
                            --render PATH --evidence PATH
                            [--rows N] [--dim N] [--rounds N]

Both observers must be built with OB_BUILD_CUVS=ON and OB_BUILD_CUVS_TRACE=ON.
The two observers are started separately. Each timed round drops the vector index,
then measures CREATE VECTOR INDEX plus the first fixed APPROXIMATE query.
EOF
}

BASELINE_OBSERVER=""
CANDIDATE_OBSERVER=""
BRIDGE=""
BASELINE_BRIDGE=""
CANDIDATE_BRIDGE=""
BASE_DIR=""
BASELINE_PORT=""
CANDIDATE_PORT=""
RENDER=""
EVIDENCE=""
ROWS=10000
DIM=128
ROUNDS=3

while (($# > 0)); do
  case "$1" in
    --baseline-observer) BASELINE_OBSERVER=$2; shift 2 ;;
    --candidate-observer) CANDIDATE_OBSERVER=$2; shift 2 ;;
    --bridge) BRIDGE=$2; shift 2 ;;
    --baseline-bridge) BASELINE_BRIDGE=$2; shift 2 ;;
    --candidate-bridge) CANDIDATE_BRIDGE=$2; shift 2 ;;
    --base-dir) BASE_DIR=$2; shift 2 ;;
    --baseline-port) BASELINE_PORT=$2; shift 2 ;;
    --candidate-port) CANDIDATE_PORT=$2; shift 2 ;;
    --render) RENDER=$2; shift 2 ;;
    --evidence) EVIDENCE=$2; shift 2 ;;
    --rows) ROWS=$2; shift 2 ;;
    --dim) DIM=$2; shift 2 ;;
    --rounds) ROUNDS=$2; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "unknown argument: $1" >&2; usage >&2; exit 2 ;;
  esac
done

[[ -x "$BASELINE_OBSERVER" ]] || { echo "baseline observer is not executable" >&2; exit 2; }
[[ -x "$CANDIDATE_OBSERVER" ]] || { echo "candidate observer is not executable" >&2; exit 2; }
BASELINE_BRIDGE=${BASELINE_BRIDGE:-$BRIDGE}
CANDIDATE_BRIDGE=${CANDIDATE_BRIDGE:-$BRIDGE}
[[ -f "$BASELINE_BRIDGE" && -f "$CANDIDATE_BRIDGE" ]] ||
  { echo "baseline and candidate bridges must exist" >&2; exit 2; }
[[ -c /dev/kfd ]] || { echo "/dev/kfd is unavailable" >&2; exit 2; }
[[ -c "$RENDER" ]] || { echo "render node is unavailable: $RENDER" >&2; exit 2; }
[[ -n "$BASE_DIR" && "$BASE_DIR" != "/" ]] || { echo "unsafe base-dir" >&2; exit 2; }
[[ -n "$EVIDENCE" && "$EVIDENCE" != "/" ]] || { echo "unsafe evidence path" >&2; exit 2; }
[[ "$ROWS" =~ ^[0-9]+$ && "$DIM" =~ ^[0-9]+$ && "$ROUNDS" -ge 3 ]] ||
  { echo "invalid rows/dim/rounds; rounds must be >= 3" >&2; exit 2; }
((ROWS >= 256 && DIM > 0)) || { echo "rows must be >= 256 and dim must be > 0" >&2; exit 2; }
[[ "$BASELINE_PORT" != "$CANDIDATE_PORT" ]] ||
  { echo "baseline and candidate ports must differ" >&2; exit 2; }
command -v mysql >/dev/null
command -v paste >/dev/null
command -v python3 >/dev/null
command -v timeout >/dev/null

BASELINE_DIR=$BASE_DIR/baseline
CANDIDATE_DIR=$BASE_DIR/candidate
DATA_DIR=$EVIDENCE/data
BASELINE_TRACE=$EVIDENCE/baseline-obvsag-trace.log
CANDIDATE_TRACE=$EVIDENCE/candidate-obvsag-trace.log
BASELINE_LOG=$EVIDENCE/baseline-observer.log
CANDIDATE_LOG=$EVIDENCE/candidate-observer.log
RESULTS=$EVIDENCE/results.tsv
SUMMARY=$EVIDENCE/summary.json
WAIT_FIFO=$BASE_DIR/.wait
ACTIVE_PID=""
ACTIVE_DIR=""

rm -rf "$BASELINE_DIR" "$CANDIDATE_DIR" "$DATA_DIR"
rm -f "$BASELINE_TRACE" "$CANDIDATE_TRACE" "$RESULTS" "$SUMMARY" \
  "$BASELINE_LOG" "$CANDIDATE_LOG" "$WAIT_FIFO"
mkdir -p "$BASE_DIR" "$EVIDENCE" "$DATA_DIR"
: > "$RESULTS"
mkfifo "$WAIT_FIFO"
exec 9<>"$WAIT_FIFO"

wait_tick()
{
  read -r -t 1 -u 9 _ || true
}

stop_observer()
{
  local pid=$1
  local expected_dir=$2
  [[ -n "$pid" && -d "/proc/$pid" ]] || return 0
  local state cmd
  state=$(awk '/^State:/{print $2}' "/proc/$pid/status" 2>/dev/null || true)
  [[ "$state" == "Z" ]] && return 0
  cmd=$(tr '\0' ' ' < "/proc/$pid/cmdline" 2>/dev/null || true)
  if [[ "$cmd" != *"--base-dir $expected_dir"* ]]; then
    echo "refusing to stop pid=$pid: $cmd" >&2
    return 1
  fi
  kill -TERM "$pid" 2>/dev/null || true
  for _ in 1 2 3 4 5; do
    [[ ! -d "/proc/$pid" ]] && break
    state=$(awk '/^State:/{print $2}' "/proc/$pid/status" 2>/dev/null || true)
    [[ "$state" == "Z" ]] && break
    wait_tick
  done
  if [[ -d "/proc/$pid" ]]; then
    kill -KILL "$pid" 2>/dev/null || true
  fi
  wait "$pid" 2>/dev/null || true
}

on_exit()
{
  local rc=$?
  trap - EXIT INT TERM
  stop_observer "$ACTIVE_PID" "$ACTIVE_DIR" || true
  exec 9>&- || true
  rm -f "$WAIT_FIFO"
  exit "$rc"
}
trap on_exit EXIT INT TERM

mysql_cmd()
{
  timeout 300 mysql --connect-timeout=5 --protocol=TCP -h127.0.0.1 -P"$1" -uroot -A -N -B "${@:2}"
}

wait_ready()
{
  local port=$1
  local pid=$2
  for _ in $(seq 1 120); do
    [[ -d "/proc/$pid" ]] || return 1
    if mysql_cmd "$port" -e 'select 1' >/dev/null 2>&1; then
      return 0
    fi
    wait_tick
  done
  return 1
}

start_observer()
{
  local observer=$1
  local run_dir=$2
  local port=$3
  local trace=$4
  local log=$5
  local pid_var=$6
  local bridge=$7
  mkdir -p "$run_dir"
  ulimit -n 65535
  env -u OB_VSAG_USE_CUVS \
    "LD_LIBRARY_PATH=$(dirname "$bridge"):/opt/hipvs/lib:/opt/rocm/lib" \
    "OB_VSAG_TRACE=1" \
    "OB_VSAG_TRACE_FILE=$trace" \
    "$observer" \
      --nodaemon \
      --port "$port" \
      --data-dir "$run_dir/store" \
      --base-dir "$run_dir" \
      --parameter __min_full_resource_pool_memory=2147483648 \
      --parameter memory_budget=2G \
      --parameter datafile_size=20G \
      --parameter cpu_count=24 \
      --parameter local_ip=127.0.0.1 \
      >>"$log" 2>&1 &
  local pid=$!
  printf -v "$pid_var" '%s' "$pid"
  wait_ready "$port" "$pid"
}

trace_offset()
{
  local trace=$1
  local lines=0
  [[ -f "$trace" ]] && lines=$(wc -l < "$trace")
  echo $((lines + 1))
}

trace_count()
{
  local trace=$1
  local offset=$2
  local marker=$3
  tail -n +"$offset" "$trace" 2>/dev/null |
    awk -v marker="$marker" '$1 == marker {count++} END {print count+0}'
}

python3 - "$DATA_DIR" "$ROWS" "$DIM" <<'PY'
import pathlib
import sys

try:
    import numpy as np
except ImportError as exc:
    raise SystemExit("numpy is required by run_build_compare.sh") from exc

out = pathlib.Path(sys.argv[1])
rows = int(sys.argv[2])
dim = int(sys.argv[3])
rng = np.random.default_rng(20261027)
base = rng.standard_normal((rows, dim), dtype=np.float32)
query = rng.standard_normal(dim, dtype=np.float32)
distances = np.sum((base - query) ** 2, axis=1)
truth = np.argsort(distances)[:100].astype(np.int32)

def vector(values):
    return "[" + ",".join(f"{value:.9g}" for value in values) + "]"

with (out / "load.sql").open("w") as sql:
    sql.write("use cuvs_build_bench;\n")
    for start in range(0, rows, 250):
        values = [
            f"({row},'{vector(base[row])}')" for row in range(start, min(start + 250, rows))
        ]
        sql.write("insert into bench values " + ",".join(values) + ";\n")

(out / "query.txt").write_text(vector(query) + "\n")
truth.tofile(out / "truth.i32")
PY

QUERY=$(cat "$DATA_DIR/query.txt")
BASE_TABLE_Q="use cuvs_build_bench; create table bench(id int primary key, v vector($DIM));"
INDEX_Q="use cuvs_build_bench; create vector index idx_bench on bench(v) with (distance=l2,type=hnsw,lib=cuvs,m=16,ef_construction=200,ef_search=200);"

for port in "$BASELINE_PORT" "$CANDIDATE_PORT"; do
  if mysql_cmd "$port" -e 'select 1' >/dev/null 2>&1; then
    echo "port $port is already serving MySQL" >&2
    exit 2
  fi
done

initialize_version()
{
  local version=$1
  local observer run_dir port bridge
  if [[ "$version" == "baseline" ]]; then
    observer=$BASELINE_OBSERVER
    run_dir=$BASELINE_DIR
    port=$BASELINE_PORT
    bridge=$BASELINE_BRIDGE
  else
    observer=$CANDIDATE_OBSERVER
    run_dir=$CANDIDATE_DIR
    port=$CANDIDATE_PORT
    bridge=$CANDIDATE_BRIDGE
  fi
  start_observer "$observer" "$run_dir" "$port" \
    "$EVIDENCE/$version-obvsag-trace.log" "$EVIDENCE/$version-observer.log" ACTIVE_PID "$bridge" ||
    { echo "$version observer failed to initialize" >&2; return 1; }
  ACTIVE_DIR=$run_dir
  mysql_cmd "$port" -e 'alter system set ob_vector_memory_limit_percentage=30; create database if not exists cuvs_build_bench;' >/dev/null
  mysql_cmd "$port" -e "$BASE_TABLE_Q" >/dev/null
  mysql_cmd "$port" < "$DATA_DIR/load.sql" >/dev/null
  stop_observer "$ACTIVE_PID" "$ACTIVE_DIR"
  ACTIVE_PID=""
  ACTIVE_DIR=""
}

initialize_version baseline
initialize_version candidate

run_one()
{
  local version=$1
  local round=$2
  local observer run_dir port trace log bridge
  if [[ "$version" == "baseline" ]]; then
    observer=$BASELINE_OBSERVER
    run_dir=$BASELINE_DIR
    port=$BASELINE_PORT
    trace=$BASELINE_TRACE
    log=$BASELINE_LOG
    bridge=$BASELINE_BRIDGE
  else
    observer=$CANDIDATE_OBSERVER
    run_dir=$CANDIDATE_DIR
    port=$CANDIDATE_PORT
    trace=$CANDIDATE_TRACE
    log=$CANDIDATE_LOG
    bridge=$CANDIDATE_BRIDGE
  fi
  start_observer "$observer" "$run_dir" "$port" "$trace" "$log" ACTIVE_PID "$bridge" ||
    { echo "$version observer failed to start for round $round" >&2; return 1; }
  ACTIVE_DIR=$run_dir
  local offset
  offset=$(trace_offset "$trace")
  local lifecycle_log="$run_dir/log/seekdb.log"
  local lifecycle_offset
  lifecycle_offset=$(trace_offset "$lifecycle_log")
  local start_ns end_ns ids
  start_ns=$(date +%s%N)
  ids=$(mysql_cmd "$port" -e "$INDEX_Q
    select /*+ index(bench idx_bench) */ id from bench
    order by l2_distance(v,'$QUERY') approximate limit 100;" | paste -sd, -)
  end_ns=$(date +%s%N)
  local elapsed_ns=$((end_ns - start_ns))
  local builds serves deferred materialized
  builds=$(trace_count "$trace" "$offset" cuvs_build)
  serves=$(trace_count "$trace" "$offset" cuvs_serve)
  local lifecycle="$EVIDENCE/$version-round-$round-lifecycle.log"
  tail -n +"$lifecycle_offset" "$lifecycle_log" > "$lifecycle"
  local snapshot_builds
  snapshot_builds=$(grep -a ' serialized GPU CAGRA snapshot' "$lifecycle" | grep -ac 'built=true' || true)
  builds=$((builds + snapshot_builds))
  deferred=0 # V5 does not emit the old V2 deferred-sidecar trace.
  materialized=$(grep -ac 'lazily built VSAG fallback' "$lifecycle" || true)
  [[ "$builds" -ge 1 && "$serves" -ge 1 ]] ||
    { echo "$version round $round did not run CAGRA build/search" >&2; return 1; }
  if [[ "$version" == "candidate" ]]; then
    [[ "$materialized" -eq 0 ]] ||
      { echo "$version round $round unexpectedly built a CPU sidecar in the timed region" >&2; return 1; }
  fi
  [[ -n "$ids" ]] || { echo "$version round $round returned no IDs" >&2; return 1; }
  local id_count
  id_count=$(awk -F',' '{print NF}' <<<"$ids")
  [[ "$id_count" -eq 100 ]] ||
    { echo "$version round $round returned $id_count IDs, expected 100" >&2; return 1; }
  printf '%s\t%d\t%d\t%s\t%s\t%s\t%s\n' \
    "$version" "$round" "$elapsed_ns" "$ids" "$builds" "$deferred" "$materialized" >> "$RESULTS"
  mysql_cmd "$port" -e 'use cuvs_build_bench; drop index idx_bench on bench;' >/dev/null
  stop_observer "$ACTIVE_PID" "$ACTIVE_DIR"
  ACTIVE_PID=""
  ACTIVE_DIR=""
}

for round in $(seq 1 "$ROUNDS"); do
  if ((round % 2 == 1)); then
    run_one baseline "$round"
    run_one candidate "$round"
  else
    run_one candidate "$round"
    run_one baseline "$round"
  fi
done

python3 - "$RESULTS" "$DATA_DIR/truth.i32" "$SUMMARY" "$ROWS" "$DIM" <<'PY'
import json
import pathlib
import statistics
import struct
import sys

results_path, truth_path, summary_path = map(pathlib.Path, sys.argv[1:4])
rows, dim = map(int, sys.argv[4:6])
truth = struct.unpack("<100i", truth_path.read_bytes())
truth10 = set(truth[:10])
truth100 = set(truth)
runs = {"baseline": [], "candidate": []}
for line in results_path.read_text().splitlines():
    version, round_id, elapsed_ns, ids, builds, deferred, materialized = line.split("\t")
    returned = [int(value) for value in ids.split(",") if value]
    if len(returned) != 100 or len(set(returned)) != 100 or any(i < 0 or i >= rows for i in returned):
        raise SystemExit(f"{version} round {round_id}: invalid or duplicate result IDs")
    runs[version].append({
        "round": int(round_id),
        "seconds": int(elapsed_ns) / 1e9,
        "recall_at_10": len(set(returned[:10]) & truth10) / 10.0,
        "recall_at_100": len(set(returned[:100]) & truth100) / 100.0,
        "cuvs_builds": int(builds),
        "deferred_vsag": int(deferred),
        "vsag_materializations": int(materialized),
    })

if any(len(items) < 3 for items in runs.values()):
    raise SystemExit("each version requires at least three successful rounds")

medians = {name: statistics.median(item["seconds"] for item in items)
           for name, items in runs.items()}
recalls10 = {name: statistics.median(item["recall_at_10"] for item in items)
             for name, items in runs.items()}
recalls100 = {name: statistics.median(item["recall_at_100"] for item in items)
              for name, items in runs.items()}
summary = {
    "rows": rows,
    "dim": dim,
    "runs": runs,
    "median_seconds": medians,
    "median_recall_at_10": recalls10,
    "median_recall_at_100": recalls100,
    "build_speedup": medians["baseline"] / medians["candidate"],
}
summary_path.write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n")
print(json.dumps(summary, indent=2, sort_keys=True))
PY

echo "A/B build result: $SUMMARY"
