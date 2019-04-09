/******************************************************************************
** Copyright (c) 2016-2019, Intel Corporation                                **
** All rights reserved.                                                      **
**                                                                           **
** Redistribution and use in source and binary forms, with or without        **
** modification, are permitted provided that the following conditions        **
** are met:                                                                  **
** 1. Redistributions of source code must retain the above copyright         **
**    notice, this list of conditions and the following disclaimer.          **
** 2. Redistributions in binary form must reproduce the above copyright      **
**    notice, this list of conditions and the following disclaimer in the    **
**    documentation and/or other materials provided with the distribution.   **
** 3. Neither the name of the copyright holder nor the names of its          **
**    contributors may be used to endorse or promote products derived        **
**    from this software without specific prior written permission.          **
**                                                                           **
** THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS       **
** "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT         **
** LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR     **
** A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT      **
** HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,    **
** SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED  **
** TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR    **
** PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF    **
** LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING      **
** NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS        **
** SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.              **
******************************************************************************/
/* Hans Pabst (Intel Corp.)
******************************************************************************/
#include <libxsmm.h>
#include "libxsmm_gemm.h"
#include "libxsmm_ext.h"

#if defined(LIBXSMM_OFFLOAD_TARGET)
# pragma offload_attribute(push,target(LIBXSMM_OFFLOAD_TARGET))
#endif
#include <string.h>
#if defined(LIBXSMM_OFFLOAD_TARGET)
# pragma offload_attribute(pop)
#endif

#if defined(LIBXSMM_GEMM_MMBATCH) && defined(LIBXSMM_BUILD_EXT)
# include "libxsmm_trace.h"
#endif

#if defined(LIBXSMM_GEMM_MMBATCH) && defined(LIBXSMM_BUILD_EXT)
# if !defined(LIBXSMM_GEMM_EXT_MMBATCH_PREFETCH)
#   define LIBXSMM_GEMM_EXT_MMBATCH_PREFETCH libxsmm_get_gemm_prefetch(LIBXSMM_PREFETCH_AUTO)
# endif
# if !defined(LIBXSMM_GEMM_EXT_MMBATCH_MAXDEPTH)
#   define LIBXSMM_GEMM_EXT_MMBATCH_MAXDEPTH 8/*POT*/
# endif
LIBXSMM_APIVAR_ARRAY(libxsmm_gemm_descriptor internal_ext_gemm_batchdesc, LIBXSMM_GEMM_EXT_MMBATCH_MAXDEPTH);
LIBXSMM_APIVAR(unsigned int internal_ext_gemm_batchdepth);
LIBXSMM_APIVAR(unsigned int internal_ext_gemm_batchsize);
#endif


#if defined(LIBXSMM_GEMM_MMBATCH) && defined(LIBXSMM_BUILD_EXT)
LIBXSMM_API_INLINE int internal_mmbatch_sortrev(const void* stat_a, const void* stat_b)
{
  const libxsmm_mmbatch_item *const a = (const libxsmm_mmbatch_item*)stat_a;
  const libxsmm_mmbatch_item *const b = (const libxsmm_mmbatch_item*)stat_b;
  LIBXSMM_ASSERT(NULL != stat_a && NULL != stat_b);
  return a->stat.count < b->stat.count ? 1 : (b->stat.count < a->stat.count ? -1 : 0);
}
#endif /*defined(LIBXSMM_GEMM_MMBATCH) && defined(LIBXSMM_BUILD_EXT)*/


LIBXSMM_API_INLINE int internal_mmbatch_flush(const libxsmm_gemm_descriptor* batchdesc,
  libxsmm_blasint batchsize, libxsmm_mmbatch_item* batcharray)
{
  int result = EXIT_SUCCESS;
#if defined(LIBXSMM_GEMM_MMBATCH) && defined(LIBXSMM_BUILD_EXT)
  if (0 != batchsize) { /* recorded/lazy multiplications */
    const libxsmm_blasint itemsize = sizeof(libxsmm_mmbatch_item);
    LIBXSMM_ASSERT(NULL != batchdesc);
    if (0 == (LIBXSMM_MMBATCH_FLAG_STATISTIC & batchdesc->flags)) { /* process batch */
      const libxsmm_xmmfunction kernel = libxsmm_xmmdispatch(batchdesc);
      if (NULL != kernel.xmm) {
#if defined(_OPENMP)
        if (0 == (LIBXSMM_MMBATCH_FLAG_SEQUENTIAL & batchdesc->flags)) { /* parallelized */
# if defined(LIBXSMM_EXT_TASKS)
          if (0 == omp_get_active_level()) {
            const int nthreads = omp_get_max_threads();
            if (0 >= libxsmm_gemm_taskscale)
# else
          if (0 == omp_in_parallel()) {
            const int nthreads = omp_get_max_threads();
# endif
            { /* classic internal parallelization */
#             pragma omp parallel num_threads(nthreads)
              /*check*/libxsmm_mmbatch_kernel(
                kernel, 0/*index_base*/, 0/*index_stride*/, &itemsize, &itemsize, &itemsize,
                &batcharray->value.a, &batcharray->value.b, &batcharray->value.c,
                0 == (LIBXSMM_MMBATCH_FLAG_SYNCHRONIZED & batchdesc->flags) ? batchsize : -batchsize,
                omp_get_thread_num(), nthreads, batchdesc);
            }
# if defined(LIBXSMM_EXT_TASKS)
            else { /* internal parallelization with tasks */
              const int ntasks = nthreads * libxsmm_gemm_taskscale;
#             pragma omp parallel num_threads(nthreads)
              { /* first thread discovering work will launch all tasks */
#               pragma omp single nowait /* anyone is good */
                { int tid;
                  for (tid = 0; tid < ntasks; ++tid) {
#                   pragma omp task untied
                    /*check*/libxsmm_mmbatch_kernel(
                      kernel, 0/*index_base*/, 0/*index_stride*/, &itemsize, &itemsize, &itemsize,
                      &batcharray->value.a, &batcharray->value.b, &batcharray->value.c,
                      0 == (LIBXSMM_MMBATCH_FLAG_SYNCHRONIZED & batchdesc->flags) ? batchsize : -batchsize,
                      tid, ntasks, batchdesc);
                  }
                }
              } /* implicit synchronization (barrier) */
            }
# endif
          }
          else { /* assume external parallelization */
# if defined(LIBXSMM_EXT_TASKS)
            const int ntasks = omp_get_num_threads() * (0 == libxsmm_gemm_taskscale ? (LIBXSMM_GEMM_TASKSCALE) : libxsmm_gemm_taskscale);
            int tid;
            for (tid = 0; tid < ntasks; ++tid) {
#             pragma omp task untied
              /*check*/libxsmm_mmbatch_kernel(
                kernel, 0/*index_base*/, 0/*index_stride*/, &itemsize, &itemsize, &itemsize,
                &batcharray->value.a, &batcharray->value.b, &batcharray->value.c,
                0 == (LIBXSMM_MMBATCH_FLAG_SYNCHRONIZED & batchdesc->flags) ? batchsize : -batchsize,
                tid, ntasks, batchdesc);
            }
            if (0 == libxsmm_nosync) { /* allow to omit synchronization */
#             pragma omp taskwait
            }
# else
            /*check*/libxsmm_mmbatch_kernel(
              kernel, 0/*index_base*/, 0/*index_stride*/, &itemsize, &itemsize, &itemsize,
              &batcharray->value.a, &batcharray->value.b, &batcharray->value.c,
              0 == (LIBXSMM_MMBATCH_FLAG_SYNCHRONIZED & batchdesc->flags) ? batchsize : -batchsize,
              0/*tid*/, 1/*nthreads*/, batchdesc);
# endif
          }
        }
        else
#endif
        { /* sequential */
          result = libxsmm_mmbatch_kernel(
            kernel, 0/*index_base*/, 0/*index_stride*/, &itemsize, &itemsize, &itemsize,
            &batcharray->value.a, &batcharray->value.b, &batcharray->value.c, batchsize,
            0/*tid*/, 1/*nthreads*/, batchdesc);
        }
      }
      else { /* no fall-back */
        /* several reasons to arrive here: try-lock, unsuitable SMM, etc. */
        result = EXIT_FAILURE;
      }
      memset(batcharray, 0, (size_t)batchsize * (size_t)itemsize); /* clear */
    }
    else { /* print statistic */
      const libxsmm_blasint limit = (LIBXSMM_GEMM_MMBATCH_VERBOSITY < libxsmm_verbosity ? batchsize/*unlimited*/ : 7/*limited*/);
      unsigned int threshold, batchcount;
      libxsmm_blasint count = 0, i;
      LIBXSMM_ASSERT(NULL != batcharray);
      qsort(batcharray, (size_t)batchsize, (size_t)itemsize, internal_mmbatch_sortrev);
      batchcount = batcharray[0].stat.count;
      threshold = ((LIBXSMM_GEMM_MMBATCH_VERBOSITY < libxsmm_verbosity || 3 >= batchsize) ? 0 : (batchcount / 2));
      for (i = 1; i < batchsize; ++i) batchcount += batcharray[i].stat.count;
      LIBXSMM_STDIO_ACQUIRE();
      for (i = 0; i < batchsize; ++i) {
        const libxsmm_gemm_descriptor descriptor = batcharray[i].stat.desc;
        const libxsmm_blasint lda = descriptor.lda, ldb = descriptor.ldb, ldc = descriptor.ldc;
        const libxsmm_blasint m = descriptor.m, n = descriptor.n, k = descriptor.k;
        const char *const symbol = batcharray[i].stat.symbol;
        const unsigned int ci = batcharray[i].stat.count;
        memset(&batcharray[i], 0, (size_t)itemsize); /* clear */
        if (threshold < ci && count < limit /* limit printed statistic */
          && 0 < m && 0 < n && 0 < k)
        {
          const unsigned int ciperc = (unsigned int)(100.0 * ci / batchcount + 0.5);
          if (0 != ciperc) {
            LIBXSMM_ASSERT(0 != ci);
            if (0 == count) {
              fprintf(stderr, "\nLIBXSMM STATISTIC: %u multiplication%c\n", batchcount, 1 < batchcount ? 's' : ' ');
            }
            LIBXSMM_GEMM_PRINT2(stderr,
              LIBXSMM_GETENUM_INP(descriptor.datatype), LIBXSMM_GETENUM_OUT(descriptor.datatype), descriptor.flags, m, n, k,
            /*0 != (LIBXSMM_GEMM_FLAG_ALPHA_0 & descriptor.flags) ? 0 : */1, NULL/*a*/, lda, NULL/*b*/, ldb,
              0 != (LIBXSMM_GEMM_FLAG_BETA_0  & descriptor.flags) ? 0 : 1, NULL/*c*/, ldc);
            if (NULL != symbol && 0 != *symbol) {
              fprintf(stderr, ": %u%% [%s]\n", ciperc, symbol);
            }
            else {
              fprintf(stderr, ": %u%%\n", ciperc);
            }
            ++count;
          }
          else break;
        }
      }
      LIBXSMM_STDIO_RELEASE();
    }
  }
#else
  LIBXSMM_UNUSED(batchdesc); LIBXSMM_UNUSED(batchsize); LIBXSMM_UNUSED(batcharray);
#endif
  return result;
}


#if defined(LIBXSMM_BUILD) && defined(LIBXSMM_BUILD_EXT)

#if defined(LIBXSMM_BLAS_WRAP_DYNAMIC)
LIBXSMM_API_EXPORT libxsmm_dgemm_function libxsmm_original_dgemm(void)
{
  LIBXSMM_BLAS_WRAPPER(double, gemm, libxsmm_original_dgemm_function, libxsmm_original_dgemm/*self*/);
  LIBXSMM_ASSERT(NULL != libxsmm_original_dgemm_function);
  return libxsmm_original_dgemm_function;
}

LIBXSMM_API_EXPORT libxsmm_sgemm_function libxsmm_original_sgemm(void)
{
  LIBXSMM_BLAS_WRAPPER(float, gemm, libxsmm_original_sgemm_function, libxsmm_original_sgemm/*self*/);
  LIBXSMM_ASSERT(NULL != libxsmm_original_sgemm_function);
  return libxsmm_original_sgemm_function;
}
#endif /*defined(LIBXSMM_BLAS_WRAP_DYNAMIC)*/


LIBXSMM_APIEXT LIBXSMM_ATTRIBUTE_USED void LIBXSMM_FSYMBOL(__wrap_dgemm)(
  const char* transa, const char* transb,
  const libxsmm_blasint* m, const libxsmm_blasint* n, const libxsmm_blasint* k,
  const double* alpha, const double* a, const libxsmm_blasint* lda,
  const double* b, const libxsmm_blasint* ldb,
  const double* beta, double* c, const libxsmm_blasint* ldc)
{
  LIBXSMM_ASSERT(NULL != lda && NULL != ldb && NULL != ldc && NULL != m && NULL != n && NULL != k);
  LIBXSMM_ASSERT(NULL != transa && NULL != transb && NULL != alpha && NULL != beta);
  {
#if defined(LIBXSMM_GEMM_MMBATCH) && defined(LIBXSMM_BUILD_EXT)
    unsigned int i = 0; /* no flush */
    int flags = -1;
# if !defined(NDEBUG)
    static int error_once = 0;
    int result = EXIT_SUCCESS;
# endif
    LIBXSMM_INIT
    if (NULL == libxsmm_mmbatch_array
      || LIBXSMM_GEMM_PRECISION_F64 != libxsmm_mmbatch_desc.datatype
      || ((unsigned int)*lda) != libxsmm_mmbatch_desc.lda
      || ((unsigned int)*ldb) != libxsmm_mmbatch_desc.ldb
      || ((unsigned int)*ldc) != libxsmm_mmbatch_desc.ldc
      || ((unsigned int)*m) != libxsmm_mmbatch_desc.m
      || ((unsigned int)*n) != libxsmm_mmbatch_desc.n
      || ((unsigned int)*k) != libxsmm_mmbatch_desc.k
      || (flags = LIBXSMM_GEMM_FLAGS(*transa, *transb)) != (LIBXSMM_GEMM_FLAG_TRANS_AB & libxsmm_mmbatch_desc.flags)
      || LIBXSMM_NEQ(/*0 != (LIBXSMM_GEMM_FLAG_ALPHA_0 & libxsmm_mmbatch_desc.flags) ? 0 : */1, *alpha)
      || LIBXSMM_NEQ(0 != (LIBXSMM_GEMM_FLAG_BETA_0 & libxsmm_mmbatch_desc.flags) ? 0 : 1, *beta))
#endif
    {
#if defined(_DEBUG)
      const char *const env_check = getenv("LIBXSMM_GEMM_CHECK");
      const double check = LIBXSMM_ABS(NULL == env_check ? 0 : atof(env_check));
      void* d = NULL;
      if (LIBXSMM_NEQ(0, check)) {
        const size_t size = (size_t)(*ldc) * (size_t)(*n) * sizeof(double);
        d = libxsmm_scratch_malloc(size, 0/*auto*/, LIBXSMM_MALLOC_SCRATCH_INTERNAL);
        if (NULL != d && LIBXSMM_NEQ(0, *beta)) memcpy(d, c, size); /* copy destination */
      }
#endif
      if (0 == (libxsmm_gemm_wrap % 2) || 0 > libxsmm_gemm_wrap) { /* parallelized/tiled */
        libxsmm_dgemm_omp(transa, transb, m, n, k, alpha, a, lda, b, ldb, beta, c, ldc);
      }
      else { /* small problem size */
        libxsmm_dgemm(transa, transb, m, n, k, alpha, a, lda, b, ldb, beta, c, ldc);
      }
#if defined(_DEBUG)
      if (NULL != d) {
        libxsmm_matdiff_info diff;
        libxsmm_blas_dgemm(transa, transb, m, n, k, alpha, a, lda, b, ldb, beta, d, ldc);
        if (EXIT_SUCCESS == libxsmm_matdiff(&diff, LIBXSMM_DATATYPE_F64, *m, *n, d, c, ldc, ldc)
          && check < 100.0 * diff.normf_rel)
        {
          LIBXSMM_STDIO_ACQUIRE();
          fprintf(stderr, "LIBXSMM: ");
          libxsmm_gemm_print(stderr, LIBXSMM_GEMM_PRECISION_F64, transa, transb,
            m, n, k, alpha, a, lda, b, ldb, beta, c, ldc);
          fprintf(stderr, " => %f%% ERROR\n", 100.0 * diff.normf_rel);
          LIBXSMM_STDIO_RELEASE();
        }
        libxsmm_free(d);
      }
#endif
#if defined(LIBXSMM_GEMM_MMBATCH) && defined(LIBXSMM_BUILD_EXT)
      if (0 != (LIBXSMM_MMBATCH_FLAG_STATISTIC & libxsmm_mmbatch_desc.flags)) {
        libxsmm_descriptor_blob blob;
        const libxsmm_gemm_descriptor *const descriptor = libxsmm_dgemm_descriptor_init(&blob,
          *m, *n, *k, *lda, *ldb, *ldc, *alpha, *beta, LIBXSMM_GEMM_FLAGS(*transa, *transb),
          LIBXSMM_GEMM_EXT_MMBATCH_PREFETCH);

        LIBXSMM_ASSERT(0 != libxsmm_mmbatch_size);
        if (NULL != descriptor) {
          const unsigned int max_batchsize = (unsigned int)((LIBXSMM_GEMM_MMBATCH_SCALE) * libxsmm_mmbatch_size);
          const unsigned int batchsize = LIBXSMM_ATOMIC_LOAD(&internal_ext_gemm_batchsize, LIBXSMM_ATOMIC_RELAXED);
          const unsigned int max_size = (0 != batchsize ? (((batchsize - 1) % max_batchsize) + 1) : 0);
          libxsmm_mmbatch_item *const batcharray = (libxsmm_mmbatch_item*)libxsmm_mmbatch_array;
          libxsmm_mmbatch_item* batcharray_cur = batcharray;
          unsigned int size = max_size;
          if (libxsmm_mmbatch_size < max_size) {
            size = max_size - libxsmm_mmbatch_size;
            batcharray_cur += libxsmm_mmbatch_size;
          }
          i = libxsmm_diff_n(descriptor, batcharray_cur, sizeof(libxsmm_gemm_descriptor),
            sizeof(libxsmm_mmbatch_item)/*stride*/, 0/*hint*/, size);

          if (i < size) { /* update existing entry */
            LIBXSMM_ATOMIC_ADD_FETCH(&batcharray_cur[i].stat.count, 1, LIBXSMM_ATOMIC_RELAXED);
          }
          else { /* new entry needed */
            const int all = -1, shift = 0;
            void* extra = 0;
            i = ((LIBXSMM_ATOMIC_ADD_FETCH(&internal_ext_gemm_batchsize, 1, LIBXSMM_ATOMIC_RELAXED) - 1) % max_batchsize) + 1;
            batcharray[i-1].stat.desc = *descriptor;
            batcharray[i-1].stat.count = 1;
            batcharray[i-1].stat.symbol = libxsmm_trace_info(NULL/*depth*/, NULL/*tid*/, &all, LIBXSMM_CALLER, &shift, &all);
            if (EXIT_SUCCESS == libxsmm_get_malloc_xinfo(libxsmm_mmbatch_array, NULL/*size*/, NULL/*flags*/, &extra)) {
              *(libxsmm_mmbatch_flush_function*)extra = libxsmm_mmbatch_end;
            }
# if !defined(NDEBUG)
            else {
              result = EXIT_FAILURE;
            }
# endif
          }
        }
      }
#endif
    }
#if defined(LIBXSMM_GEMM_MMBATCH) && defined(LIBXSMM_BUILD_EXT)
    else {
      libxsmm_mmbatch_item *const batcharray = (libxsmm_mmbatch_item*)libxsmm_mmbatch_array;
      const unsigned int max_batchsize = (unsigned int)((LIBXSMM_GEMM_MMBATCH_SCALE) * libxsmm_mmbatch_size);
      i = ((LIBXSMM_ATOMIC_ADD_FETCH(&internal_ext_gemm_batchsize, 1, LIBXSMM_ATOMIC_RELAXED) - 1) % max_batchsize) + 1;
      batcharray[i-1].value.a = a;
      batcharray[i-1].value.b = b;
      batcharray[i-1].value.c = c;
      LIBXSMM_ASSERT(0 <= flags);
    }
    if (libxsmm_mmbatch_size == (i - 1)) { /* condition ensure to flush once (first discovery) */
# if !defined(NDEBUG)
      result =
# endif
      internal_mmbatch_flush(&libxsmm_mmbatch_desc, libxsmm_mmbatch_size, (libxsmm_mmbatch_item*)libxsmm_mmbatch_array);
    }
# if !defined(NDEBUG) /* library code is expected to be mute */
    if (EXIT_SUCCESS != result && 0 != libxsmm_verbosity &&
      1 == LIBXSMM_ATOMIC_ADD_FETCH(&error_once, 1, LIBXSMM_ATOMIC_RELAXED))
    {
      fprintf(stderr, "LIBXSMM ERROR: DGEMM batch recording failed!\n");
    }
# endif
#endif
  }
}


LIBXSMM_APIEXT LIBXSMM_ATTRIBUTE_USED void LIBXSMM_FSYMBOL(__wrap_sgemm)(
  const char* transa, const char* transb,
  const libxsmm_blasint* m, const libxsmm_blasint* n, const libxsmm_blasint* k,
  const float* alpha, const float* a, const libxsmm_blasint* lda,
  const float* b, const libxsmm_blasint* ldb,
  const float* beta, float* c, const libxsmm_blasint* ldc)
{
  LIBXSMM_ASSERT(NULL != lda && NULL != ldb && NULL != ldc && NULL != m && NULL != n && NULL != k);
  LIBXSMM_ASSERT(NULL != transa && NULL != transb && NULL != alpha && NULL != beta);
  {
#if defined(LIBXSMM_GEMM_MMBATCH) && defined(LIBXSMM_BUILD_EXT)
    unsigned int i = 0; /* no flush */
    int flags = -1;
# if !defined(NDEBUG)
    static int error_once = 0;
    int result = EXIT_SUCCESS;
# endif
    LIBXSMM_INIT
    if (NULL == libxsmm_mmbatch_array
      || LIBXSMM_GEMM_PRECISION_F32 != libxsmm_mmbatch_desc.datatype
      || ((unsigned int)*lda) != libxsmm_mmbatch_desc.lda
      || ((unsigned int)*ldb) != libxsmm_mmbatch_desc.ldb
      || ((unsigned int)*ldc) != libxsmm_mmbatch_desc.ldc
      || ((unsigned int)*m) != libxsmm_mmbatch_desc.m
      || ((unsigned int)*n) != libxsmm_mmbatch_desc.n
      || ((unsigned int)*k) != libxsmm_mmbatch_desc.k
      || (flags = LIBXSMM_GEMM_FLAGS(*transa, *transb)) != (LIBXSMM_GEMM_FLAG_TRANS_AB & libxsmm_mmbatch_desc.flags)
      || LIBXSMM_NEQ(/*0 != (LIBXSMM_GEMM_FLAG_ALPHA_0 & libxsmm_mmbatch_desc.flags) ? 0 : */1, *alpha)
      || LIBXSMM_NEQ(0 != (LIBXSMM_GEMM_FLAG_BETA_0 & libxsmm_mmbatch_desc.flags) ? 0 : 1, *beta))
#endif
    {
#if defined(_DEBUG)
      const char *const env_check = getenv("LIBXSMM_GEMM_CHECK");
      const double check = LIBXSMM_ABS(NULL == env_check ? 0 : atof(env_check));
      void* d = NULL;
      if (LIBXSMM_NEQ(0, check)) {
        const size_t size = (size_t)(*ldc) * (size_t)(*n) * sizeof(float);
        d = libxsmm_scratch_malloc(size, 0/*auto*/, LIBXSMM_MALLOC_SCRATCH_INTERNAL);
        if (NULL != d && LIBXSMM_NEQ(0, *beta)) memcpy(d, c, size); /* copy destination */
      }
#endif
      if (0 == (libxsmm_gemm_wrap % 2) || 0 > libxsmm_gemm_wrap) { /* parallelized/tiled */
        libxsmm_sgemm_omp(transa, transb, m, n, k, alpha, a, lda, b, ldb, beta, c, ldc);
      }
      else { /* small problem size */
        libxsmm_sgemm(transa, transb, m, n, k, alpha, a, lda, b, ldb, beta, c, ldc);
      }
#if defined(_DEBUG)
      if (NULL != d) {
        libxsmm_matdiff_info diff;
        libxsmm_blas_sgemm(transa, transb, m, n, k, alpha, a, lda, b, ldb, beta, d, ldc);
        if (EXIT_SUCCESS == libxsmm_matdiff(&diff, LIBXSMM_DATATYPE_F32, *m, *n, d, c, ldc, ldc)
          && check < 100.0 * diff.normf_rel)
        {
          LIBXSMM_STDIO_ACQUIRE();
          fprintf(stderr, "LIBXSMM: ");
          libxsmm_gemm_print(stderr, LIBXSMM_GEMM_PRECISION_F32, transa, transb,
            m, n, k, alpha, a, lda, b, ldb, beta, c, ldc);
          fprintf(stderr, " => %f%% ERROR\n", 100.0 * diff.normf_rel);
          LIBXSMM_STDIO_RELEASE();
        }
        libxsmm_free(d);
      }
#endif
#if defined(LIBXSMM_GEMM_MMBATCH) && defined(LIBXSMM_BUILD_EXT)
      if (0 != (LIBXSMM_MMBATCH_FLAG_STATISTIC & libxsmm_mmbatch_desc.flags)) {
        libxsmm_descriptor_blob blob;
        const libxsmm_gemm_descriptor *const descriptor = libxsmm_sgemm_descriptor_init(&blob,
          *m, *n, *k, *lda, *ldb, *ldc, *alpha, *beta, LIBXSMM_GEMM_FLAGS(*transa, *transb),
          LIBXSMM_GEMM_EXT_MMBATCH_PREFETCH);

        LIBXSMM_ASSERT(0 != libxsmm_mmbatch_size);
        if (NULL != descriptor) {
          const unsigned int max_batchsize = (unsigned int)((LIBXSMM_GEMM_MMBATCH_SCALE) * libxsmm_mmbatch_size);
          const unsigned int batchsize = LIBXSMM_ATOMIC_LOAD(&internal_ext_gemm_batchsize, LIBXSMM_ATOMIC_RELAXED);
          const unsigned int max_size = (0 != batchsize ? (((batchsize - 1) % max_batchsize) + 1) : 0);
          libxsmm_mmbatch_item *const batcharray = (libxsmm_mmbatch_item*)libxsmm_mmbatch_array;
          libxsmm_mmbatch_item* batcharray_cur = batcharray;
          unsigned int size = max_size;
          if (libxsmm_mmbatch_size < max_size) {
            size = max_size - libxsmm_mmbatch_size;
            batcharray_cur += libxsmm_mmbatch_size;
          }
          i = libxsmm_diff_n(descriptor, batcharray_cur, sizeof(libxsmm_gemm_descriptor),
            sizeof(libxsmm_mmbatch_item)/*stride*/, 0/*hint*/, size);

          if (i < size) { /* update existing entry */
            LIBXSMM_ATOMIC_ADD_FETCH(&batcharray_cur[i].stat.count, 1, LIBXSMM_ATOMIC_RELAXED);
          }
          else { /* new entry needed */
            const int all = -1, shift = 0;
            void* extra = 0;
            i = ((LIBXSMM_ATOMIC_ADD_FETCH(&internal_ext_gemm_batchsize, 1, LIBXSMM_ATOMIC_RELAXED) - 1) % max_batchsize) + 1;
            batcharray[i-1].stat.desc = *descriptor;
            batcharray[i-1].stat.count = 1;
            batcharray[i-1].stat.symbol = libxsmm_trace_info(NULL/*depth*/, NULL/*tid*/, &all, LIBXSMM_CALLER, &shift, &all);
            if (EXIT_SUCCESS == libxsmm_get_malloc_xinfo(libxsmm_mmbatch_array, NULL/*size*/, NULL/*flags*/, &extra)) {
              *(libxsmm_mmbatch_flush_function*)extra = libxsmm_mmbatch_end;
            }
# if !defined(NDEBUG)
            else {
              result = EXIT_FAILURE;
            }
# endif
          }
        }
      }
#endif
    }
#if defined(LIBXSMM_GEMM_MMBATCH) && defined(LIBXSMM_BUILD_EXT)
    else {
      libxsmm_mmbatch_item *const batcharray = (libxsmm_mmbatch_item*)libxsmm_mmbatch_array;
      const unsigned int max_batchsize = (unsigned int)((LIBXSMM_GEMM_MMBATCH_SCALE) * libxsmm_mmbatch_size);
      i = ((LIBXSMM_ATOMIC_ADD_FETCH(&internal_ext_gemm_batchsize, 1, LIBXSMM_ATOMIC_RELAXED) - 1) % max_batchsize) + 1;
      batcharray[i-1].value.a = a;
      batcharray[i-1].value.b = b;
      batcharray[i-1].value.c = c;
      LIBXSMM_ASSERT(0 <= flags);
    }
    if (libxsmm_mmbatch_size == (i - 1)) { /* condition ensure to flush once (first discovery) */
# if !defined(NDEBUG)
      result =
# endif
      internal_mmbatch_flush(&libxsmm_mmbatch_desc, libxsmm_mmbatch_size, (libxsmm_mmbatch_item*)libxsmm_mmbatch_array);
    }
# if !defined(NDEBUG) /* library code is expected to be mute */
    if (EXIT_SUCCESS != result && 0 != libxsmm_verbosity &&
      1 == LIBXSMM_ATOMIC_ADD_FETCH(&error_once, 1, LIBXSMM_ATOMIC_RELAXED))
    {
      fprintf(stderr, "LIBXSMM ERROR: SGEMM batch recording failed!\n");
    }
# endif
#endif
  }
}

#endif /*defined(LIBXSMM_BUILD) && defined(LIBXSMM_BUILD_EXT)*/


LIBXSMM_APIEXT void libxsmm_xgemm_omp(libxsmm_gemm_precision iprec, libxsmm_gemm_precision oprec,
  const char* transa, const char* transb, const libxsmm_blasint* m, const libxsmm_blasint* n, const libxsmm_blasint* k,
  const void* alpha, const void* a, const libxsmm_blasint* lda, const void* b, const libxsmm_blasint* ldb,
  const void* beta, void* c, const libxsmm_blasint* ldc)
{
  libxsmm_gemm_blob blob;
#if defined(LIBXSMM_EXT_TASKS) /* implies _OPENMP */
  const int omp_external = omp_get_active_level(), nthreads = (0 == omp_external ? omp_get_max_threads() : omp_get_num_threads());
#elif defined(_OPENMP)
  const int omp_external = omp_in_parallel(), nthreads = (0 == omp_external ? omp_get_max_threads() : 1);
#else
  const int nthreads = 1;
#endif
  const libxsmm_gemm_handle *const handle = libxsmm_gemm_handle_init(&blob, iprec, oprec, transa, transb,
    m, n, k, lda, ldb, ldc, alpha, beta, LIBXSMM_GEMM_HANDLE_FLAG_AUTO, nthreads);
  const size_t scratch_size = libxsmm_gemm_handle_get_scratch_size(handle);
  void* scratch = NULL;
  if (NULL != handle && (0 == scratch_size ||
      NULL != (scratch = libxsmm_scratch_malloc(scratch_size, LIBXSMM_CACHELINE, LIBXSMM_MALLOC_SCRATCH_INTERNAL))))
  {
#if defined(_OPENMP)
    if (0 == omp_external) { /* enable internal parallelization */
# if defined(LIBXSMM_EXT_TASKS)
      if (0 >= libxsmm_gemm_taskscale)
# endif
      {
#       pragma omp parallel num_threads(nthreads)
        libxsmm_gemm_thread(handle, scratch, a, b, c, omp_get_thread_num());
      }
# if defined(LIBXSMM_EXT_TASKS)
      else { /* tasks requested */
        const int ntasks = nthreads * libxsmm_gemm_taskscale;
#       pragma omp parallel num_threads(nthreads)
        { /* first thread discovering work will launch all tasks */
#         pragma omp single nowait /* anyone is good */
          { int tid;
            for (tid = 0; tid < ntasks; ++tid) {
#             pragma omp task untied
              libxsmm_gemm_thread(handle, scratch, a, b, c, tid);
            }
          }
        } /* implicit synchronization (barrier) */
      }
# endif
    }
    else { /* assume external parallelization */
# if defined(LIBXSMM_EXT_TASKS) /* implies _OPENMP */
      const int ntasks = (0 == libxsmm_gemm_taskscale
        ? (LIBXSMM_GEMM_TASKSCALE)
        : libxsmm_gemm_taskscale) * nthreads;
      int tid;
      for (tid = 0; tid < ntasks; ++tid) {
#       pragma omp task untied
        libxsmm_gemm_thread(handle, scratch, a, b, c, tid);
      }
      if (0 == libxsmm_nosync) { /* allow to omit synchronization */
#       pragma omp taskwait
      }
# else
      LIBXSMM_ASSERT(1 == handle->nthreads);
      libxsmm_gemm_thread(handle, scratch, a, b, c, 0/*tid*/);
# endif
    }
    if (LIBXSMM_VERBOSITY_HIGH <= libxsmm_verbosity || 0 > libxsmm_verbosity) { /* library code is expected to be mute */
      const unsigned int ntasks = handle->mt * handle->nt * handle->kt;
      const double imbalance = 100.0 * (handle->nthreads - ntasks) / handle->nthreads;
      static double max_imbalance = 50.0;
      if (max_imbalance < imbalance) {
        fprintf(stderr, "LIBXSMM WARNING (XGEMM): %.0f%% imbalance (%u of %u workers utilized)!\n",
          imbalance, ntasks, handle->nthreads);
        max_imbalance = imbalance;
      }
    }
#else
    LIBXSMM_ASSERT(1 == handle->nthreads);
    libxsmm_gemm_thread(handle, scratch, a, b, c, 0/*tid*/);
#endif /*defined(_OPENMP)*/
    libxsmm_free(scratch);
  }
  else { /* fall-back or error */
    static int error_once = 0;
    if (NULL == handle) { /* fall-back */
      if ((LIBXSMM_VERBOSITY_HIGH <= libxsmm_verbosity || 0 > libxsmm_verbosity) /* library code is expected to be mute */
        && 1 == LIBXSMM_ATOMIC_ADD_FETCH(&error_once, 1, LIBXSMM_ATOMIC_RELAXED))
      {
        fprintf(stderr, "LIBXSMM WARNING (XGEMM): fall-back code path triggered!\n");
      }
    }
    else if (0 != libxsmm_verbosity && /* library code is expected to be mute */
      1 == LIBXSMM_ATOMIC_ADD_FETCH(&error_once, 1, LIBXSMM_ATOMIC_RELAXED))
    {
      fprintf(stderr, "LIBXSMM ERROR: failed to allocate GEMM-scratch memory!\n");
    }
    libxsmm_blas_xgemm(iprec, oprec, transa, transb, m, n, k, alpha, a, lda, b, ldb, beta, c, ldc);
  }
}


LIBXSMM_APIEXT void libxsmm_gemm_batch_omp(libxsmm_gemm_precision iprec, libxsmm_gemm_precision oprec,
  const char* transa, const char* transb, libxsmm_blasint m, libxsmm_blasint n, libxsmm_blasint k,
  const void* alpha, const void* a, const libxsmm_blasint* lda, const void* b, const libxsmm_blasint* ldb,
  const void* beta, void* c, const libxsmm_blasint* ldc, libxsmm_blasint index_base, libxsmm_blasint index_stride,
  const libxsmm_blasint stride_a[], const libxsmm_blasint stride_b[], const libxsmm_blasint stride_c[],
  libxsmm_blasint batchsize)
{
  static int error_once = 0;
#if defined(LIBXSMM_GEMM_CHECK)
  if (NULL != a && NULL != b && NULL != c)
#endif
  {
    int result;
    if (LIBXSMM_SMM_AI(m, n, k, 2/*RFO*/, libxsmm_typesize((libxsmm_datatype)oprec))) { /* check if an SMM is suitable */
      libxsmm_xmmfunction kernel;
      libxsmm_descriptor_blob blob;
      const int gemm_flags = LIBXSMM_GEMM_PFLAGS(transa, transb, LIBXSMM_FLAGS);
      libxsmm_gemm_descriptor *const desc = libxsmm_gemm_descriptor_init2(&blob, iprec, oprec, m, n, k,
        NULL != lda ? *lda : (0 == (LIBXSMM_GEMM_FLAG_TRANS_A & gemm_flags) ? m : k),
        NULL != ldb ? *ldb : (0 == (LIBXSMM_GEMM_FLAG_TRANS_B & gemm_flags) ? k : n),
        NULL != ldc ? *ldc : m, alpha, beta, gemm_flags, libxsmm_get_gemm_prefetch(LIBXSMM_PREFETCH_AUTO));
#if defined(_OPENMP)
      const int nchunks = (int)((LIBXSMM_ABS(batchsize) + libxsmm_gemm_taskgrain - 1) / libxsmm_gemm_taskgrain);
      result = EXIT_SUCCESS;
      if (1 < nchunks) {
# if defined(LIBXSMM_EXT_TASKS)
        if (0 == omp_get_active_level())
# else
        if (0 == omp_in_parallel())
# endif
        { /* enable internal parallelization */
          const int max_nthreads = omp_get_max_threads();
          const int nthreads = LIBXSMM_MIN(max_nthreads, nchunks);
# if defined(LIBXSMM_EXT_TASKS)
          if (0 >= libxsmm_gemm_taskscale)
# endif
          {
            libxsmm_gemm_internal_set_batchflag(desc, c, index_stride, batchsize, 1 != nthreads);
            kernel = libxsmm_xmmdispatch(desc);
            if (NULL != kernel.xmm) {
#             pragma omp parallel num_threads(nthreads)
              /*check*/libxsmm_mmbatch_kernel(kernel, index_base, index_stride,
                stride_a, stride_b, stride_c, a, b, c, batchsize,
                omp_get_thread_num(), nthreads, desc);
            }
            else {
              result = EXIT_FAILURE;
            }
          }
# if defined(LIBXSMM_EXT_TASKS)
          else { /* tasks requested */
            const int ntasks = nthreads * libxsmm_gemm_taskscale;
            libxsmm_gemm_internal_set_batchflag(desc, c, index_stride, batchsize, 1 != ntasks);
            kernel = libxsmm_xmmdispatch(desc);
            if (NULL != kernel.xmm) {
#             pragma omp parallel num_threads(nthreads)
              { /* first thread discovering work will launch all tasks */
#               pragma omp single nowait /* anyone is good */
                { int tid;
                for (tid = 0; tid < ntasks; ++tid) {
#                 pragma omp task
                  /*check*/libxsmm_mmbatch_kernel(kernel, index_base, index_stride,
                    stride_a, stride_b, stride_c, a, b, c, batchsize,
                    tid, ntasks, desc);
                }
                }
              } /* implicit synchronization (barrier) */
            }
            else {
              result = EXIT_FAILURE;
            }
          }
# endif
        }
        else { /* assume external parallelization */
          libxsmm_gemm_internal_set_batchflag(desc, c, index_stride, batchsize, 0/*multithreaded*/);
          kernel = libxsmm_xmmdispatch(desc);
          if (NULL != kernel.xmm) {
# if defined(LIBXSMM_EXT_TASKS) /* OpenMP-tasks */
            const int ntasks = (0 == libxsmm_gemm_taskscale
              ? (LIBXSMM_GEMM_TASKSCALE)
              : libxsmm_gemm_taskscale) * omp_get_num_threads();
            int tid;
            for (tid = 0; tid < ntasks; ++tid) {
#             pragma omp task
              /*check*/libxsmm_mmbatch_kernel(kernel, index_base, index_stride,
                stride_a, stride_b, stride_c, a, b, c, batchsize,
                tid, ntasks, desc);
            }
            /* allow to omit synchronization */
            if (0 == libxsmm_nosync) {
#             pragma omp taskwait
            }
# else
            result = libxsmm_mmbatch_kernel(kernel, index_base, index_stride,
              stride_a, stride_b, stride_c, a, b, c, batchsize,
              0/*tid*/, 1/*nthreads*/, desc);
# endif
          }
          else {
            result = EXIT_FAILURE;
          }
        }
      }
      else
#endif /*defined(_OPENMP)*/
      { /* sequential */
        libxsmm_gemm_internal_set_batchflag(desc, c, index_stride, batchsize, 0/*multithreaded*/);
        kernel = libxsmm_xmmdispatch(desc);
        result = (NULL != kernel.xmm ? libxsmm_mmbatch_kernel(kernel, index_base, index_stride,
          stride_a, stride_b, stride_c, a, b, c, batchsize,
          0/*tid*/, 1/*nthreads*/, desc) : EXIT_FAILURE);
      }
    }
    else {
      result = EXIT_FAILURE;
    }
    if (EXIT_SUCCESS != result) {
      if (EXIT_SUCCESS == libxsmm_mmbatch_blas(iprec, oprec,
        transa, transb, m, n, k, alpha, a, lda, b, ldb, beta, c, ldc,
        index_base, index_stride, stride_a, stride_b, stride_c, batchsize))
      {
        if ((LIBXSMM_VERBOSITY_WARN <= libxsmm_verbosity || 0 > libxsmm_verbosity)
          && 1 == LIBXSMM_ATOMIC_ADD_FETCH(&error_once, 1, LIBXSMM_ATOMIC_RELAXED))
        {
          fprintf(stderr, "LIBXSMM WARNING: batched GEMM was falling back to BLAS!\n");
        }
      }
      else if (0 != libxsmm_verbosity /* library code is expected to be mute */
        && 1 == LIBXSMM_ATOMIC_ADD_FETCH(&error_once, 1, LIBXSMM_ATOMIC_RELAXED))
      {
        fprintf(stderr, "LIBXSMM ERROR: libxsmm_gemm_batch_omp failed!\n");
      }
    }
  }
#if defined(LIBXSMM_GEMM_CHECK)
  else if (0 != libxsmm_verbosity /* library code is expected to be mute */
    && 1 == LIBXSMM_ATOMIC_ADD_FETCH(&error_once, 1, LIBXSMM_ATOMIC_RELAXED))
  {
    fprintf(stderr, "LIBXSMM ERROR: libxsmm_mmbatch called with incorrect argument(s)!\n");
  }
#endif
}


LIBXSMM_APIEXT void libxsmm_mmbatch_begin(libxsmm_gemm_precision precision,
  const int* flags, const libxsmm_blasint* m, const libxsmm_blasint* n, const libxsmm_blasint* k,
  const libxsmm_blasint* lda, const libxsmm_blasint* ldb, const libxsmm_blasint* ldc,
  const void* alpha, const void* beta)
{
#if defined(LIBXSMM_GEMM_MMBATCH) && defined(LIBXSMM_BUILD_EXT)
# if defined(_MSC_VER)
#   pragma warning(push)
#   pragma warning(disable: 26115) /* try-lock is treated incorrectly by static analysis */
# endif
  LIBXSMM_INIT
  if (NULL != libxsmm_mmbatch_array /* batch-recording available, but not yet running */
    /* currently, batch recording is only enabled if all values are present (no complex filtering) */
    && NULL != flags && NULL != alpha && NULL != beta
    && NULL != lda && NULL != ldb && NULL != ldc
    && NULL != m && NULL != n && NULL != k
    && LIBXSMM_LOCK_ACQUIRED(LIBXSMM_LOCK_DEFAULT) == LIBXSMM_LOCK_TRYLOCK(LIBXSMM_LOCK_DEFAULT, &libxsmm_mmbatch_lock))
  {
    libxsmm_descriptor_blob blob;
    const libxsmm_gemm_descriptor *const descriptor = libxsmm_gemm_descriptor_init(&blob, precision,
      *m, *n, *k, *lda, *ldb, *ldc, alpha, beta, *flags, libxsmm_get_gemm_prefetch(LIBXSMM_GEMM_EXT_MMBATCH_PREFETCH));
    static int error_once = 0;
    int result = EXIT_SUCCESS;

    if (NULL != descriptor) {
      const unsigned int max_batchsize = (unsigned int)((LIBXSMM_GEMM_MMBATCH_SCALE) * libxsmm_mmbatch_size);
      unsigned int i;
#if !defined(NDEBUG)
      const unsigned int mmbatch_maxdepth = LIBXSMM_UP2POT(LIBXSMM_GEMM_EXT_MMBATCH_MAXDEPTH);
      LIBXSMM_ASSERT((LIBXSMM_GEMM_EXT_MMBATCH_MAXDEPTH) == mmbatch_maxdepth/*is pot*/);
#endif
      /* eventually overwrite the oldest entry */
      i = LIBXSMM_MOD2(internal_ext_gemm_batchdepth, LIBXSMM_GEMM_EXT_MMBATCH_MAXDEPTH);
      internal_ext_gemm_batchdesc[i] = libxsmm_mmbatch_desc; /* backup */
      ++internal_ext_gemm_batchdepth;

      /* ensure descriptor does not match any GEMM such that... */
      memset(&libxsmm_mmbatch_desc, 0, sizeof(libxsmm_mmbatch_desc));
      /* ...the batch stops and completely flushes */
      if (0 != internal_ext_gemm_batchsize) {
        result = internal_mmbatch_flush(internal_ext_gemm_batchdesc + i,
          ((internal_ext_gemm_batchsize - 1) % max_batchsize) + 1,
          (libxsmm_mmbatch_item*)libxsmm_mmbatch_array);
      }

      if (EXIT_SUCCESS == result) { /* enable descriptor */
        internal_ext_gemm_batchsize = 0; /* reset */
        if (0 == (LIBXSMM_MMBATCH_FLAG_STATISTIC & *flags)) {
          libxsmm_mmbatch_desc = *descriptor;
        }
        else {
          libxsmm_mmbatch_desc.flags = LIBXSMM_MMBATCH_FLAG_STATISTIC;
        }
      }
    }
    else {
      result = EXIT_FAILURE;
    }
    if (EXIT_SUCCESS != result && 0 != libxsmm_verbosity /* library code is expected to be mute */
      && 1 == LIBXSMM_ATOMIC_ADD_FETCH(&error_once, 1, LIBXSMM_ATOMIC_RELAXED))
    {
      fprintf(stderr, "LIBXSMM ERROR: GEMM batch enabling failed!\n");
    }
    LIBXSMM_LOCK_RELEASE(LIBXSMM_LOCK_DEFAULT, &libxsmm_mmbatch_lock);
  }
# if defined(_MSC_VER)
#   pragma warning(pop)
# endif
#else
  LIBXSMM_UNUSED(precision); LIBXSMM_UNUSED(flags);
  LIBXSMM_UNUSED(m); LIBXSMM_UNUSED(n); LIBXSMM_UNUSED(k);
  LIBXSMM_UNUSED(lda); LIBXSMM_UNUSED(ldb); LIBXSMM_UNUSED(ldc);
  LIBXSMM_UNUSED(alpha); LIBXSMM_UNUSED(beta);
#endif
}


LIBXSMM_APIEXT void libxsmm_mmbatch_end(void)
{
#if defined(LIBXSMM_GEMM_MMBATCH) && defined(LIBXSMM_BUILD_EXT)
# if defined(_MSC_VER)
#   pragma warning(push)
#   pragma warning(disable: 26115) /* try-lock is treated incorrectly by static analysis */
# endif
  if (LIBXSMM_LOCK_ACQUIRED(LIBXSMM_LOCK_DEFAULT) == LIBXSMM_LOCK_TRYLOCK(LIBXSMM_LOCK_DEFAULT, &libxsmm_mmbatch_lock)) {
    const unsigned int max_batchsize = (unsigned int)((LIBXSMM_GEMM_MMBATCH_SCALE) * libxsmm_mmbatch_size);
    const libxsmm_gemm_descriptor flushdesc = libxsmm_mmbatch_desc;
    static int error_once = 0;
#if !defined(NDEBUG)
    const unsigned int mmbatch_maxdepth = LIBXSMM_UP2POT(LIBXSMM_GEMM_EXT_MMBATCH_MAXDEPTH);
#endif
    /* ensure descriptor does not match any GEMM such that... */
    memset(&libxsmm_mmbatch_desc, 0, sizeof(libxsmm_mmbatch_desc));
    /* ...the batch stops and completely flushes */
    if (EXIT_SUCCESS == internal_mmbatch_flush(&flushdesc,
      0 != internal_ext_gemm_batchsize ? (((internal_ext_gemm_batchsize - 1) % max_batchsize) + 1) : 0,
      (libxsmm_mmbatch_item*)libxsmm_mmbatch_array))
    {
      internal_ext_gemm_batchsize = 0; /* reset */
      --internal_ext_gemm_batchdepth; /* restore the previous descriptor */
      assert((LIBXSMM_GEMM_EXT_MMBATCH_MAXDEPTH) == mmbatch_maxdepth/*is pot*/); /* no LIBXSMM_ASSERT! */
      libxsmm_mmbatch_desc = internal_ext_gemm_batchdesc[LIBXSMM_MOD2(internal_ext_gemm_batchdepth, LIBXSMM_GEMM_EXT_MMBATCH_MAXDEPTH)];
    }
    else if (0 != libxsmm_verbosity /* library code is expected to be mute */
      && 1 == LIBXSMM_ATOMIC_ADD_FETCH(&error_once, 1, LIBXSMM_ATOMIC_RELAXED))
    {
      fprintf(stderr, "LIBXSMM ERROR: GEMM batch processing failed!\n");
    }
    LIBXSMM_LOCK_RELEASE(LIBXSMM_LOCK_DEFAULT, &libxsmm_mmbatch_lock);
  }
# if defined(_MSC_VER)
#   pragma warning(pop)
# endif
#endif
}


#if defined(LIBXSMM_BUILD) && defined(LIBXSMM_BUILD_EXT)

/* implementation provided for Fortran 77 compatibility */
LIBXSMM_APIEXT void LIBXSMM_FSYMBOL(libxsmm_xgemm_omp)(const libxsmm_gemm_precision*, const libxsmm_gemm_precision*,
  const char*, const char*, const libxsmm_blasint*, const libxsmm_blasint*, const libxsmm_blasint*,
  const double*, const double*, const libxsmm_blasint*, const double*, const libxsmm_blasint*,
  const double*, double*, const libxsmm_blasint*);
LIBXSMM_APIEXT void LIBXSMM_FSYMBOL(libxsmm_xgemm_omp)(const libxsmm_gemm_precision* iprec, const libxsmm_gemm_precision* oprec,
  const char* transa, const char* transb, const libxsmm_blasint* m, const libxsmm_blasint* n, const libxsmm_blasint* k,
  const double* alpha, const double* a, const libxsmm_blasint* lda, const double* b, const libxsmm_blasint* ldb,
  const double* beta, double* c, const libxsmm_blasint* ldc)
{
  LIBXSMM_ASSERT(NULL != iprec && NULL != oprec);
  libxsmm_xgemm_omp(*iprec, *oprec, transa, transb, m, n, k, alpha, a, lda, b, ldb, beta, c, ldc);
}


/* implementation provided for Fortran 77 compatibility */
LIBXSMM_APIEXT void LIBXSMM_FSYMBOL(libxsmm_dgemm_omp)(const char*, const char*,
  const libxsmm_blasint*, const libxsmm_blasint*, const libxsmm_blasint*,
  const double*, const double*, const libxsmm_blasint*,
  const double*, const libxsmm_blasint*,
  const double*, double*, const libxsmm_blasint*);
LIBXSMM_APIEXT void LIBXSMM_FSYMBOL(libxsmm_dgemm_omp)(const char* transa, const char* transb,
  const libxsmm_blasint* m, const libxsmm_blasint* n, const libxsmm_blasint* k,
  const double* alpha, const double* a, const libxsmm_blasint* lda,
  const double* b, const libxsmm_blasint* ldb,
  const double* beta, double* c, const libxsmm_blasint* ldc)
{
  libxsmm_dgemm_omp(transa, transb, m, n, k, alpha, a, lda, b, ldb, beta, c, ldc);
}


/* implementation provided for Fortran 77 compatibility */
LIBXSMM_APIEXT void LIBXSMM_FSYMBOL(libxsmm_sgemm_omp)(const char*, const char*,
  const libxsmm_blasint*, const libxsmm_blasint*, const libxsmm_blasint*,
  const float*, const float*, const libxsmm_blasint*,
  const float*, const libxsmm_blasint*,
  const float*, float*, const libxsmm_blasint*);
LIBXSMM_APIEXT void LIBXSMM_FSYMBOL(libxsmm_sgemm_omp)(const char* transa, const char* transb,
  const libxsmm_blasint* m, const libxsmm_blasint* n, const libxsmm_blasint* k,
  const float* alpha, const float* a, const libxsmm_blasint* lda,
  const float* b, const libxsmm_blasint* ldb,
  const float* beta, float* c, const libxsmm_blasint* ldc)
{
  libxsmm_sgemm_omp(transa, transb, m, n, k, alpha, a, lda, b, ldb, beta, c, ldc);
}


/* implementation provided for Fortran 77 compatibility */
LIBXSMM_APIEXT void LIBXSMM_FSYMBOL(libxsmm_gemm_batch_omp)(const libxsmm_gemm_precision*, const libxsmm_gemm_precision*,
  const char*, const char*, const libxsmm_blasint*, const libxsmm_blasint*, const libxsmm_blasint*,
  const void*, const void*, const libxsmm_blasint*, const void*, const libxsmm_blasint*,
  const void*, void*, const libxsmm_blasint*, const libxsmm_blasint*, const libxsmm_blasint*,
  const libxsmm_blasint[], const libxsmm_blasint[], const libxsmm_blasint[],
  const libxsmm_blasint*);
LIBXSMM_APIEXT void LIBXSMM_FSYMBOL(libxsmm_gemm_batch_omp)(const libxsmm_gemm_precision* iprec, const libxsmm_gemm_precision* oprec,
  const char* transa, const char* transb, const libxsmm_blasint* m, const libxsmm_blasint* n, const libxsmm_blasint* k,
  const void* alpha, const void* a, const libxsmm_blasint* lda, const void* b, const libxsmm_blasint* ldb,
  const void* beta, void* c, const libxsmm_blasint* ldc, const libxsmm_blasint* index_base, const libxsmm_blasint* index_stride,
  const libxsmm_blasint stride_a[], const libxsmm_blasint stride_b[], const libxsmm_blasint stride_c[],
  const libxsmm_blasint* batchsize)
{
  LIBXSMM_ASSERT(NULL != iprec && NULL != oprec && NULL != m && NULL != n && NULL != k);
  LIBXSMM_ASSERT(NULL != index_base && NULL != index_stride && NULL != batchsize);
  libxsmm_gemm_batch_omp(*iprec, *oprec, transa, transb, *m, *n, *k, alpha, a, lda, b, ldb, beta, c, ldc,
    *index_base, *index_stride, stride_a, stride_b, stride_c, *batchsize);
}


/* implementation provided for Fortran 77 compatibility */
LIBXSMM_APIEXT void LIBXSMM_FSYMBOL(libxsmm_mmbatch_begin)(const libxsmm_gemm_precision*,
  const int*, const libxsmm_blasint*, const libxsmm_blasint*, const libxsmm_blasint*,
  const libxsmm_blasint*, const libxsmm_blasint*, const libxsmm_blasint*,
  const void*, const void*);
LIBXSMM_APIEXT void LIBXSMM_FSYMBOL(libxsmm_mmbatch_begin)(const libxsmm_gemm_precision* precision,
  const int* flags, const libxsmm_blasint* m, const libxsmm_blasint* n, const libxsmm_blasint* k,
  const libxsmm_blasint* lda, const libxsmm_blasint* ldb, const libxsmm_blasint* ldc,
  const void* alpha, const void* beta)
{
  LIBXSMM_ASSERT(NULL != precision);
  libxsmm_mmbatch_begin(*precision, flags, m, n, k, lda, ldb, ldc, alpha, beta);
}


/* implementation provided for Fortran 77 compatibility */
LIBXSMM_APIEXT void LIBXSMM_FSYMBOL(libxsmm_mmbatch_end)(void);
LIBXSMM_APIEXT void LIBXSMM_FSYMBOL(libxsmm_mmbatch_end)(void)
{
  libxsmm_mmbatch_end();
}

#endif /*defined(LIBXSMM_BUILD) && defined(LIBXSMM_BUILD_EXT)*/

