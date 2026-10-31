# V5 contest readiness — 2026-10-29

The candidate branch `contest/hipvs-improve-20261029` now contains official V5
`634888d9b037153fa143d2c913ba4b25503e1d62` through merge commit `1789c0e9a`.
`git merge-base --is-ancestor 634888d9b HEAD` returned exit code 0.
The original `contest/hipvs-cagra-optimization` remote branch was not pushed.

## Implemented optimization

Single-row searches retain their three GPU scratch buffers per index handle.
Repeated searches at the same or smaller k reuse the buffers; successful growth
replaces them. Failed growth frees partial allocations and preserves the old
cache. Batched calls retain their original temporary-buffer lifetime. Index
free releases cached buffers before destroying cuVS resources. For dimension
256 and k=100 the retained payload is 1,824 bytes per handle.

Build parameters remain V5 NN_DESCENT, graph degree 128, intermediate degree
256, and 100 iterations. Search parameters remain at least max(512, 4*k)
intermediate candidates and width 4. No quality parameters were reduced.
Distance-copy failures now return an error. Invalid dimensions, counts, pointers,
and overflowing search allocation sizes are rejected.

The adaptor and GPU smoke runner use the complete V5 implementation. V5 already
includes lazy CPU fallback; the older sidecar implementation was not combined
with its separate state machine.

## Verification actually performed

- MinGW GCC C11: `-Wall -Wextra -Werror`, bridge compiled against a test-only shim.
- CPU fault-injection test: PASS for 4096 repeated single-row calls with only
  three initial scratch allocations, cache growth failure recovery, changing
  query values and k, batch isolation, distance-copy error propagation,
  deserialize-then-search, and final release with zero live scratch allocations.
- A/B script Bash syntax check: PASS.
- V5 ancestry: PASS.

These results do not validate the real hipVS ABI, GPU execution, recall,
performance, VRAM peak, or full seekdb compilation.

## Reproduction

Build the baseline and candidate separately with the supplied bridge/build.sh
and seekdb build commands. Use separate bridge directories with the same library
filename `libseekdb_cuvs_bridge.so`. The A/B script accepts separate
`--baseline-bridge` and `--candidate-bridge` arguments so each observer loads its
own library. It recognizes V5 snapshot-build logs and checks unique, legal IDs.
See BUILD_REPORT.md for the command. Its synthetic single-probe build comparison
is a development diagnostic, not the official 4096-query evaluation.

Run the V5 GPU smoke and DDL persistence smoke with actual AMD hardware. Then
run organizer evaluation: 250K FP32 L2 vectors at dimensions 128 and 256,
paired V5/candidate runs with fresh indexes, build timing through first query,
4096 individual Top-10 and Top-100 SQL queries, baseline-relative recall,
query regression, profiler, resource and lib=vsag gates.

SIFT recall must be at least 0.95 at both k values; Hidden minimums are 0.90
and 0.85. Each recall may decrease at most 0.005 from paired V5. Per-dataset
geometric mean QPS ratio must be at least 0.90. Peak VRAM must not exceed
45 GB or 1.25 times baseline. A 3x weighted build speedup targets all 14 build
points; the official query score equation is still missing.

No AMD GPU server is currently available. The supplied repository examples
are not confirmed organizer evaluation commands. No full-score claim is made.
