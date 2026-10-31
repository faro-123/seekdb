"""Real-GPU bridge diagnostic; excludes SQL/DDL overhead and official scoring."""

import argparse
import ctypes
import json
import statistics
import tempfile
import threading
import time
from concurrent.futures import ThreadPoolExecutor
from contextlib import nullcontext
from pathlib import Path


class VramSampler:
    """Sample this GPU's sysfs counter; peaks between samples can be missed."""

    def __init__(self, path):
        self.path = Path(path)
        self.initial = int(self.path.read_text().strip())
        self.peak = self.initial
        self.minimum = self.initial
        self.samples = 1
        self.error = None
        self.stop = threading.Event()
        self.thread = threading.Thread(target=self.poll, daemon=True)

    def poll(self):
        while not self.stop.wait(0.02):
            try:
                value = int(self.path.read_text().strip())
                self.peak = max(self.peak, value)
                self.minimum = min(self.minimum, value)
                self.samples += 1
            except (OSError, ValueError) as error:
                self.error = error
                return

    def __enter__(self):
        self.thread.start()
        return self

    def __exit__(self, kind, value, traceback):
        self.stop.set()
        self.thread.join()
        if self.error is not None and kind is None:
            raise RuntimeError(f"VRAM sampling failed: {self.error}")


def summarize_vram(runs):
    activity = {
        name: any(r["vram_peak_sampled_bytes"] > r["vram_min_sampled_bytes"] for r in values)
        for name, values in runs.items()
    }
    peaks = {name: max(r["vram_peak_sampled_bytes"] for r in values) for name, values in runs.items()}
    ratio = peaks["candidate"] / peaks["baseline"] if all(activity.values()) and peaks["baseline"] else None
    return activity, ratio


def recall_at_k(returned, truth, rows, k):
    if len(returned) != len(truth) or not truth:
        raise ValueError("query result count does not match ground truth")
    hits = 0
    for actual, expected in zip(returned, truth):
        if len(actual) != k or len(set(actual)) != k:
            raise ValueError("missing or duplicate result IDs")
        if any(i < 0 or i >= rows for i in actual):
            raise ValueError("result ID out of range")
        hits += len(set(actual).intersection(expected[:k]))
    return hits / (len(truth) * k)


def load_vectors(path, dim):
    import numpy as np

    size = Path(path).stat().st_size
    if size == 0 or size % (4 * dim):
        raise ValueError(f"{path}: not a raw FP32 matrix with {dim} columns")
    data = np.memmap(path, dtype="<f4", mode="r", shape=(size // (4 * dim), dim))
    if not np.isfinite(data).all():
        raise ValueError(f"{path}: non-finite vectors")
    return data


def exact_truth(base, queries):
    import numpy as np

    # Use FP64 arithmetic over the original FP32 values for the CPU oracle.
    data = np.asarray(base, dtype=np.float64)
    norms = np.einsum("ij,ij->i", data, data)
    truth = []
    for query in queries:
        q = np.asarray(query, dtype=np.float64)
        distances = norms - 2 * (data @ q) + (q @ q)
        cutoff = np.partition(distances, 99)[99]
        ids = np.flatnonzero(distances < cutoff)
        ties = np.flatnonzero(distances == cutoff)[:100 - len(ids)]
        ids = np.concatenate((ids, ties))
        ids = ids[np.lexsort((ids, distances[ids]))]
        truth.append(ids.tolist())
    return truth


class Bridge:
    def __init__(self, path):
        self.lib = ctypes.CDLL(str(Path(path).resolve()))
        self.lib.seekdb_cuvs_build.argtypes = [ctypes.c_void_p, ctypes.c_long, ctypes.c_long]
        self.lib.seekdb_cuvs_build.restype = ctypes.c_void_p
        self.lib.seekdb_cuvs_search.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_long,
                                               ctypes.c_long, ctypes.c_void_p, ctypes.c_void_p]
        self.lib.seekdb_cuvs_search.restype = ctypes.c_int
        self.lib.seekdb_cuvs_free.argtypes = [ctypes.c_void_p]
        self.lib.seekdb_cuvs_free.restype = None
        self.lib.seekdb_cuvs_serialize.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
        self.lib.seekdb_cuvs_serialize.restype = ctypes.c_int
        self.lib.seekdb_cuvs_deserialize.argtypes = [ctypes.c_char_p, ctypes.c_long, ctypes.c_long]
        self.lib.seekdb_cuvs_deserialize.restype = ctypes.c_void_p

    def search(self, handle, query, ids, distances, k):
        rc = self.lib.seekdb_cuvs_search(handle, query.ctypes.data, 1, k,
                                       ids.ctypes.data, distances.ctypes.data)
        if rc != 0:
            raise RuntimeError(f"GPU search failed: rc={rc}")

    def run(self, base, queries, truth, snapshot):
        import numpy as np

        rows, dim = base.shape
        handle = None
        path = str(snapshot).encode()
        try:
            start = time.perf_counter()
            handle = self.lib.seekdb_cuvs_build(base.ctypes.data, rows, dim)
            if not handle:
                raise RuntimeError("GPU build failed")
            built = time.perf_counter()
            if self.lib.seekdb_cuvs_serialize(handle, path) != 0:
                raise RuntimeError("GPU snapshot serialize failed")
            serialized = time.perf_counter()
            snapshot_bytes = snapshot.stat().st_size
            self.lib.seekdb_cuvs_free(handle)
            handle = None
            handle = self.lib.seekdb_cuvs_deserialize(path, rows, dim)
            if not handle:
                raise RuntimeError("GPU snapshot reload failed")
            reloaded = time.perf_counter()
            ids = np.empty((1, 10), dtype=np.uint32)
            distances = np.empty((1, 10), dtype=np.float32)
            self.search(handle, queries[0], ids[0], distances[0], 10)
            recall_at_k(ids.tolist(), truth[:1], rows, 10)
            ready = time.perf_counter()
            result = {
                "build_call_seconds": built - start,
                "serialize_call_seconds": serialized - built,
                "free_and_reload_call_seconds": reloaded - serialized,
                "first_query_seconds": ready - reloaded,
                "ready_seconds": ready - start,
                "snapshot_bytes": snapshot_bytes,
                "recall": {}, "bridge_qps": {},
            }
            for k in (10, 100):
                ids = np.empty((len(queries), k), dtype=np.uint32)
                distances = np.empty((len(queries), k), dtype=np.float32)
                start_query = time.perf_counter()
                for q, query in enumerate(queries):
                    self.search(handle, query, ids[q], distances[q], k)
                seconds = time.perf_counter() - start_query
                result["bridge_qps"][str(k)] = len(queries) / seconds
                result["recall"][str(k)] = recall_at_k(ids.tolist(), truth, rows, k)
            return result
        finally:
            if handle:
                self.lib.seekdb_cuvs_free(handle)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", required=True)
    parser.add_argument("--candidate", required=True)
    parser.add_argument("--base", required=True)
    parser.add_argument("--queries", required=True)
    parser.add_argument("--dim", required=True, type=int)
    parser.add_argument("--rounds", type=int, default=3)
    parser.add_argument("--output", required=True)
    parser.add_argument("--vram-used-file", help="this GPU's mem_info_vram_used sysfs path")
    args = parser.parse_args()
    if args.dim <= 0 or args.rounds < 2:
        parser.error("positive dimension and at least two alternating rounds required")
    base = load_vectors(args.base, args.dim)
    queries = load_vectors(args.queries, args.dim)
    if len(base) < 256:
        parser.error("at least 256 base vectors required")
    print(f"rows={len(base)} dim={args.dim} queries={len(queries)}", flush=True)
    print("Computing CPU exact Top-100 outside GPU timers...", flush=True)
    truth = exact_truth(base, queries)
    output = Path(args.output).resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    report = {
        "scope": "bridge only; not official SQL/DDL evaluation",
        "rows": len(base), "dim": args.dim, "queries": len(queries),
        "libraries": {"baseline": str(Path(args.baseline).resolve()),
                      "candidate": str(Path(args.candidate).resolve())},
        "vram_used_file": args.vram_used_file,
        "runs": {"baseline": [], "candidate": []},
    }
    # All cuVS operations run on one reusable large-stack thread, including free.
    threading.stack_size(32 * 1024 * 1024)
    with ThreadPoolExecutor(max_workers=1) as worker, tempfile.TemporaryDirectory(
            prefix="cagra-bench-", dir=output.parent) as temporary:
        for round_id in range(args.rounds):
            order = ("baseline", "candidate") if round_id % 2 == 0 else ("candidate", "baseline")
            for name in order:
                print(f"Round {round_id + 1}: {name}", flush=True)
                snapshot = Path(temporary) / f"{name}-{round_id}.cagra"

                def run(library=report["libraries"][name], path=snapshot):
                    context = VramSampler(args.vram_used_file) if args.vram_used_file else nullcontext()
                    with context as sampler:
                        measured = Bridge(library).run(base, queries, truth, path)
                    if sampler is not None:
                        measured["vram_initial_bytes"] = sampler.initial
                        measured["vram_peak_sampled_bytes"] = sampler.peak
                        measured["vram_min_sampled_bytes"] = sampler.minimum
                        measured["vram_samples"] = sampler.samples
                    return measured

                result = worker.submit(run).result()
                result["round"] = round_id + 1
                report["runs"][name].append(result)
                print(json.dumps(result, sort_keys=True), flush=True)
                output.write_text(json.dumps(report, indent=2), encoding="utf-8")
                snapshot.unlink()
    medians = {name: statistics.median(r["ready_seconds"] for r in runs)
               for name, runs in report["runs"].items()}
    report["bridge_ready_speedup"] = medians["baseline"] / medians["candidate"]
    report["median_recall_delta"] = {
        str(k): statistics.median(r["recall"][str(k)] for r in report["runs"]["candidate"]) -
                statistics.median(r["recall"][str(k)] for r in report["runs"]["baseline"])
        for k in (10, 100)
    }
    if args.vram_used_file:
        activity, ratio = summarize_vram(report["runs"])
        report["vram_counter_activity_detected"] = activity
        report["sampled_vram_peak_ratio"] = ratio
        if ratio is None:
            print("VRAM counter did not vary for both libraries; sampled peak ratio is inconclusive.", flush=True)
    output.write_text(json.dumps(report, indent=2), encoding="utf-8")
    summary = {k: report[k] for k in ("bridge_ready_speedup", "median_recall_delta")}
    if args.vram_used_file:
        summary["sampled_vram_peak_ratio"] = report["sampled_vram_peak_ratio"]
    print(json.dumps(summary), flush=True)
    print(f"Report: {output}", flush=True)


if __name__ == "__main__":
    main()
