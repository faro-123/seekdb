# Generic platform execution failure: status and build correction

The user reported that a recent contest submission displayed only "execution
failed". The platform did not expose the checked-out commit, taskStatus,
failMsg, exit code, or logs. The report followed the selection of
`06a662940de619c48f8943de2ac9e7baa48d7176`, but the actual evaluated commit
cannot be confirmed. No official success or high-score claim is justified.

## Major: compile HIP kernels for the fixed runtime GPU

Location: `tools/hipvs_bridge/build.sh`, hipcc invocation.

The bridge now includes a `.hip` translation unit, but the earlier script did
not pass an offload target. hipcc discovers GPUs on the compilation machine
when none is specified. A build worker without visible GPUs cannot rely on
this discovery; a worker with a different GPU can generate a code object that
does not match the contest's gfx1100 runtime. Having compiled successfully on
the interactive GPU instance does not establish this build contract.

The script now passes `--offload-arch=gfx1100`, matching the fixed contest
target documented for V5. The correction changes compilation targeting only,
not graph construction or query parameters. It adds no dependency or network
access to the official build.

Reference: [AMD ROCm compiler reference](https://rocm.docs.amd.com/projects/llvm-project/en/latest/reference/rocmcc.html),
the hipcc architecture-discovery comparison and `--offload-arch` option.

The focused `test_hipvs_bridge_build.py` exercises the real shell script with
a fake compiler that refuses implicit GPU discovery. It failed with exit 86
before the correction and passed after it. It also checks both production
translation units are passed and an output path containing spaces works.
This is a build-command test, not an actual HIP compiler/GPU test. The existing
11 metric and device-selection tests also pass.

## What remains unknown

There is no evidence identifying which phase of the official task failed.
The architecture issue is a corrected reliability risk, not a confirmed cause
of this particular failure. Repository access, full seekdb compilation/linking,
server startup, actual GPU execution, and official quality/resource gates
still need investigation. The next teammate validation must build the full
server with the frozen dependencies and official configuration, then run SQL
validation. A bridge-only test is insufficient.

Request the task's actual checked-out commit, failing phase (checkout / bridge
build / seekdb build / startup / GPU test), exit code, and last error-log lines
from the organizer if the UI cannot show them. Do not infer a stage from the
generic status text or assume resubmitting the same commit fixes it.
