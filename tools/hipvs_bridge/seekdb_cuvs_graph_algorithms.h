#pragma once
#include <stdint.h>

#ifdef __HIPCC__
#define SK_GRAPH_HD __host__ __device__
#else
#define SK_GRAPH_HD
#endif

namespace seekdb_gpu_graph {
constexpr unsigned graph_degree = 128;
constexpr unsigned intermediate_degree = 256;
constexpr unsigned candidate_capacity = 2048;
constexpr unsigned rank_table_capacity = 1024;

SK_GRAPH_HD inline void exchange_keys(uint64_t &a, uint64_t &b, bool ascending)
{
  if (ascending ? a > b : a < b) { uint64_t temporary = a; a = b; b = temporary; }
}

SK_GRAPH_HD inline uint32_t hash_id(uint32_t id)
{
  id ^= id >> 16;
  id *= 0x7feb352dU;
  id ^= id >> 15;
  id *= 0x846ca68bU;
  return id ^ (id >> 16);
}

SK_GRAPH_HD inline uint64_t rank_key(uint32_t id, unsigned rank)
{
  return (uint64_t(rank) << 32) | id;
}

// Open addressing at <=25% occupancy. Empty entries stop unsuccessful lookups.
SK_GRAPH_HD inline unsigned find_rank(const uint64_t *table, uint32_t id)
{
  unsigned slot = hash_id(id) & (rank_table_capacity - 1);
  for (unsigned probe = 0; probe < rank_table_capacity; ++probe) {
    const uint64_t key = table[slot];
    if (key == UINT64_MAX) { break; }
    if (uint32_t(key) == id) { return unsigned(key >> 32); }
    slot = (slot + 1) & (rank_table_capacity - 1);
  }
  return UINT32_MAX;
}

SK_GRAPH_HD inline bool contains(const uint32_t *ids, unsigned count, uint32_t id)
{
  for (unsigned i = 0; i < count; ++i) { if (ids[i] == id) { return true; } }
  return false;
}

// Keep half the outgoing edges, add distinct reverse edges, then fill from the
// remaining outgoing edges. Two ring edges guarantee global connectivity.
SK_GRAPH_HD inline bool merge_reverse(uint32_t *outgoing, const uint32_t *incoming,
                                     unsigned incoming_count, uint32_t row,
                                     uint32_t rows, uint32_t *scratch)
{
  unsigned count = graph_degree / 2;
  for (unsigned i = 0; i < count; ++i) { scratch[i] = outgoing[i]; }
  for (unsigned i = 0; i < incoming_count && count < graph_degree; ++i) {
    const uint32_t id = incoming[i];
    if (id < rows && id != row && !contains(scratch, count, id)) { scratch[count++] = id; }
  }
  for (unsigned i = graph_degree / 2; i < graph_degree && count < graph_degree; ++i) {
    if (!contains(scratch, count, outgoing[i])) { scratch[count++] = outgoing[i]; }
  }
  if (count != graph_degree) { return false; }
  const uint32_t ring[2] = {row + 1 == rows ? 0 : row + 1, row == 0 ? rows - 1 : row - 1};
  for (unsigned i = 0; i < 2; ++i) {
    if (!contains(scratch, graph_degree, ring[i])) {
      unsigned slot = graph_degree - 1;
      if (scratch[slot] == ring[1 - i]) { --slot; }
      scratch[slot] = ring[i];
    }
  }
  for (unsigned i = 0; i < graph_degree; ++i) { outgoing[i] = scratch[i]; }
  return true;
}
} // namespace seekdb_gpu_graph

#undef SK_GRAPH_HD
