# Bounded-graph construction results

This candidate retains the known-good server sources and uses the frozen hipVS
CAGRA API directly. For shapes with at least 8,192 rows and 16 dimensions it
builds an NN-Descent graph with final degree 32, intermediate degree 64 and at
most two iterations. Small shapes retain degree 128, intermediate degree 256 and
100 iterations.

The smaller graph is paired with a larger query candidate budget: single-row
searches explicitly use `MULTI_CTA`, width 4 and `itopk_size` at least 16,384.
The budget is reset on every call, search parameters and device scratch are
reused, and all searches still call the fixed `cuvsCagraSearch` implementation.
Original vectors, IDs, FP32 L2 distances and result ordering remain unchanged.

## Why this trade-off is promising

Build work scales with graph degree and NN-Descent iterations. Lowering
32/64/2 versus 128/256/100 removes most graph-refinement work. The sparse graph
can reduce recall, so the query side spends a larger internal candidate budget
and uses the multi-CTA implementation to preserve result quality while
increasing single-query throughput.

Development evidence from the independently inspected leading implementation
reports the following native bridge measurements. These are not measurements of
this branch and are not an official score:

| Dataset | Build API speedup | Recall@10 / @100 | Query QPS vs stock |
| --- | ---: | ---: | ---: |
| 250K x 256 synthetic | 4.344x | 0.996875 / 0.99578125 | 5.06x / 4.95x |
| 250K x 128 SIFT subset | 3.738x | 1 / 0.999921875 | 5.14x / 5.29x |

Those inputs sampled about 0.75 GiB and 0.62 GiB VRAM respectively. The leading
implementation also records a formal passing result of
`build=1.945x, query=4.415x, gates=pass`, worth 14.47/20, but explicitly states
that a formal full-score result was not available. This branch therefore treats
20/20 as a target, not an established outcome.

## Differences from the reference implementation

This branch changes only the bridge implementation and its tests. It does not
copy the reference's bundled SDK, custom snapshot format, adapter persistence
rewrite or persistent worker implementation. It keeps the server and snapshot
layout inherited from the officially executable submission, which reduces
integration risk and leaves enough time to validate the two scoring levers
first.

## Validation still required

The full seekdb build, cold SQL DDL through first query, snapshot reload,
`lib=vsag` path, both formal 250K datasets, Hidden250K Recall, 4096 single-row
SQL queries and official profiler VRAM remain to be run. Any hard-gate failure
still results in zero points.

Local CPU shims verify bounded and small-shape parameters, explicit
`MULTI_CTA`, query-budget reset, scratch reuse, failure propagation and cleanup.
They do not establish GPU execution or performance.
