# V5 CAGRA candidate validation report

Date: 2026-10-29. Baseline: `634888d9b037153fa143d2c913ba4b25503e1d62`.
Candidate branch: `contest/hipvs-improve-20261029`.

This replaces the earlier V2 report. V5 is merged; the candidate reuses
single-query GPU scratch buffers without changing V5 graph/search parameters.
See [V5_READINESS.md](V5_READINESS.md) for exact changes, verified test results,
and outstanding hardware validation. The CPU shim test instructions are in
`unittest/oblib/vector/hipvs_bridge_mock/README.md`.

## Development build comparison

Build baseline and candidate observers and bridges from their respective source
revisions. Enable tracing in both observers for this diagnostic. Use:

```bash
bash docs/gpu-vector-index-hipvs-cuvs/validation/run_build_compare.sh \
  --baseline-observer /work/baseline/build_release/src/observer/seekdb \
  --candidate-observer /work/candidate/build_release/src/observer/seekdb \
  --baseline-bridge /work/baseline-bridge/libseekdb_cuvs_bridge.so \
  --candidate-bridge /work/candidate-bridge/libseekdb_cuvs_bridge.so \
  --base-dir /tmp/cagra-build-ab \
  --baseline-port 2981 --candidate-port 2991 \
  --render /dev/dri/renderD135 \
  --evidence /tmp/cagra-build-evidence \
  --rows 250000 --dim 128 --rounds 3
```

Choose the render node assigned to the GPU. The script starts and stops its own
observers and replaces its baseline/candidate data directories. Use dedicated
scratch directories. Repeat with dimension 256 and a new scratch directory.
It needs mysql, Python with numpy, and the frozen ROCm/hipVS runtime.

This measures synthetic data and one probe per build. It reports build time
through first query and diagnostic recall, not formal contest recall or query
QPS. Profiler verification and official 4096-query measurements remain required.
No score should be calculated from the CPU allocation test or this diagnostic.

| Measurement | Status |
| --- | --- |
| V5 ancestry | Passed |
| CPU scratch-buffer fault-injection test | Passed |
| Bash syntax | Passed |
| Real hipVS bridge / seekdb build | Not run |
| AMD GPU build speedup / QPS | Not measured |
| Official Recall / ID / VRAM / GPU gates | Not run |
| Official score | Unknown |
