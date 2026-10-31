// Standalone CPU fault-injection test. See hipvs_bridge_mock/README.md.
#include "../../../tools/hipvs_bridge/seekdb_cuvs_bridge.c"

// CPU shim for the separate HIP translation unit. This checks ownership only.
int seekdb_cuvs_build_graph(cuvsResources_t res, cuvsCagraIndexParams_t params,
                           DLManagedTensor* dataset, cuvsCagraIndex_t index,
                           unsigned int** graph)
{
  assert(graph != NULL);
  assert(*graph == NULL);
  if (cuvsRMMAlloc(res, (void**)graph, sizeof(unsigned)*(size_t)dataset->dl_tensor.shape[0]*128) != CUVS_SUCCESS) return 1;
  return cuvsCagraBuild(res, params, dataset, index);
}

int main(void)
{
  float base[256 * 4] = {0}, query[8] = {3}, distances[200];
  unsigned ids[200];
  assert(seekdb_cuvs_build(base, 256, 0) == NULL);
  assert(seekdb_cuvs_build(NULL, 256, 4) == NULL);
  fail_allocation = allocations + 1;
  assert(seekdb_cuvs_build(base, 256, 4) == NULL && live == 0);
  fail_allocation = 0;
  fail_allocation = allocations + 2; // graph allocation after the device dataset
  assert(seekdb_cuvs_build(base, 256, 4) == NULL && live == 0);
  fail_allocation = 0;
  fail_copy = copies + 1;
  assert(seekdb_cuvs_build(base, 256, 4) == NULL && live == 0);
  fail_copy = 0;
  fail_build = 1;
  assert(seekdb_cuvs_build(base, 256, 4) == NULL && live == 0);
  fail_build = 0;
  const int before_build = allocations;
  const int before_frees = frees;
  void *h = seekdb_cuvs_build(base, 256, 4);
  assert(h);
  assert(seekdb_cuvs_search(h, query, 1, 10, ids, distances) == 0);
  assert(allocations == before_build + 5 && live == 5 && distances[0] == 3);
  query[0] = 7;
  for (int j = 0; j < 4096; ++j)
    assert(seekdb_cuvs_search(h, query, 1, 10, ids, distances) == 0);
  assert(allocations == before_build + 5 && frees == before_frees && distances[0] == 7);
  // Failed cache growth preserves the old scratch buffers and frees partial allocations.
  fail_allocation = allocations + 2;
  assert(seekdb_cuvs_search(h, query, 1, 100, ids, distances) == 11);
  assert(live == 5);
  fail_allocation = 0;
  assert(seekdb_cuvs_search(h, query, 1, 10, ids, distances) == 0);
  assert(seekdb_cuvs_search(h, query, 1, 100, ids, distances) == 0);
  int after_growth = allocations;
  assert(live == 5 && ids[99] == 99 && distances[99] == 106);
  assert(seekdb_cuvs_search(h, query, 1, 10, ids, NULL) == 0);
  assert(allocations == after_growth);
  // A batch call does not retain its larger scratch allocations.
  assert(seekdb_cuvs_search(h, query, 2, 100, ids, distances) == 0);
  assert(live == 5);
  fail_copy = copies + 3;
  assert(seekdb_cuvs_search(h, query, 1, 10, ids, distances) == 17);
  fail_copy = 0;
  assert(seekdb_cuvs_search(h, query, 1, 10, ids, distances) == 0);
  assert(seekdb_cuvs_search(h, query, 1, 257, ids, distances) == 1);
  assert(seekdb_cuvs_search(h, query, 0, 10, ids, distances) == 1);
  seekdb_cuvs_free(h);
  assert(live == 0);
  h = seekdb_cuvs_build(base, 64, 4);
  assert(h && ((sk_cuvs_index*)h)->device_graph == NULL);
  seekdb_cuvs_free(h);
  assert(live == 0);
  saved_n = 256; saved_dim = 4;
  h = seekdb_cuvs_deserialize("mock", 256, 4);
  assert(h && seekdb_cuvs_search(h, query, 1, 100, ids, distances) == 0);
  seekdb_cuvs_free(h);
  assert(live == 0);
  puts("PASS: reuse, growth failure, batch isolation, copy failure, reload, cleanup");
  return 0;
}
