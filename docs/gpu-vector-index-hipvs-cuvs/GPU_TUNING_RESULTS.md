# GPU tuning evidence

These measurements were supplied by the user from their Radeon Cloud instance
on 2026-10-29. The assistant did not directly control the instance. The reported
software is ROCm 7.2.4, with 16 CPU cores of cgroup quota and 55 GiB memory.
Both bridge versions link to the same installed hipVS library. Its exact source
commit has not been independently verified on the instance.

## Iterative construction withdrawn

Baseline source: `634888d9b037153fa143d2c913ba4b25503e1d62`.
Candidate bridge: `d0eb7ee73483d0190f486bcd8c7deb2562ec01c8`.
Diagnostic tool: `3bac581000cda4365acc987af1c27e9a887608ab`.

The test alternated baseline/candidate for three rounds. Its ready timer covers
bridge build, snapshot serialize, original-handle release, reload, and first
query. It excludes SQL and DDL. Recall uses CPU exact Top-100. These are
development measurements, not official scores or gate results.

| Dataset | Queries | V5 median ready | Iterative median ready | V5 / iterative |
| --- | --- | --- | --- | --- |
| Template demo: 10K x 128 | 100 | 0.824028 s | 1.824523 s | 0.451641x |
| Synthetic Gaussian: 250K x 128 | 256 | 16.863077 s | 41.048169 s | 0.410812x |

For 250K x 128, raw ready seconds were:

```text
V5:        16.863077212125063, 17.111781511455774, 15.981730617582798
Iterative: 41.09958801232278, 40.941389152780175, 41.04816926829517
```

Median Recall@10 was 0.990625 for V5 and 0.995703125 for iterative; median
Recall@100 was 0.966875 and 0.9700390625. Both versions produced valid IDs in
all reported runs. Snapshot size was identical at 256001713 bytes. Query bridge
QPS remained close: approximately 349 versus 352 at Top-10 and 348 versus 350
at Top-100. The observed recall improvement did not offset a 2.43x construction
slowdown. This does not establish any result on SIFT250K or Hidden250K.

V5's build call alone took a median 16.347218 s; snapshot serialization plus
free/reload was about 0.5 s. This evidence directs further work toward graph
construction rather than snapshot I/O or small scratch allocations.

The submission branch restores V5 NN-Descent with graph degree 128,
intermediate degree 256, and iteration cap 100. It retains the reusable
large-stack worker and scratch buffers. The worker still needs full SQL/GPU
validation; the bridge diagnostic cannot measure its observer integration.
No full-score claim is made. New construction experiments must remain separate
from the submission branch until measured on both 128- and 256-dimensional
data, then checked by the official evaluator.

## Built-in IVF-PQ refinement withdrawn

The user supplied a two-round alternating diagnostic on the template 10K x 128
data with 100 queries. Both versions had Recall@10 and Recall@100 of 1.0.

| Version | Ready seconds | Sampled peak VRAM bytes |
| --- | --- | --- |
| V5 round 1 | 1.176538665 | 485339136 |
| IVF-PQ round 1 | 2.764517145 | 13467451392 |
| IVF-PQ round 2 | 2.165080087 | 13470920704 |
| V5 round 2 | 0.598878495 | 550404096 |

The median ready ratio is 0.360155x. The ratio of maximum sampled peaks is
24.474601x, far above the 1.25x resource target. Sampling every 20 ms provides
a lower bound on peaks, not an official gate measurement. The runs shared one
process and later initial VRAM was about 0.5 GB; despite that limitation, this
is sufficient reason not to promote this path or upscale it to 250K.

Source inspection attributes the large allocation to the fixed GPU refinement
implementation's per-query candidate-vector lists. A separate bounded GPU
construction experiment avoids those lists; its performance remains unmeasured.

## Bounded construction: 10K gain did not scale

Candidate `4836e67220a36286420f6790bfd2ffc023e8b74b` compiled and passed
10K x 32 persistence smoke testing on the user's GPU. On the 10K x 128 demo,
three alternating rounds reported 1.691122x ready speed, no recall loss, and
1.565264x sampled VRAM peak ratio. This did not establish the memory gate.

The user then repeated the 250K x 128 synthetic test twice, with 256 queries
and three alternating rounds per run. The source and inputs were unchanged.
Both logs used the same output path, so the later JSON overwrites the earlier
file; both terminal results were supplied in the pasted attachment.

| Run | V5 median ready | Candidate median ready | Ready speedup | Recall@10 delta | Recall@100 delta | Sampled peak ratio |
| --- | --- | --- | --- | --- | --- | --- |
| 1 | 15.827008 s | 17.912748 s | 0.883561x | -0.01328125 | -0.02933594 | 1.750801x |
| 2 | 16.150208 s | 17.870709 s | 0.903725x | -0.01406250 | -0.03164062 | 1.365942x |

Candidate peak bytes were 1602867200 and 1602887680. V5 peak bytes were
915505152 and 1173467136. The changing denominator and shared-process initial
VRAM limit interpretation of the relative sampler result. Neither ratio is
evidence that the 1.25x resource target passes. Both median recall losses
exceed the allowed 0.005 paired regression target. These synthetic diagnostics
are not official SIFT/Hidden gate results, but clearly reject promotion.

The next revision replaces repeated remote neighbor scans with an exact
shared rank hash table, eliminates the per-row Bloom allocation, delays graph
allocations until their consuming phases, and collects reverse edges in rank
order. It adds opt-in synchronized phase timings to locate remaining cost.
Its quality, timing, and actual resource use are unmeasured until a new GPU run.

## Shared-rank revision: diagnostic result and environment reset

The user supplied two alternating trace rounds for candidate
`06a662940de619c48f8943de2ac9e7baa48d7176` on newly generated 250K x 128
synthetic data (seed 20261029, 256 queries). Ready seconds were 19.148122849
and 16.095424537 for V5, and 14.365920920 and 13.930208458 for the candidate.
The trace-run median ratio was 1.245525x. Median Recall@10 delta was
+0.00214844; Recall@100 delta was -0.00068359. These trace results fit the
paired 0.005 recall-loss target for this synthetic case, not official gates.

Candidate phase milliseconds were:

| Round | Train | IVF-PQ lookup | FP32 refine | Prune | Reverse/merge |
| --- | --- | --- | --- | --- | --- |
| 1 | 1600.705 | 11443.288 | 489.160 | 131.446 | 182.861 |
| 2 | 1225.072 | 11401.261 | 489.601 | 131.541 | 182.119 |

Candidate lookup dominates this synchronized diagnostic. Clean untraced timing
and both dimensions remain unverified. All reported VRAM initial and peak
samples were exactly 624242688 bytes for both libraries, so the printed 1.0x
ratio is discarded as inconclusive. GPU attribution was not captured then.

The user next reported a Jupyter kernel-start failure. A later terminal showed
GPU[0] at 0000:43:00.0, 28028928 VRAM bytes used, cgroup memory.current
162508800, memory.max 59055800320, and all OOM counters zero. The former test
library was absent. This establishes changed environment state, not the cause
of the prior failure; zero current OOM counters cannot diagnose a previous
environment. Recovery now rebuilds pinned inputs and selects the current bus.

## Platform execution failure reported

After preparing the selected candidate, the user reported a contest execution
failure. Only generic failure text was available; the checked-out commit and
failure stage are unconfirmed. This supersedes any interpretation that the
bridge diagnostic established full contest executability. The build now pins
gfx1100 instead of depending on compiler-host GPU discovery. See
`EXECUTION_FAILURE_FOLLOWUP.md` for the review, narrow regression evidence, and
remaining full-server validation requirements. The actual failure cause is
still unknown.
