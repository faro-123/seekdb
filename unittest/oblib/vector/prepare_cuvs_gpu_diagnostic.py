"""Restore a pinned bridge experiment and run synthetic diagnostics after an instance reset."""

import argparse
import getpass
import json
import os
import re
import subprocess
import sys
import urllib.request
from datetime import datetime, timezone
from pathlib import Path

BASELINE = "634888d9b037153fa143d2c913ba4b25503e1d62"
BASELINES = {
    "v5": ("oceanbase/seekdb", BASELINE),
    "known-good": ("wswsadad/seekdb", "556065c62578673d6313f2fdd798cc8753cbb662"),
}


def single_gpu_bus(output):
    devices = re.findall(r"GPU\[(\d+)\]\s*:\s*PCI Bus:\s*([0-9a-fA-F]{4}:[0-9a-fA-F]{2}:[0-9a-fA-F]{2}\.[0-7])", output)
    if len(devices) != 1 or devices[0][0] != "0":
        raise RuntimeError("Expected one visible GPU[0]; cannot safely choose a VRAM counter")
    return devices[0][1].lower()


def main(token=None, argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--revision", required=True, help="full candidate commit SHA")
    parser.add_argument("--root", type=Path, default=Path("/workspace/seekdb-gpu-diagnostic"))
    parser.add_argument("--dim", type=int, choices=(128, 256), default=128)
    parser.add_argument("--rounds", type=int, default=3)
    parser.add_argument("--baseline", choices=BASELINES, default="v5",
                        help="official V5 or the user's previously successful submission")
    parser.add_argument("--trace", action="store_true", help="enable synchronized construction phase timings")
    args = parser.parse_args(argv)
    if not re.fullmatch(r"[0-9a-f]{40}", args.revision) or args.rounds < 2:
        parser.error("a full lowercase commit SHA and at least two rounds are required")

    hardware = subprocess.run(["rocm-smi", "--showbus", "--showmeminfo", "vram"],
                              check=True, text=True, capture_output=True).stdout
    print(hardware, flush=True)
    bus = single_gpu_bus(hardware)
    counter = Path("/sys/bus/pci/devices") / bus / "mem_info_vram_used"
    int(counter.read_text().strip())
    print("Selected GPU[0] VRAM counter:", counter, flush=True)
    if token is None:
        token = getpass.getpass("GitHub token (paste once): ")

    def download(repo, revision, name, target):
        url = f"https://api.github.com/repos/{repo}/contents/{name}?ref={revision}"
        request = urllib.request.Request(url, headers={
            "Authorization": "Bearer " + token,
            "Accept": "application/vnd.github.raw+json",
            "User-Agent": "seekdb-gpu-debug",
        })
        with urllib.request.urlopen(request, timeout=60) as response:
            data = response.read()
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
        print("Downloaded:", target, len(data), "bytes", flush=True)

    root = args.root.resolve()
    candidate_root = root / ("candidate-" + args.revision[:12])
    source = candidate_root / "src"
    for name in ("build.sh", "seekdb_cuvs_bridge.c", "seekdb_cuvs_bridge.h",
                 "seekdb_cuvs_graph.hip", "seekdb_cuvs_graph_algorithms.h", "test_persistence.cpp"):
        download("wswsadad/seekdb", args.revision, "tools/hipvs_bridge/" + name, source / name)
    benchmark = candidate_root / "benchmark_cuvs_bridge.py"
    download("wswsadad/seekdb", args.revision,
             "unittest/oblib/vector/benchmark_cuvs_bridge.py", benchmark)
    baseline_repo, baseline_revision = BASELINES[args.baseline]
    baseline_root = root / ("baseline-" + baseline_revision[:12])
    for name in ("build.sh", "seekdb_cuvs_bridge.c", "seekdb_cuvs_bridge.h"):
        download(baseline_repo, baseline_revision, "tools/hipvs_bridge/" + name, baseline_root / "src" / name)

    env = os.environ.copy()
    env["LD_LIBRARY_PATH"] = "/opt/hipvs/lib:/opt/rocm/lib"
    env["OMP_NUM_THREADS"] = env["OPENBLAS_NUM_THREADS"] = "16"
    env.pop("SEEKDB_CUVS_BUILD_TRACE", None)
    if args.trace:
        env["SEEKDB_CUVS_BUILD_TRACE"] = "1"
    candidate = candidate_root / "libseekdb_cuvs_bridge.so"
    baseline = baseline_root / "libseekdb_cuvs_bridge.so"
    for script, library in ((source / "build.sh", candidate), (baseline_root / "src/build.sh", baseline)):
        subprocess.run(["bash", str(script), str(library)], env=env, check=True)

    smoke = candidate_root / "test_persistence"
    smoke_env = dict(env, LD_LIBRARY_PATH=f"{candidate_root}:" + env["LD_LIBRARY_PATH"])
    subprocess.run(["g++", "-O2", "-std=c++17", str(source / "test_persistence.cpp"),
                    "-L" + str(candidate_root), "-lseekdb_cuvs_bridge", "-o", str(smoke)],
                   env=smoke_env, check=True)
    for action in ("write", "read"):
        subprocess.run([str(smoke), action, str(candidate_root / "test.cagra")], env=smoke_env, check=True)

    data = root / "synthetic-seed20261029"
    data.mkdir(parents=True, exist_ok=True)
    base, queries = data / f"base-{args.dim}.f32", data / f"query-{args.dim}.f32"
    if not all(path.is_file() and path.stat().st_size == count * args.dim * 4
               for path, count in ((base, 250000), (queries, 256))):
        import numpy as np
        rng = np.random.default_rng(20261029)
        rng.standard_normal((250000, args.dim), dtype=np.float32).tofile(base)
        rng.standard_normal((256, args.dim), dtype=np.float32).tofile(queries)
        print("Generated synthetic data; seed=20261029, dim=", args.dim, flush=True)

    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
    report = candidate_root / f"comparison-{args.dim}-{'trace' if args.trace else 'plain'}-{stamp}.json"
    report.with_suffix(".setup.json").write_text(json.dumps({
        "candidate_revision": args.revision, "baseline_revision": baseline_revision,
        "gpu_bus": bus, "hardware": hardware, "vram_used_file": str(counter),
        "synthetic_seed": 20261029, "dim": args.dim, "trace": args.trace,
        "scope": "synthetic bridge diagnostic, not official evaluation",
    }, indent=2), encoding="utf-8")
    subprocess.run([sys.executable, str(benchmark), "--baseline", str(baseline),
                    "--candidate", str(candidate), "--base", str(base), "--queries", str(queries),
                    "--dim", str(args.dim), "--rounds", str(args.rounds),
                    "--vram-used-file", str(counter), "--output", str(report)], env=env, check=True)
    print("Download this report before recreating the instance:", report, flush=True)


if __name__ == "__main__":
    main()
