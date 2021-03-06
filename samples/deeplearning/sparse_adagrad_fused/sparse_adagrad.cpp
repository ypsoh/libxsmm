/******************************************************************************
* Copyright (c) Intel Corporation - All rights reserved.                      *
* This file is part of the LIBXSMM library.                                   *
*                                                                             *
* For information on the license, see the LICENSE file.                       *
* Further information: https://github.com/hfp/libxsmm/                        *
* SPDX-License-Identifier: BSD-3-Clause                                       *
******************************************************************************/
/* Dhiraj Kalamkar (Intel Corp.)
******************************************************************************/

#include <vector>
#include <time.h>
#include <sys/syscall.h>
#include <algorithm>
#include <parallel/algorithm>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <math.h>
#include <unistd.h>
#include <immintrin.h>
#ifdef _OPENMP
#include <omp.h>
#else
#define omp_get_num_threads() (1)
#define omp_get_thread_num() (0)
#define omp_get_max_threads() (1)
#endif
#include <libxsmm.h>

#ifdef USE_PERF_COUNTERS
#include "counters.h"
#endif

const int alignment = 64;
typedef long ITyp;
typedef float FTyp;

thread_local struct drand48_data rand_buf;

void set_random_seed(int seed)
{
#pragma omp parallel
  {
    int tid = omp_get_thread_num();
    srand48_r(seed+tid, &rand_buf);
  }
}

static double get_time() {
  static bool init_done = false;
  static struct timespec stp = {0,0};
  struct timespec tp;
  clock_gettime(CLOCK_REALTIME, &tp);
  /*clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &tp);*/

  if(!init_done) {
    init_done = true;
    stp = tp;
  }
  double ret = (tp.tv_sec - stp.tv_sec) * 1e3 + (tp.tv_nsec - stp.tv_nsec)*1e-6;
  return ret;
}

template<typename T>
void init_zero(size_t sz, T *buf)
{
#pragma omp parallel for
  for(size_t i = 0; i < sz; i++)
    buf[i] = (T)0;
}

template<typename T>
void init_random(size_t sz, T *buf, T low = -0.1, T high = 0.1)
{
  T range = high - low;
#pragma omp parallel for schedule(static)
  for(size_t i = 0; i < sz; i++) {
    double randval;
    drand48_r(&rand_buf, &randval);
    buf[i] = randval * range - low;
  }
}

double get_checksum(FTyp *buf, size_t sz)
{
  double sum = 0.0;
#pragma omp parallel for reduction(+:sum)
  for(size_t i = 0; i < sz; i++) {
    sum += buf[i];
  }
  return sum;
}

inline void *my_malloc(size_t sz, size_t align)
{
  return libxsmm_aligned_malloc(sz, align);
}

inline void my_free(void *p)
{
    if(!p) return;
    libxsmm_free(p);
}

#define DECL_VLA_PTR(type, name, dims, ptr) type (*name)dims = (type (*)dims)ptr

template <typename T>
class EmbeddingBagImpl
{
public:
  EmbeddingBagImpl(int M, int E) : M(M), E(E)
  {
    weight_ = (T*)my_malloc((size_t)M * E * sizeof(T), alignment);
    h = (T*)my_malloc((size_t)M * sizeof(T), alignment);

#ifdef USE_LIBXSMM_JIT
    _ld = E;
    kernel = libxsmm_dispatch_meltw_reduce_cols_idx(E, &_ld, &_ld, LIBXSMM_DATATYPE_F32, LIBXSMM_DATATYPE_F32, (sizeof(long) == 8) ? LIBXSMM_DATATYPE_I64 : LIBXSMM_DATATYPE_I32);
    kernel1 = libxsmm_dispatch_meltw_reduce(E, 1, &_ld, &_ld, LIBXSMM_DATATYPE_F32, LIBXSMM_DATATYPE_F32, LIBXSMM_MELTW_FLAG_REDUCE_OP_ADD_ROWS_ELTS_SQUARED, 0);
    kernel2 = libxsmm_dispatch_meltw_scale(E, 1, &_ld, &_ld, LIBXSMM_DATATYPE_F32, LIBXSMM_DATATYPE_F32, LIBXSMM_MELTW_FLAG_SCALE_ROWS_BCASTVAL_ACCUMULATE, 0);
#endif
  }

  ~EmbeddingBagImpl()
  {
    my_free(weight_);
    my_free(h);
    weight_ = 0;
    h = 0;
  }

  void init(T low = -0.1, T high = 0.1)
  {
    //init_random(M * E, weight_, low, high);
    init_zero(M * E, weight_);
    init_zero(M, h);
  }

  void fused_backward_update_adagrad(int U, int NS, int N, long *mb_offsets, long *mb_indices, long *wt_indices, T *outGrad_, float lr, float eps)
  {
    DECL_VLA_PTR(T, outGrad, [E], outGrad_);
    DECL_VLA_PTR(T, wt, [E], weight_);

#pragma omp parallel for
    for (int u = 0; u < U; u++) {
      int start = mb_offsets[u];
      int end = mb_offsets[u+1];
      float g_sum[E] = {0};
      float sum = 0.0;

#ifdef USE_LIBXSMM_JIT

     // lookup reduction kernel
      libxsmm_meltw_reduce_cols_idx_param params;
      params.n = end - start;
      params.ind_ptr = &mb_indices[start];
      params.inp_ptr = outGrad;
      params.out_ptr = &g_sum[0];
      kernel( &params );

      // squared + reduction kernel
      libxsmm_meltw_reduce_param    params1;
      params1.in_ptr = g_sum;
      params1.out_ptr_1 = &sum;
      kernel1( &params1 );

      sum /= E;
      int idx = wt_indices[u];
      float hi = h[idx];
      hi += sum;
      h[idx] = hi;
      float scale = lr / (sqrt(hi) + eps);

      // scale and accumulate kernel
      libxsmm_meltw_scale_param params2;
      params2.in_ptr = g_sum;
      params2.out_ptr = &wt[idx][0];
      params2.scale_vals_ptr = &scale;
      kernel2( &params2 );

#else
      for (int l = start; l < end; l++) {
        int idx = mb_indices[l];
        for (int e = 0; e < E; e++) {
          g_sum[e] += outGrad[idx][e];
        }
      }
      for (int e = 0; e < E; e++) {
        sum += g_sum[e] * g_sum[e];
      }
      sum /= E;
      int idx = wt_indices[u];
      float hi = h[idx];
      hi += sum;
      h[idx] = hi;
      float scale = lr / (sqrt(hi) + eps);
      for (int e = 0; e < E; e++) {
        wt[idx][e] += g_sum[e] * scale;
      }
#endif
    }
  }

  T *weight_;
  T *h;
  int M;
  int E;

#ifdef USE_LIBXSMM_JIT
  int _ld;
  libxsmm_meltwfunction_reduce_cols_idx kernel;
  libxsmm_meltwfunction_reduce kernel1;
  libxsmm_meltwfunction_scale kernel2;
#endif
};

typedef EmbeddingBagImpl<FTyp> EmbeddingBag;


struct EmbeddingInOut {
  int N, NS, E, U;
  ITyp *offsets;
  ITyp *indices;
  FTyp *output;
  FTyp *gradout;
  FTyp *grads;
  ITyp * mb_offsets;
  ITyp * mb_indices;
  ITyp * wt_indices;
};

void sparse_transpose(EmbeddingInOut *eio)
{
  int N = eio->N;
  int NS = eio->NS;
  std::vector<std::pair<int, int>> tmpBuf(NS);
  for(int i = 0; i < N; i++) {
    int start = eio->offsets[i];
    int end = eio->offsets[i+1];
    for (int j = start; j < end; j++) {
      tmpBuf[j].first = eio->indices[j];
      tmpBuf[j].second = i;
    }
  }
  //std::sort(tmpBuf.begin(), tmpBuf.end(), [](const std::pair<int, int> &a, const std::pair<int, int> &b) { return a.first < b.first; });
  __gnu_parallel::sort(tmpBuf.begin(), tmpBuf.end(), [](const std::pair<int, int> &a, const std::pair<int, int> &b) { return a.first < b.first; });

  int U = 1;
  for (int i = 1; i < NS; i++) {
    if (tmpBuf[i].first != tmpBuf[i-1].first) U++;
  }
  eio->mb_offsets = (ITyp*)my_malloc((U+1) * sizeof(ITyp), alignment);
  eio->mb_indices = (ITyp*)my_malloc((NS) * sizeof(ITyp), alignment);
  eio->wt_indices = (ITyp*)my_malloc((U) * sizeof(ITyp), alignment);

  eio->mb_offsets[0] = 0;
  eio->mb_indices[0] = tmpBuf[0].second;
  eio->wt_indices[0] = tmpBuf[0].first;
  int u = 0;
  for (int i = 1; i < NS; i++) {
    eio->mb_indices[i] = tmpBuf[i].second;
    if (tmpBuf[i].first != tmpBuf[i-1].first) {
      u++;
      eio->wt_indices[u] = tmpBuf[i].first;
      eio->mb_offsets[u] = i;
    }
  }
  eio->mb_offsets[u+1] = NS;
  eio->U = U;
}

void allocate_buffers_and_generte_rnd_input(int N, int P, EmbeddingBag *eb, EmbeddingInOut *eio)
{
  int E = eb->E;
  int M = eb->M;
  int NS = 0;
  eio->N = N;
  eio->E = E;

  eio->offsets = (ITyp*)my_malloc((N+1) * sizeof(ITyp), alignment);
  eio->output = (FTyp*)my_malloc(N * E * sizeof(FTyp), alignment);
  eio->gradout = (FTyp*)my_malloc(N * E * sizeof(FTyp), alignment);
  init_zero(N * E, eio->output);
  init_random(N * E, eio->gradout, -0.01f, 0.01f);

  eio-> offsets[0] = 0;
  for(int i = 1; i <= N; i++) {
    double randval;
    drand48_r(&rand_buf, &randval);
    int cp = (int)(randval * P);
    if (cp == 0) cp = 1;
    NS += cp;
    eio->offsets[i] = NS;
  }
  eio->NS = NS;
  eio->indices = (ITyp*)my_malloc(NS * sizeof(ITyp), alignment);
  eio->grads = (FTyp*)my_malloc(NS * E * sizeof(FTyp), alignment);
  init_zero(NS * E, eio->grads);
  for (int n = 0; n < N; n++)
  {
    int start = eio->offsets[n];
    int end = eio->offsets[n+1];
    for (int i = start; i < end; i++)
    {
      double randval;
      drand48_r(&rand_buf, &randval);
      ITyp ind = (ITyp)(randval * M);
      if (ind == M)
        ind--;
      eio->indices[i] = ind;
    }
    std::sort(&eio->indices[start], &eio->indices[end]);
  }
  eio->U = -1;
  eio->mb_offsets = NULL;
  eio->mb_indices = NULL;
  eio->wt_indices = NULL;
  auto t0 = get_time();
  sparse_transpose(eio);
  auto t1 = get_time();
  //printf("Trans Time = %.3f ms\n", t1-t0);
}

void free_buffers(EmbeddingInOut *eio)
{
  my_free(eio->grads);
  my_free(eio->indices);
  my_free(eio->gradout);
  my_free(eio->output);
  my_free(eio->offsets);
  my_free(eio->mb_offsets);
  my_free(eio->mb_indices);
  my_free(eio->wt_indices);
}


int iters = 100;
int N = 2048;
int E = 64;
int P = 100;
int M = 1000000;
int S = 8;

#define my_printf(fmt, args...) printf(fmt, args)

int main(int argc, char * argv[]) {
  if(argc > 1 && strncmp(argv[1], "-h", 3) == 0) {
    printf("Usage: %s iters N E M S P\n", argv[0]);
    printf("iters: Number of iterations (= %d)\n", iters);
    printf("N: Minibatch (= %d)\n", N);
    printf("E: embedding row width (= %d)\n", E);
    printf("M: Number of rows per table (= %d)\n", M);
    printf("S: Number of Tables (= %d)\n", S);
    printf("P: Average number of indices per look up (= %d)\n", P);
    exit(0);
  }

  {
    int i = 1;
    if(argc > i) iters = atoi(argv[i++]);
    if(argc > i) N = atoi(argv[i++]);
    if(argc > i) E = atoi(argv[i++]);
    if(argc > i) M = atoi(argv[i++]);
    if(argc > i) S = atoi(argv[i++]);
    if(argc > i) P = atoi(argv[i++]);
  }

  printf("Using: iters: %d N: %d E: %d M: %d S: %d P: %d\n", iters, N, E, M, S, P);

  double checksum = 0.0;

  int LS = S;
  int LN = N;
  set_random_seed(777);

#ifdef USE_PERF_COUNTERS
  ctrs_skx_uc a, b, s;
  bw_gibs bw_min, bw_max, bw_avg;

  setup_skx_uc_ctrs( CTRS_EXP_DRAM_CAS );
  zero_skx_uc_ctrs( &a );
  zero_skx_uc_ctrs( &b );
  zero_skx_uc_ctrs( &s );
#endif

  EmbeddingInOut *eio[iters][LS];
  EmbeddingBag *eb[LS];
  size_t tNS = 0;
  size_t tU = 0;

  for(int i = 0; i < LS; i++)
  {
    eb[i] = new EmbeddingBag(M, E);
    eb[i]->init();
    for(int j = 0; j < iters; j++)
    {
      eio[j][i] = new EmbeddingInOut();
      auto t0 = get_time();
      allocate_buffers_and_generte_rnd_input(N, P, eb[i], eio[j][i]);
      auto t1 = get_time();
      //printf("Rand init time = %.3f ms\n", t1 - t0);
      tNS += eio[j][i]->NS;
      tU += eio[j][i]->U;
    }
  }
  int warmup = (iters > 2 ? 2 : iters);

  for(int i = 0; i < warmup; i++) {
    double t0 = get_time();
    for(int s = 0; s < LS; s++) {
      eb[s]->fused_backward_update_adagrad(eio[i][s]->U, eio[i][s]->NS, N, eio[i][s]->mb_offsets, eio[i][s]->mb_indices, eio[i][s]->wt_indices, eio[i][s]->gradout, -0.1, 1.0e-6);
    }
    double t1 = get_time();
    printf("Warmup Iter %4d: Time = %.3f ms\n", i, t1-t0);
  }

#ifdef USE_PERF_COUNTERS
  read_skx_uc_ctrs( &a );
#endif
  double t0 = get_time();
  double bwdupdTime = 0.0;

  for(int i = 0; i < iters; i++) {
    double t0 = get_time();
    for(int s = 0; s < LS; s++) {
      //printf("Gradout checksum = %g\n", get_checksum(eio[i][s]->gradout, N*E));
      eb[s]->fused_backward_update_adagrad(eio[i][s]->U, eio[i][s]->NS, N, eio[i][s]->mb_offsets, eio[i][s]->mb_indices, eio[i][s]->wt_indices, eio[i][s]->gradout, -0.1, 1.0e-6);
    }
    double t1 = get_time();
    //printf("Iter %4d: Time = %.3f ms\n", i, t1-t0);
    bwdupdTime += t1-t0;
  }
  double t1 = get_time();
#ifdef USE_PERF_COUNTERS
  read_skx_uc_ctrs( &b );
  difa_skx_uc_ctrs( &a, &b, &s );
  divi_skx_uc_ctrs( &s, iters );
#endif
#ifdef VERIFY_CORRECTNESS
  for(int s = 0; s < LS; s++) {
    double psum = get_checksum(eb[s]->weight_, M*E);
    //my_printf("PSUM %d: %g\n", s, psum);
    checksum += psum;
  }
#endif

  //  tU*E wt RW + N*E gO R + N mb_offsets + NS mb_ind + U wt_ind
  size_t bwdupdBytesMinRd = ((size_t)tU*(E+1)) * sizeof(FTyp) + ((size_t)tNS+tU) * sizeof(ITyp) + ((size_t)iters*LS*N*E) * sizeof(FTyp) + ((size_t)iters*LS*N) * sizeof(ITyp);
  size_t bwdupdBytesMaxRd = ((size_t)tU*(E+16)) * sizeof(FTyp) + ((size_t)tNS+tU) * sizeof(ITyp) + ((size_t)iters*LS*N*E) * sizeof(FTyp) + ((size_t)iters*LS*N) * sizeof(ITyp);

  size_t bwdupdBytesMinWr = ((size_t)tU*(E+1)) * sizeof(FTyp);
  size_t bwdupdBytesMaxWr = ((size_t)tU*(E+16)) * sizeof(FTyp);

  size_t bwdupdBytesMin = bwdupdBytesMinRd + bwdupdBytesMinWr;
  size_t bwdupdBytesMax = bwdupdBytesMaxRd + bwdupdBytesMaxWr;

  my_printf("Iters = %d, LS = %d, N = %d, M = %d, E = %d, avgNS = %ld, avgU = %ld, P = %d\n", iters, LS, N, M, E, tNS/(iters*LS), tU/(iters*LS), P);
  //printf("Time: Fwd: %.3f ms Bwd: %.3f ms Upd: %.3f  Total: %.3f\n", fwdTime, bwdTime, updTime, t1-t0);
  my_printf("Per Iter  Time: %.3f ms  Total: %.3f ms\n", bwdupdTime/(iters), (t1-t0)/(iters));
  my_printf("Per Table Time: %.3f ms  Total: %.3f ms\n", bwdupdTime/(iters*LS), (t1-t0)/(iters*LS));

  double scale = 1000.0/(1024.0*1024.0*1024.0);
  my_printf("BW: RD Min: %.3f GB/s   Max: %.3f GB/s \n", bwdupdBytesMinRd*scale/bwdupdTime, bwdupdBytesMaxRd*scale/bwdupdTime);
  my_printf("BW: WR Min: %.3f GB/s   Max: %.3f GB/s \n", bwdupdBytesMinWr*scale/bwdupdTime, bwdupdBytesMaxWr*scale/bwdupdTime);
  my_printf("BW: TT Min: %.3f GB/s   Max: %.3f GB/s \n", bwdupdBytesMin*scale/bwdupdTime, bwdupdBytesMax*scale/bwdupdTime);

#ifdef USE_PERF_COUNTERS
  get_cas_ddr_bw_skx( &s, bwdupdTime/iters/1000.0, &bw_avg );
  printf("Measured AVG GB/s: RD %f   WR %f   TT %f\n", bw_avg.rd, bw_avg.wr, bw_avg.rd + bw_avg.wr);
#endif

#ifdef VERIFY_CORRECTNESS
  printf("Checksum = %g\n", checksum);
#endif

  for(int i = 0; i < LS; i++)
  {
    for(int j = 0; j < iters; j++)
    {
      free_buffers(eio[j][i]);
      delete eio[j][i];
    }
    delete eb[i];
  }
  return 0;
}
