/* seekdb <-> hipVS bridge: plain-C CAGRA kNN; all cuVS/ROCm deps live here. */
#include "seekdb_cuvs_bridge.h"
#include <cuvs/core/c_api.h>
#include <cuvs/neighbors/cagra.h>
#include <dlpack/dlpack.h>
#ifdef __HIP_PLATFORM_AMD__
#include <cuvs/cuda_runtime.h>
#include <cuvs/library_types.h>
#else
#include <cuda_runtime.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

typedef struct {
  cuvsResources_t        res;
  cuvsCagraIndexParams_t ip;
  cuvsCagraIndex_t       idx;
  cuvsCagraSearchParams_t sp;
  size_t default_itopk;
  long n, dim;
  float *query_buffer, *distance_buffer;
  unsigned *neighbor_buffer;
  long search_capacity;
} sk_cuvs_index;

static int uses_bounded_graph(long n, long dim)
{
  return n >= 8192 && dim >= 16;
}

static void release_search_buffers(sk_cuvs_index* h)
{
  if (h->distance_buffer) cuvsRMMFree(h->res, h->distance_buffer, sizeof(float)*h->search_capacity);
  if (h->neighbor_buffer) cuvsRMMFree(h->res, h->neighbor_buffer, sizeof(unsigned)*h->search_capacity);
  if (h->query_buffer) cuvsRMMFree(h->res, h->query_buffer, sizeof(float)*h->dim);
  h->query_buffer = NULL; h->distance_buffer = NULL; h->neighbor_buffer = NULL;
  h->search_capacity = 0;
}

void* seekdb_cuvs_build(const float* base, long n, long dim)
{
  if (!base || n <= 0 || (uint64_t)n > UINT32_MAX || dim <= 0 ||
      (size_t)n > SIZE_MAX / sizeof(float) / (size_t)dim) return NULL;
  sk_cuvs_index* h = (sk_cuvs_index*)calloc(1, sizeof(*h));
  if (!h) return NULL;
  if (cuvsResourcesCreate(&h->res) != CUVS_SUCCESS) { free(h); return NULL; }

  DLManagedTensor dset; int64_t ds[2] = {n, dim};
  dset.dl_tensor.data = (void*)base;
  dset.dl_tensor.device.device_type = kDLCPU; dset.dl_tensor.device.device_id = 0;
  dset.dl_tensor.ndim = 2;
  dset.dl_tensor.dtype.code = kDLFloat; dset.dl_tensor.dtype.bits = 32; dset.dl_tensor.dtype.lanes = 1;
  dset.dl_tensor.shape = ds; dset.dl_tensor.strides = 0; dset.dl_tensor.byte_offset = 0;

  if (cuvsCagraIndexParamsCreate(&h->ip) != CUVS_SUCCESS) goto fail_res;
  h->ip->build_algo = NN_DESCENT;
  if (uses_bounded_graph(n, dim)) {
    h->ip->graph_degree = 32;
    h->ip->intermediate_graph_degree = 64;
    h->ip->nn_descent_niter = 2;
  } else {
    h->ip->graph_degree = 128;
    h->ip->intermediate_graph_degree = 256;
    h->ip->nn_descent_niter = 100;
  }
  if (cuvsCagraIndexCreate(&h->idx)      != CUVS_SUCCESS) goto fail_ip;
  if (cuvsCagraBuild(h->res, h->ip, &dset, h->idx) != CUVS_SUCCESS ||
      cuvsStreamSync(h->res) != CUVS_SUCCESS) goto fail_idx;
  {
    int64_t actual_n = 0;
    int64_t actual_dim = 0;
    if (h->idx->dtype.code != kDLFloat || h->idx->dtype.bits != 32 ||
        h->idx->dtype.lanes != 1 ||
        cuvsCagraIndexGetSize(h->idx, &actual_n) != CUVS_SUCCESS ||
        cuvsCagraIndexGetDims(h->idx, &actual_dim) != CUVS_SUCCESS ||
        actual_n != n || actual_dim != dim) goto fail_idx;
  }
  h->n = n; h->dim = dim;
  return h;

fail_idx: cuvsCagraIndexDestroy(h->idx);
fail_ip:
  cuvsCagraIndexParamsDestroy(h->ip);
fail_res:
  cuvsResourcesDestroy(h->res); free(h);
  fprintf(stderr, "[seekdb_cuvs] build failed\n");
  return NULL;
}

int seekdb_cuvs_search(void* handle, const float* query, long nq, long topk,
                       unsigned int* out_ids, float* out_dist)
{
  sk_cuvs_index* h = (sk_cuvs_index*)handle;
  if (!h || !query || !out_ids || nq <= 0 || topk <= 0 || topk > h->n ||
      (size_t)nq > SIZE_MAX / sizeof(float) / (size_t)h->dim ||
      (size_t)nq > SIZE_MAX / sizeof(unsigned) / (size_t)topk ||
      (size_t)nq > SIZE_MAX / sizeof(float) / (size_t)topk) return 1;
  int rc = 0;
  const int reuse = nq == 1 && h->search_capacity >= topk;
  float* qd = 0; unsigned* nd = 0; float* dd = 0;
  if (reuse) {
    qd = h->query_buffer; nd = h->neighbor_buffer; dd = h->distance_buffer;
  } else {
    if (cuvsRMMAlloc(h->res, (void**)&qd, sizeof(float)*nq*h->dim)  != CUVS_SUCCESS) { rc=10; goto done; }
    if (cuvsRMMAlloc(h->res, (void**)&nd, sizeof(unsigned)*nq*topk) != CUVS_SUCCESS) { rc=11; goto done; }
    if (cuvsRMMAlloc(h->res, (void**)&dd, sizeof(float)*nq*topk)    != CUVS_SUCCESS) { rc=12; goto done; }
  }
  if (cudaMemcpy(qd, query, sizeof(float)*nq*h->dim, cudaMemcpyDefault) != cudaSuccess) { rc=13; goto done; }
  {
    DLManagedTensor qt, nt, dt; int64_t qs[2]={nq,h->dim}, nsh[2]={nq,topk}, dsh[2]={nq,topk};
    qt.dl_tensor.data=qd; qt.dl_tensor.device.device_type=kDLCUDA; qt.dl_tensor.device.device_id=0;
    qt.dl_tensor.ndim=2; qt.dl_tensor.dtype.code=kDLFloat; qt.dl_tensor.dtype.bits=32; qt.dl_tensor.dtype.lanes=1;
    qt.dl_tensor.shape=qs; qt.dl_tensor.strides=0; qt.dl_tensor.byte_offset=0;
    nt=qt; nt.dl_tensor.data=nd; nt.dl_tensor.dtype.code=kDLUInt; nt.dl_tensor.shape=nsh;
    dt=qt; dt.dl_tensor.data=dd; dt.dl_tensor.dtype.code=kDLFloat; dt.dl_tensor.shape=dsh;
    if (!h->sp) {
      if (cuvsCagraSearchParamsCreate(&h->sp) != CUVS_SUCCESS) { rc=14; goto done; }
      h->default_itopk = h->sp->itopk_size;
    }
    {
      size_t required_itopk = (size_t)topk * 8;
      const size_t query_floor = uses_bounded_graph(h->n, h->dim) ? 16384 : 2048;
      if (required_itopk < query_floor) required_itopk = query_floor;
      if (required_itopk < h->default_itopk) required_itopk = h->default_itopk;
      h->sp->itopk_size = required_itopk;
      h->sp->search_width = 4;
      h->sp->algo = MULTI_CTA;
    }
    cuvsFilter f; f.type=NO_FILTER; f.addr=(uintptr_t)0;
    if (cuvsCagraSearch(h->res, h->sp, h->idx, &qt, &nt, &dt, f) != CUVS_SUCCESS) { rc=15; goto done; }
    if (cuvsStreamSync(h->res) != CUVS_SUCCESS) { rc=19; goto done; }
    if (cudaMemcpy(out_ids, nd, sizeof(unsigned)*nq*topk, cudaMemcpyDefault) != cudaSuccess) { rc=16; goto done; }
    if (out_dist && cudaMemcpy(out_dist, dd, sizeof(float)*nq*topk, cudaMemcpyDefault) != cudaSuccess) { rc=17; goto done; }
  }
done:
  // Retain only single-query scratch space. Batch calls keep their bounded lifetime.
  if (!reuse && rc == 0 && nq == 1) {
    release_search_buffers(h);
    h->query_buffer = qd; h->neighbor_buffer = nd; h->distance_buffer = dd;
    h->search_capacity = topk;
    qd = NULL; nd = NULL; dd = NULL;
  }
  if (!reuse) {
    if (dd) cuvsRMMFree(h->res, dd, sizeof(float)*nq*topk);
    if (nd) cuvsRMMFree(h->res, nd, sizeof(unsigned)*nq*topk);
    if (qd) cuvsRMMFree(h->res, qd, sizeof(float)*nq*h->dim);
  }
  if (rc) fprintf(stderr, "[seekdb_cuvs] search rc=%d: %s\n", rc, cuvsGetLastErrorText());
  return rc;
}

void seekdb_cuvs_free(void* handle)
{
  sk_cuvs_index* h = (sk_cuvs_index*)handle;
  if (!h) return;
  release_search_buffers(h);
  if (h->sp) cuvsCagraSearchParamsDestroy(h->sp);
  cuvsCagraIndexDestroy(h->idx);
  cuvsCagraIndexParamsDestroy(h->ip);
  cuvsResourcesDestroy(h->res);
  free(h);
}

int seekdb_cuvs_serialize(void* handle, const char* filename)
{
  sk_cuvs_index* index = (sk_cuvs_index*)handle;
  if (!index || !filename) return 1;
  if (cuvsCagraSerialize(index->res, filename, index->idx, true) != CUVS_SUCCESS) {
    fprintf(stderr, "[seekdb_cuvs] serialize failed: %s\n", cuvsGetLastErrorText());
    return 1;
  }
  return 0;
}

void* seekdb_cuvs_deserialize(const char* filename, long n, long dim)
{
  if (!filename || n <= 0 || dim <= 0) return NULL;
  sk_cuvs_index* index = (sk_cuvs_index*)calloc(1, sizeof(*index));
  if (!index) return NULL;
  if (cuvsResourcesCreate(&index->res) != CUVS_SUCCESS) goto fail;
  if (cuvsCagraIndexParamsCreate(&index->ip) != CUVS_SUCCESS) goto fail_res;
  if (cuvsCagraIndexCreate(&index->idx) != CUVS_SUCCESS) goto fail_ip;
  if (cuvsCagraDeserialize(index->res, filename, index->idx) != CUVS_SUCCESS) goto fail_idx;
  {
    int64_t actual_n = 0;
    int64_t actual_dim = 0;
    if (cuvsCagraIndexGetSize(index->idx, &actual_n) != CUVS_SUCCESS ||
        cuvsCagraIndexGetDims(index->idx, &actual_dim) != CUVS_SUCCESS ||
        actual_n != n || actual_dim != dim) {
      fprintf(stderr, "[seekdb_cuvs] snapshot shape mismatch: %ld x %ld, expected %ld x %ld\n",
              (long)actual_n, (long)actual_dim, n, dim);
      goto fail_idx;
    }
  }
  index->n = n;
  index->dim = dim;
  return index;
fail_idx:
  cuvsCagraIndexDestroy(index->idx);
fail_ip:
  cuvsCagraIndexParamsDestroy(index->ip);
fail_res:
  cuvsResourcesDestroy(index->res);
fail:
  fprintf(stderr, "[seekdb_cuvs] deserialize failed: %s\n", cuvsGetLastErrorText());
  free(index);
  return NULL;
}

int seekdb_cuvs_cagra_knn(const float* base, long n, long dim,
                          const float* query, long nq, long topk,
                          unsigned int* out_ids)
{
  void* h = seekdb_cuvs_build(base, n, dim);
  if (!h) return 10;
  int rc = seekdb_cuvs_search(h, query, nq, topk, out_ids, NULL);
  seekdb_cuvs_free(h);
  return rc;
}
