// Bounded GPU construction only. Query execution stays in the fixed cuVS C API.
#include <hip/hip_runtime.h>
#include <cuvs/core/c_api.h>
#include <cuvs/neighbors/cagra.h>
#include <cuvs/neighbors/ivf_pq.h>
#include "seekdb_cuvs_graph_algorithms.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>

namespace {
using namespace seekdb_gpu_graph;
constexpr unsigned batch_capacity = 512;

void check_cuvs(int status)
{
  if (status != CUVS_SUCCESS) {
    const char *error = cuvsGetLastErrorText();
    throw std::runtime_error(error ? error : "cuVS construction call failed without error text");
  }
}

void check_hip(hipError_t status)
{
  if (status != hipSuccess) { throw std::runtime_error(hipGetErrorString(status)); }
}

struct DeviceBuffer {
  cuvsResources_t res;
  void *ptr = nullptr;
  size_t bytes;
  DeviceBuffer(cuvsResources_t resource, size_t size) : res(resource), bytes(size)
  {
    check_cuvs(cuvsRMMAlloc(res, &ptr, bytes));
  }
  ~DeviceBuffer() { if (ptr) { cuvsRMMFree(res, ptr, bytes); } }
  DeviceBuffer(const DeviceBuffer &) = delete;
  DeviceBuffer &operator=(const DeviceBuffer &) = delete;
};

// Opt-in diagnostics only: synchronization changes execution scheduling.
struct BuildTrace {
  enum Stage { train, lookup, refine, prune, reverse, wrap, stage_count };
  cuvsResources_t res;
  bool enabled;
  double milliseconds[stage_count]{};
  std::chrono::steady_clock::time_point started;
  explicit BuildTrace(cuvsResources_t resource) : res(resource)
  {
    const char *value = std::getenv("SEEKDB_CUVS_BUILD_TRACE");
    enabled = value && value[0] == '1' && value[1] == '\0';
  }
  void begin()
  {
    if (enabled) { check_cuvs(cuvsStreamSync(res)); started = std::chrono::steady_clock::now(); }
  }
  void end(Stage stage)
  {
    if (enabled) {
      check_cuvs(cuvsStreamSync(res));
      milliseconds[stage] += std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - started).count();
    }
  }
  void report(uint32_t rows) const
  {
    if (enabled) {
      std::fprintf(stderr, "[seekdb_cuvs] build_stage_ms rows=%u train=%.3f lookup=%.3f "
                   "refine=%.3f prune=%.3f reverse=%.3f wrap=%.3f\n", rows,
                   milliseconds[train], milliseconds[lookup], milliseconds[refine],
                   milliseconds[prune], milliseconds[reverse], milliseconds[wrap]);
    }
  }
};

template<unsigned Count>
__device__ void sort_keys(uint64_t *keys)
{
  for (unsigned size = 2; size <= Count; size <<= 1) {
    for (unsigned stride = size >> 1; stride; stride >>= 1) {
      for (unsigned i = threadIdx.x; i < Count; i += blockDim.x) {
        const unsigned partner = i ^ stride;
        if (partner > i) { exchange_keys(keys[i], keys[partner], (i & size) == 0); }
      }
      __syncthreads();
    }
  }
}

__global__ void refine_candidates(const float *data, uint32_t rows, unsigned dim,
                                  uint32_t offset, const int64_t *candidates,
                                  unsigned candidate_count, uint32_t *knn, int *error)
{
  __shared__ uint64_t keys[candidate_capacity];
  for (unsigned i = threadIdx.x; i < candidate_capacity; i += blockDim.x) { keys[i] = ~uint64_t(0); }
  __syncthreads();
  const uint32_t row = offset + blockIdx.x;
  const unsigned lane = threadIdx.x % warpSize;
  const unsigned warp = threadIdx.x / warpSize;
  const unsigned warps = blockDim.x / warpSize;
  for (unsigned i = warp; i < candidate_count; i += warps) {
    const int64_t id = candidates[size_t(blockIdx.x) * candidate_count + i];
    if (id >= 0 && uint64_t(id) < rows && uint32_t(id) != row) {
      float distance = 0;
      for (unsigned d = lane; d < dim; d += warpSize) {
        const float delta = data[size_t(row) * dim + d] - data[size_t(id) * dim + d];
        distance += delta * delta;
      }
      for (unsigned shift = warpSize / 2; shift; shift >>= 1) { distance += __shfl_down(distance, shift); }
      if (lane == 0) { keys[i] = (uint64_t(__float_as_uint(distance)) << 32) | uint32_t(id); }
    }
  }
  __syncthreads();
  sort_keys<candidate_capacity>(keys);
  const unsigned degree = rows - 1 < intermediate_degree ? rows - 1 : intermediate_degree;
  const unsigned rank = threadIdx.x;
  if (rank < intermediate_degree) {
    const uint32_t id = rank < degree ? uint32_t(keys[rank]) : UINT32_MAX;
    if (rank < degree && (keys[rank] == ~uint64_t(0) ||
        (rank > 0 && id == uint32_t(keys[rank - 1])))) { atomicExch(error, 1); }
    knn[size_t(row) * intermediate_degree + rank] = id;
  }
}

__global__ void prune_graph(const uint32_t *knn, uint32_t rows, uint32_t *graph)
{
  __shared__ uint64_t rank_table[rank_table_capacity];
  __shared__ unsigned detours[intermediate_degree];
  __shared__ uint64_t scores[intermediate_degree];
  const uint32_t row = blockIdx.x;
  const unsigned rank = threadIdx.x;
  const unsigned degree = rows - 1 < intermediate_degree ? rows - 1 : intermediate_degree;
  for (unsigned i = rank; i < rank_table_capacity; i += blockDim.x) { rank_table[i] = UINT64_MAX; }
  detours[rank] = 0;
  __syncthreads();
  if (rank < degree) {
    const uint32_t id = knn[size_t(row) * intermediate_degree + rank];
    unsigned slot = hash_id(id) & (rank_table_capacity - 1);
    while (atomicCAS(reinterpret_cast<unsigned long long *>(rank_table + slot),
                     ~0ULL, static_cast<unsigned long long>(rank_key(id, rank))) != ~0ULL) {
      slot = (slot + 1) & (rank_table_capacity - 1);
    }
  }
  __syncthreads();
  // Read each neighbor-of-neighbor row once, coalesced across the block.
  // A shared rank lookup counts the same exact two-hop detours as before.
  for (unsigned via_rank = 0; via_rank + 1 < degree; ++via_rank) {
    const uint32_t via = knn[size_t(row) * intermediate_degree + via_rank];
    if (rank < degree) {
      const uint32_t candidate = knn[size_t(via) * intermediate_degree + rank];
      const unsigned target_rank = find_rank(rank_table, candidate);
      if (target_rank < degree && target_rank > via_rank) { atomicAdd(detours + target_rank, 1U); }
    }
  }
  __syncthreads();
  scores[rank] = rank < degree ? (uint64_t(detours[rank]) << 32) | rank : UINT64_MAX;
  __syncthreads();
  sort_keys<intermediate_degree>(scores);
  if (rank < graph_degree) { graph[size_t(row) * graph_degree + rank] = knn[size_t(row) * intermediate_degree + uint32_t(scores[rank])]; }
}

__global__ void reverse_edges(const uint32_t *graph, uint32_t rows,
                              unsigned rank, uint32_t *incoming, uint32_t *counts)
{
  const uint32_t row = blockIdx.x * blockDim.x + threadIdx.x;
  if (row < rows) {
    const uint32_t target = graph[size_t(row) * graph_degree + rank];
    const uint32_t slot = atomicAdd(counts + target, 1U);
    if (slot < graph_degree) { incoming[size_t(target) * graph_degree + slot] = row; }
  }
}

__global__ void merge_graph(uint32_t *graph, const uint32_t *incoming,
                            const uint32_t *counts, uint32_t rows, int *error)
{
  __shared__ uint32_t scratch[graph_degree];
  if (threadIdx.x == 0) {
    const uint32_t row = blockIdx.x;
    const unsigned count = counts[row] < graph_degree ? counts[row] : graph_degree;
    if (!merge_reverse(graph + size_t(row) * graph_degree,
                       incoming + size_t(row) * graph_degree, count, row, rows, scratch)) { atomicExch(error, 1); }
  }
}

void check_graph_error(cuvsResources_t res, hipStream_t stream, int *device_error)
{
  int error = 0;
  check_hip(hipMemcpyAsync(&error, device_error, sizeof(error), hipMemcpyDeviceToHost, stream));
  check_cuvs(cuvsStreamSync(res));
  if (error) { throw std::runtime_error("GPU graph construction produced invalid or duplicate neighbors"); }
}
} // namespace

extern "C" int seekdb_cuvs_build_graph(cuvsResources_t res, cuvsCagraIndexParams_t params,
                                      DLManagedTensor *dataset, cuvsCagraIndex_t index,
                                      unsigned int **graph)
{
  cuvsIvfPqIndexParams_t train = nullptr;
  cuvsIvfPqIndex_t ivf = nullptr;
  auto cleanup = [&] {
    cuvsStreamSync(res);
    if (ivf) { cuvsIvfPqIndexDestroy(ivf); ivf = nullptr; }
    if (train) { cuvsIvfPqIndexParamsDestroy(train); train = nullptr; }
  };
  try {
    if (params->graph_degree != graph_degree || params->intermediate_graph_degree != intermediate_degree) {
      throw std::runtime_error("bounded GPU builder requires graph degrees 128/256");
    }
    const uint32_t rows = uint32_t(dataset->dl_tensor.shape[0]);
    const unsigned dim = unsigned(dataset->dl_tensor.shape[1]);
    const auto *data = static_cast<const float *>(dataset->dl_tensor.data);
    const unsigned candidates = rows < candidate_capacity ? rows : candidate_capacity;
    hipStream_t stream;
    check_cuvs(cuvsStreamGet(res, &stream));
    BuildTrace trace(res);
    DeviceBuffer flag(res, sizeof(int));
    auto *error = static_cast<int *>(flag.ptr);
    check_hip(hipMemsetAsync(error, 0, sizeof(int), stream));
    trace.begin();
    check_cuvs(cuvsIvfPqIndexParamsCreate(&train));
    train->metric = L2Expanded;
    train->add_data_on_build = true;
    train->n_lists = rows / 2000 > 0 ? rows / 2000 : 1;
    train->kmeans_n_iters = 10;
    train->kmeans_trainset_fraction = std::fmin(1.0, 1.0 / std::sqrt(double(rows) * 1e-5));
    train->pq_bits = 8;
    train->pq_dim = dim % 2 == 0 ? dim / 2 : 0;
    train->codebook_kind = PER_SUBSPACE;
    train->conservative_memory_allocation = true;
    check_cuvs(cuvsIvfPqIndexCreate(&ivf));
    check_cuvs(cuvsIvfPqBuild(res, train, dataset, ivf));
    check_cuvs(cuvsStreamSync(res));
    trace.end(BuildTrace::train);
    uint32_t *output_graph = nullptr;
    {
      DeviceBuffer knn(res, size_t(rows) * intermediate_degree * sizeof(uint32_t));
      auto *neighbors = static_cast<uint32_t *>(knn.ptr);
      {
        DeviceBuffer candidate_ids(res, size_t(batch_capacity) * candidates * sizeof(int64_t));
        DeviceBuffer candidate_distances(res, size_t(batch_capacity) * candidates * sizeof(float));
        for (uint64_t offset = 0; offset < rows; offset += batch_capacity) {
          const unsigned count = rows - offset < batch_capacity ? unsigned(rows - offset) : batch_capacity;
          int64_t query_shape[2] = {count, dim}, result_shape[2] = {count, candidates};
          DLManagedTensor queries = *dataset;
          queries.dl_tensor.data = const_cast<float *>(data + size_t(offset) * dim);
          queries.dl_tensor.shape = query_shape;
          DLManagedTensor ids = queries;
          ids.dl_tensor.data = candidate_ids.ptr;
          ids.dl_tensor.shape = result_shape;
          ids.dl_tensor.dtype.code = kDLInt;
          ids.dl_tensor.dtype.bits = 64;
          DLManagedTensor distances = ids;
          distances.dl_tensor.data = candidate_distances.ptr;
          distances.dl_tensor.dtype.code = kDLFloat;
          distances.dl_tensor.dtype.bits = 32;
          trace.begin();
          check_cuvs(cuvsIvfPqSearch(res, params->graph_build_params->ivf_pq_search_params,
                                    ivf, &queries, &ids, &distances));
          trace.end(BuildTrace::lookup);
          trace.begin();
          hipLaunchKernelGGL(refine_candidates, dim3(count), dim3(256), 0, stream,
                             data, rows, dim, uint32_t(offset), static_cast<const int64_t *>(candidate_ids.ptr),
                             candidates, neighbors, error);
          check_hip(hipGetLastError());
          trace.end(BuildTrace::refine);
        }
        check_graph_error(res, stream, error);
        cleanup();
      }
      trace.begin();
      // Allocate the retained graph after training and candidate selection end.
      // The C bridge owns this pointer even if a later construction step fails.
      check_cuvs(cuvsRMMAlloc(res, reinterpret_cast<void **>(graph), size_t(rows) * graph_degree * sizeof(uint32_t)));
      output_graph = *graph;
      hipLaunchKernelGGL(prune_graph, dim3(rows), dim3(256), 0, stream, neighbors, rows, output_graph);
      check_hip(hipGetLastError());
      check_cuvs(cuvsStreamSync(res));
      trace.end(BuildTrace::prune);
    }
    {
      trace.begin();
      DeviceBuffer incoming(res, size_t(rows) * graph_degree * sizeof(uint32_t));
      DeviceBuffer counts(res, size_t(rows) * sizeof(uint32_t));
      check_hip(hipMemsetAsync(counts.ptr, 0, counts.bytes, stream));
      // Rank-ordered launches prevent low-priority edges from filling the cap
      // before higher-priority edges, as in the fixed CAGRA optimizer.
      for (unsigned rank = 0; rank < graph_degree; ++rank) {
        hipLaunchKernelGGL(reverse_edges, dim3(unsigned((uint64_t(rows) + 255) / 256)),
                           dim3(256), 0, stream, output_graph, rows, rank,
                           static_cast<uint32_t *>(incoming.ptr), static_cast<uint32_t *>(counts.ptr));
        check_hip(hipGetLastError());
      }
      hipLaunchKernelGGL(merge_graph, dim3(rows), dim3(256), 0, stream, output_graph,
                         static_cast<const uint32_t *>(incoming.ptr),
                         static_cast<const uint32_t *>(counts.ptr), rows, error);
      check_hip(hipGetLastError());
      check_graph_error(res, stream, error);
      trace.end(BuildTrace::reverse);
    }
    trace.begin();
    int64_t graph_shape[2] = {rows, graph_degree};
    DLManagedTensor graph_tensor = *dataset;
    graph_tensor.dl_tensor.data = output_graph;
    graph_tensor.dl_tensor.shape = graph_shape;
    graph_tensor.dl_tensor.dtype.code = kDLUInt;
    graph_tensor.dl_tensor.dtype.bits = 32;
    check_cuvs(cuvsCagraIndexFromArgs(res, L2Expanded, &graph_tensor, dataset, index));
    trace.end(BuildTrace::wrap);
    trace.report(rows);
    cuvsSetLastErrorText(nullptr);
    return CUVS_SUCCESS;
  } catch (const std::exception &error) {
    const std::string message = error.what();
    cleanup();
    cuvsSetLastErrorText(message.c_str());
    return CUVS_ERROR;
  }
}
