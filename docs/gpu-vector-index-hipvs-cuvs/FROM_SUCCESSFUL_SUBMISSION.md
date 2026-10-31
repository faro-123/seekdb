# Isolate bridge optimization on the officially successful submission

Starting commit: `556065c62578673d6313f2fdd798cc8753cbb662`.
Branch: `contest/hipvs-from-556065c6`.

The user explicitly reported this starting commit completed official evaluation
with about four points. This is user-provided execution evidence, not a
guarantee that an altered commit will pass. The later generic execution failure
did not expose the actual commit, failing phase, or error log.

This candidate deliberately retains the starting commit's entire `src/`,
`deps/`, `cmake/`, root `CMakeLists.txt`, and root `build.sh`. In particular,
it uses the original per-call large-stack pthread implementation, not the
subsequent persistent-worker change. This isolates the GPU bridge from that
separate server integration change; it does not prove that the worker caused
the failure.

Only the bridge implementation/build and related diagnostics/tests/docs are
changed. The bridge matches the shared-rank GPU construction implementation
from `48e2d2c8ae105f15c4e6ff8eb572f55d38ead426`, including its explicit
gfx1100 target. The graph algorithm had previously measured about 1.2455x
ready speed on a 250K x 128 synthetic trace diagnostic at `06a662940...`.
That result excludes SQL and is not a measurement of this new combination.
No full-score or official executability claim is made.

The public C ABI header remains byte-identical to the starting commit. Fixed
CAGRA search and its returned results are retained. The production compiler
script has no network dependency and requires only the installed HIP/C API
headers and fixed cuVS library.

## Validation order

1. Reproduce full seekdb build/startup/SQL on the known-good commit in the
   current environment. This checks the previously successful contract rather
   than assuming an old score diagnoses today's environment.
2. Run this candidate's bridge smoke and paired tests against `known-good`.
   The diagnostic helper now accepts `--baseline known-good`, fetching
   `556065c6...` from the answer repository. Its default remains official V5.
3. Build this candidate's full seekdb and run the same SQL checks. Confirm
   `lib=vsag`, GPU index creation, first ANN, result integrity, and reload.
4. Run both 128- and 256-dimensional diagnostics against official V5 without
   trace. Validate recall, throughput, and actual GPU memory before tuning
   parameters or moving a submission branch.
5. Record the evaluated full commit and all official result fields. A generic
   "execution failed" is not enough to claim a particular cause was fixed.

The bridge-only helper commands, after obtaining it from this commit, are:

```bash
python3 prepare_cuvs_gpu_diagnostic.py --revision <this_full_commit> --baseline known-good --rounds 3 --dim 128
python3 prepare_cuvs_gpu_diagnostic.py --revision <this_full_commit> --baseline v5 --rounds 3 --dim 128
python3 prepare_cuvs_gpu_diagnostic.py --revision <this_full_commit> --baseline v5 --rounds 3 --dim 256
```

The helper does not build the full seekdb server. Existing full-GPU SQL smoke
assets are under `docs/gpu-vector-index-hipvs-cuvs/validation/`. Observer
compilation needs the full source and organizer-frozen dependencies, not just
the precompiled template server. A new commit must not be called official-safe
because the unchanged server source or a CPU/mock test passed.
