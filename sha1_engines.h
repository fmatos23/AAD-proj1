//
// Arquiteturas de Alto Desempenho 2026/2027 --- assignment A1
//
// search engines (scalar, SIMD, CUDA) and the OpenMP parallel driver, shared by sha1_find,
// sha1_client and sha1_find_mpi
//
// splitting of the counter digits (most significant first), each level done with search_split():
//   [ client/rank digits | thread digits | SIMD lane digits | odometer digits ]
// every level freezes its digits to a distinct value, so no two workers test the same counter
//
// requires: sha1_search.h (and <omp.h> if compiled with -fopenmp)
//

#ifndef SHA1_ENGINES
#define SHA1_ENGINES

#ifdef _OPENMP
# include <omp.h>
#endif


//
// time and search result
//

static double now(void)
{
  struct timespec ts;

  (void)clock_gettime(CLOCK_MONOTONIC,&ts);
  return (double)ts.tv_sec + 1.0e-9 * (double)ts.tv_nsec;
}

typedef struct
{
  uint64_t attempts; // number of tested counters
  uint64_t n_found;  // number of solutions found
  int found;         // the search state holds a solution
  int exhausted;     // the whole search space was visited
  double seconds;    // elapsed wall time
}
result_t;

#define CHECK_PERIOD (1u << 16) // attempts between clock (and stop flag) checks


//
// stop flag: set by the thread that finds a solution, polled by the others every CHECK_PERIOD attempts
//

static int search_stop_flag;

static inline int search_stopped(void)
{
  return __atomic_load_n(&search_stop_flag,__ATOMIC_RELAXED);
}

static inline void search_set_stop(int v)
{
  __atomic_store_n(&search_stop_flag,v,__ATOMIC_RELAXED);
}


//
// scalar engine (one SHA1_UPDATE per tail chunk per attempt)
//

__attribute__((unused))
static void run_scalar(search_t *s,double max_seconds,int bench,u32_t mask,result_t *res)
{
  double t0 = now();
  u32_t h[5];

  for(;;)
  {
    for(u32_t j = 0u;j < CHECK_PERIOD;j++)
    {
      search_hash(s,h);
      res->attempts++;
      if((h[4] & mask) == 0u)
      { // the last bits of the SHA1 are zero
        res->n_found++;
        if(!bench)
        {
          res->found = 1;
          res->seconds = now() - t0;
          return;
        }
      }
      if(!search_next(s))
      {
        res->exhausted = 1;
        res->seconds = now() - t0;
        return;
      }
    }
    if((res->seconds = now() - t0) >= max_seconds || search_stopped())
      return;
  }
}


//
// SIMD and CUDA engines
//

#include "sha1_simd.h"
#include "sha1_shani.h"
#ifdef WITH_ISPC
# include "sha1_ispc.h"
#endif
#ifdef WITH_CUDA
# include "sha1_cuda_host.h"
#endif
#ifdef WITH_OPENCL
# include "sha1_opencl_host.h"
#endif

typedef struct
{
  const char *name;
  int n_lanes;   // attempts per SHA1_UPDATE call (SIMD width)
  int n_threads; // concurrent threads per engine instance
  void (*run)(search_t *,double,int,u32_t,result_t *);
  int (*test)(search_t *,file_template_t *,int);
}
engine_t;

__attribute__((unused))
static const engine_t engines[] =
{
  { "scalar"   , 1,1,run_scalar   ,NULL           },
#if defined(__AVX__)
  { "avx"      , 4,1,run_avx      ,test_avx       },
#endif
#if defined(__AVX2__)
  { "avx2"     , 8,1,run_avx2     ,test_avx2      },
#endif
#if defined(__AVX512F__)
  { "avx512f"  ,16,1,run_avx512f  ,test_avx512f   },
#endif
#if defined(__ARM_NEON)
  { "neon"     , 4,1,run_neon     ,test_neon      },
#endif
#if defined(__SHA__) && defined(__SSE4_1__)
  { "sha_ni"   ,SHA_LANES,1,run_sha_ni   ,test_sha_ni    },
#endif
#if defined(__ARM_NEON) && (defined(__ARM_FEATURE_SHA2) || defined(__ARM_FEATURE_CRYPTO))
  { "neon_sha1",SHA_LANES,1,run_neon_sha1,test_neon_sha1 },
#endif
#ifdef WITH_ISPC
  { "ispc"     ,ISPC_LANES,1,run_ispc    ,test_ispc      },
#endif
#if defined(__wasm_simd128__)
  { "wasm_simd", 4,1,run_wasm_simd,test_wasm_simd },
#endif
#ifdef WITH_CUDA
  { "cuda"     , 1,(int)CUDA_THREADS,run_cuda,test_cuda },
#endif
#ifdef WITH_OPENCL
  { "opencl"   , 1,(int)OCL_THREADS_DEF,run_opencl,test_opencl },
#endif
};

#define N_ENGINES  ((int)(sizeof(engines) / sizeof(engines[0])))

__attribute__((unused))
static const engine_t *find_engine(const char *name)
{
  for(int i = 0;i < N_ENGINES;i++)
    if(strcmp(name,engines[i].name) == 0)
      return &engines[i];
  return NULL;
}

__attribute__((unused))
static void list_engines(FILE *fp)
{
  fprintf(fp,"engines:");
  for(int i = 0;i < N_ENGINES;i++)
    fprintf(fp," %s",engines[i].name);
  fprintf(fp,"\n");
}


//
// parallel (OpenMP) driver: one private search state per thread, each with its own thread digits;
// the states persist between par_run() calls, so a search can be resumed (used by MPI time slices)
//

typedef struct
{
  int n;          // number of threads
  search_t *st;   // per-thread search states
  int *done;      // the thread visited its whole part of the search space
}
par_t;

__attribute__((unused))
static int par_init(par_t *P,const search_t *s,int n_threads)
{
#ifndef _OPENMP
  n_threads = 1;
#endif
  P->n = n_threads;
  P->st = (search_t *)malloc((size_t)n_threads * sizeof(search_t));
  P->done = (int *)calloc((size_t)n_threads,sizeof(int));
  if(P->st == NULL || P->done == NULL)
    return -1;
  for(int t = 0;t < n_threads;t++)
  {
    memcpy(&P->st[t],s,sizeof(search_t));
    if(search_split(&P->st[t],(uint64_t)n_threads,(uint64_t)t) < 0)
      return -1;
  }
  return 0;
}

__attribute__((unused))
static void par_free(par_t *P)
{
  free(P->st);
  free(P->done);
  P->st = NULL;
  P->done = NULL;
}

//
// run engine e on all threads; on success the solution is copied to *winner
//

__attribute__((unused))
static void par_run(par_t *P,const engine_t *e,double max_seconds,int bench,u32_t mask,result_t *res,search_t *winner)
{
  result_t r[P->n];
  int win = -1,n_exhausted = 0;

  memset(r,0,sizeof(r));
  search_set_stop(0);
#ifdef _OPENMP
# pragma omp parallel for num_threads(P->n) schedule(static,1)
#endif
  for(int t = 0;t < P->n;t++)
  {
    if(P->done[t])
      continue; // never revisit an exhausted part
    e->run(&P->st[t],max_seconds,bench,mask,&r[t]);
    P->done[t] = r[t].exhausted;
    if(r[t].found)
    {
#ifdef _OPENMP
#     pragma omp critical
#endif
      if(win < 0)
        win = t;
      search_set_stop(1); // the other threads stop at their next check
    }
  }
  for(int t = 0;t < P->n;t++)
  {
    res->attempts += r[t].attempts;
    res->n_found += r[t].n_found;
    n_exhausted += P->done[t];
    if(r[t].seconds > res->seconds)
      res->seconds = r[t].seconds;
  }
  res->exhausted = (n_exhausted == P->n);
  if(win >= 0)
  {
    res->found = 1;
    memcpy(winner,&P->st[win],sizeof(search_t));
  }
}

#endif
