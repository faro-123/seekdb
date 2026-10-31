#include "../../../tools/hipvs_bridge/seekdb_cuvs_graph_algorithms.h"
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdio>
#include <random>
#include <vector>

using namespace seekdb_gpu_graph;

// Exercise the same compare-exchange schedule used by the HIP block sort.
static void test_sort()
{
  std::mt19937 generator(20261030);
  for (unsigned round = 0; round < 8; ++round) {
    std::array<uint64_t, candidate_capacity> keys, expected;
    for (unsigned i = 0; i < keys.size(); ++i) {
      keys[i] = i % 5 == 0 ? UINT64_MAX : generator() % (round + 1);
    }
    expected = keys;
    std::sort(expected.begin(), expected.end());
    for (unsigned size = 2; size <= keys.size(); size <<= 1) {
      for (unsigned stride = size >> 1; stride; stride >>= 1) {
        for (unsigned i = 0; i < keys.size(); ++i) {
          const unsigned partner = i ^ stride;
          if (partner > i) { exchange_keys(keys[i], keys[partner], (i & size) == 0); }
        }
      }
    }
    assert(keys == expected);
  }
}

static void test_merge()
{
  constexpr uint32_t rows = 1024;
  for (uint32_t row : {0U, 100U, rows - 1}) {
    for (unsigned mode = 0; mode < 4; ++mode) {
      std::array<uint32_t, graph_degree> graph, incoming, scratch;
      for (unsigned i = 0; i < graph_degree; ++i) {
        graph[i] = (row + 2 + i) % rows;
        incoming[i] = mode == 0 ? row : mode == 1 ? rows : (row + 200 + i / 2) % rows;
      }
      if (mode == 3) { graph.back() = row == 0 ? rows - 1 : row - 1; }
      const auto original = graph;
      assert(merge_reverse(graph.data(), incoming.data(), incoming.size(), row, rows, scratch.data()));
      for (unsigned i = 0; i < graph_degree; ++i) {
        assert(graph[i] < rows && graph[i] != row);
        assert(!contains(graph.data(), i, graph[i]));
        if (i < graph_degree / 2) { assert(graph[i] == original[i]); }
      }
      assert(contains(graph.data(), graph_degree, row + 1 == rows ? 0 : row + 1));
      assert(contains(graph.data(), graph_degree, row == 0 ? rows - 1 : row - 1));
    }
  }
  std::array<uint32_t, graph_degree> duplicate{}, scratch{};
  assert(!merge_reverse(duplicate.data(), nullptr, 0, 1, rows, scratch.data()));
}

static void insert_rank(std::array<uint64_t, rank_table_capacity> &table, uint32_t id, unsigned rank)
{
  unsigned slot = hash_id(id) & (rank_table_capacity - 1);
  while (table[slot] != UINT64_MAX) { slot = (slot + 1) & (rank_table_capacity - 1); }
  table[slot] = rank_key(id, rank);
}

static void test_rank_lookup()
{
  std::array<uint64_t, rank_table_capacity> table;
  table.fill(UINT64_MAX);
  assert(find_rank(table.data(), 42) == UINT32_MAX);
  // Force a long collision chain, including wrapped slots.
  std::vector<uint32_t> collisions;
  for (uint32_t id = 0; collisions.size() < intermediate_degree; ++id) {
    if ((hash_id(id) & (rank_table_capacity - 1)) == rank_table_capacity - 1) {
      insert_rank(table, id, unsigned(collisions.size()));
      collisions.push_back(id);
    }
  }
  for (unsigned rank = 0; rank < collisions.size(); ++rank) {
    assert(find_rank(table.data(), collisions[rank]) == rank);
  }
  assert(find_rank(table.data(), collisions.back() + 1) == UINT32_MAX);
  assert(find_rank(table.data(), UINT32_MAX) == UINT32_MAX);
  // A bounded miss must terminate even if the table is full.
  for (unsigned i = 0; i < table.size(); ++i) { table[i] = rank_key(i, i); }
  assert(find_rank(table.data(), 100000) == UINT32_MAX);
}

static void test_detour_equivalence(unsigned rows)
{
  const unsigned degree = std::min(rows - 1, intermediate_degree);
  std::mt19937 generator(20261030 + rows);
  std::vector<uint32_t> knn(rows * degree), ids;
  for (unsigned row = 0; row < rows; ++row) {
    ids.clear();
    for (unsigned id = 0; id < rows; ++id) { if (id != row) { ids.push_back(id); } }
    std::shuffle(ids.begin(), ids.end(), generator);
    std::copy_n(ids.begin(), degree, knn.begin() + row * degree);
  }
  for (unsigned row : {0U, rows / 2, rows - 1}) {
    const auto *neighbors = knn.data() + row * degree;
    std::array<uint64_t, rank_table_capacity> table;
    table.fill(UINT64_MAX);
    std::array<unsigned, intermediate_degree> expected{}, actual{};
    for (unsigned rank = 0; rank < degree; ++rank) { insert_rank(table, neighbors[rank], rank); }
    // Original exact definition, independent of the hash counting traversal.
    for (unsigned rank = 0; rank < degree; ++rank) {
      for (unsigned via = 0; via < rank; ++via) {
        if (contains(knn.data() + neighbors[via] * degree, degree, neighbors[rank])) { ++expected[rank]; }
      }
    }
    for (unsigned via = 0; via + 1 < degree; ++via) {
      for (unsigned k = 0; k < degree; ++k) {
        const unsigned rank = find_rank(table.data(), knn[neighbors[via] * degree + k]);
        if (rank < degree && rank > via) { ++actual[rank]; }
      }
    }
    assert(actual == expected);
  }
}

int main()
{
  test_sort();
  test_merge();
  test_rank_lookup();
  test_detour_equivalence(129);
  test_detour_equivalence(512);
  std::puts("PASS: sort, merge/ring validity, colliding rank lookup, exact detour equivalence");
}
