#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

// CPU-only fault-injection shim; this does not emulate GPU numerical quality.
typedef void* cuvsResources_t;
typedef void* cuvsCagraIndex_t;
typedef struct { uint32_t n_probes; int lut_dtype, internal_distance_dtype; } IvfSearch;
typedef IvfSearch* cuvsIvfPqSearchParams_t;
typedef struct { cuvsIvfPqSearchParams_t ivf_pq_search_params; float refinement_rate; } GraphParams;
typedef struct { int build_algo, graph_degree, intermediate_graph_degree, nn_descent_niter;
                 GraphParams* graph_build_params; } IndexParams;
typedef IndexParams* cuvsCagraIndexParams_t;
typedef struct { size_t itopk_size; int search_width; } SearchParams;
typedef SearchParams* cuvsCagraSearchParams_t;
typedef struct {
  void *data;
  struct { int device_type, device_id; } device;
  int ndim;
  struct { int code, bits, lanes; } dtype;
  long *shape, *strides;
  uint64_t byte_offset;
} DLTensor;
typedef struct { DLTensor dl_tensor; } DLManagedTensor;
typedef struct { int type; uintptr_t addr; } cuvsFilter;
enum { CUVS_SUCCESS, NN_DESCENT, ITERATIVE_CAGRA_SEARCH, IVF_PQ, HIP_R_32F, kDLCPU, kDLCUDA, kDLFloat, kDLUInt, NO_FILTER,
       cudaSuccess = 0, cudaMemcpyDefault = 0 };
static int allocations, frees, live, fail_allocation, copies, fail_copy;
static int fail_build;
static const char* last_error = "injected failure";
static long saved_n, saved_dim;
static int cuvsResourcesCreate(cuvsResources_t *r) { *r = (void*)1; return 0; }
static int cuvsResourcesDestroy(cuvsResources_t r) { (void)r; return 0; }
static const char* cuvsGetLastErrorText(void) { return last_error; }
static int cuvsCagraIndexParamsCreate(cuvsCagraIndexParams_t *p)
{ *p = calloc(1, sizeof(**p)); (*p)->graph_build_params = calloc(1, sizeof(GraphParams)); return 0; }
static int cuvsCagraIndexParamsDestroy(cuvsCagraIndexParams_t p) { free(p->graph_build_params); free(p); return 0; }
static int cuvsIvfPqSearchParamsCreate(cuvsIvfPqSearchParams_t *p) { *p = calloc(1, sizeof(**p)); return 0; }
static int cuvsIvfPqSearchParamsDestroy(cuvsIvfPqSearchParams_t p) { free(p); return 0; }
static int cuvsCagraIndexCreate(cuvsCagraIndex_t *p) { *p = (void*)1; return 0; }
static int cuvsCagraIndexDestroy(cuvsCagraIndex_t p) { (void)p; last_error = NULL; return 0; }
static int cuvsCagraBuild(cuvsResources_t r, cuvsCagraIndexParams_t p, DLManagedTensor *t, cuvsCagraIndex_t i)
{ (void)r; (void)i;
  assert(p->build_algo == IVF_PQ || p->build_algo == NN_DESCENT);
  assert(p->graph_degree == 128 && p->intermediate_graph_degree == 256);
  assert(t->dl_tensor.device.device_type == kDLCUDA);
  assert(p->graph_build_params->refinement_rate == 8.0f);
  assert(p->graph_build_params->ivf_pq_search_params->lut_dtype == HIP_R_32F);
  if (fail_build) { last_error = "injected CAGRA build failure"; return 1; }
  saved_n = t->dl_tensor.shape[0]; saved_dim = t->dl_tensor.shape[1]; return 0; }
static int cuvsRMMAlloc(cuvsResources_t r, void **p, size_t n)
{ (void)r; ++allocations; if (allocations == fail_allocation) { last_error = "injected allocation failure"; return 1; }
  *p = malloc(n); ++live; return 0; }
static int cuvsRMMFree(cuvsResources_t r, void *p, size_t n)
{ (void)r; (void)n; free(p); ++frees; --live; return 0; }
static int cudaMemcpy(void *dst, const void *src, size_t n, int kind)
{ (void)kind; if (++copies == fail_copy) return 1; memcpy(dst, src, n); return 0; }
static int cuvsCagraSearchParamsCreate(cuvsCagraSearchParams_t *p) { *p = calloc(1, sizeof(**p)); return 0; }
static int cuvsCagraSearchParamsDestroy(cuvsCagraSearchParams_t p) { free(p); return 0; }
static int cuvsCagraSearch(cuvsResources_t r, cuvsCagraSearchParams_t p, cuvsCagraIndex_t i,
                         DLManagedTensor *q, DLManagedTensor *n, DLManagedTensor *d, cuvsFilter f)
{ (void)r; (void)i; (void)f;
  assert(p->itopk_size >= 512 && p->search_width == 4);
  for (long j = 0; j < q->dl_tensor.shape[0] * n->dl_tensor.shape[1]; ++j) {
    ((unsigned*)n->dl_tensor.data)[j] = (unsigned)(j % n->dl_tensor.shape[1]);
    ((float*)d->dl_tensor.data)[j] = ((float*)q->dl_tensor.data)[0] + (float)j;
  } return 0; }
static int cuvsCagraSerialize(cuvsResources_t r, const char *f, cuvsCagraIndex_t i, bool b)
{ (void)r; (void)f; (void)i; (void)b; return 0; }
static int cuvsCagraDeserialize(cuvsResources_t r, const char *f, cuvsCagraIndex_t i)
{ (void)r; (void)f; (void)i; return 0; }
static int cuvsCagraIndexGetSize(cuvsCagraIndex_t i, int64_t *n) { (void)i; *n = saved_n; return 0; }
static int cuvsCagraIndexGetDims(cuvsCagraIndex_t i, int64_t *n) { (void)i; *n = saved_dim; return 0; }
