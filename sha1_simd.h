//
// Arquiteturas de Alto Desempenho 2026/2027 --- assignment A1
//
// SIMD engines (AVX, AVX2, AVX512F, NEON): N_LANES attempts per SHA1_UPDATE
//
// lane scheme:
//   * the first k variable bytes are the "lane digits": lane j encodes j in them (mixed radix),
//     so different lanes never test the same counter (no overlap of the search space)
//   * the remaining variable bytes form the counter, advanced once per vector iteration by the
//     scalar odometer (search_next), identically for all lanes
//   * hence lane j's tail word = common word ^ delta[word][j], where delta holds the lane digits
//     and, for PNG, the CRC change caused by them (CRC linearity: one vector xor per attempt)
//   * the interleaved layout of aad_sha1_cpu.h is used: vec[word] holds that word for all lanes
//
// requires: sha1_search.h and the result_t / CHECK_PERIOD / now() / search_stopped() definitions
// of sha1_engines.h
//

#ifndef SHA1_SIMD
#define SHA1_SIMD

#define MAX_LANES  16


//
// lane data (shared by all SIMD widths)
//

typedef struct
{
  int n_lanes;                                                         // number of lanes
  int base;                                                            // first lane digit
  int k;                                                               // number of lane digits
  int wfirst,wlast;                                                    // tail words that change during the search
  u32_t delta[16 * MAX_TAIL][MAX_LANES] __attribute__((aligned(64)));  // per-lane xor mask of each tail word
}
lanes_t;


//
// digit p (0 <= p < k) of lane j
//

__attribute__((unused))
static u32_t lane_digit(const search_t *s,int base,int k,int j,int p)
{
  for(int q = base + k - 1;q > p;q--)
    j /= s->hi[q] - s->lo[q] + 1;
  return (u32_t)(j % (s->hi[p] - s->lo[p] + 1));
}


//
// choose the lane digits (the bytes from lane_base on, lane_base = first on the first call, so a
// search can be resumed) and build the per-lane masks
//
// return value: 0 (success), -1 (too few variable bytes)
//

__attribute__((unused))
static int lanes_init(search_t *s,lanes_t *L,int n_lanes)
{
  int k,capacity = 1,last,b;

  if(s->lane_base < 0)
    s->lane_base = s->first;
  b = s->lane_base;
  for(k = 0;b + k < s->var_len && capacity < n_lanes;k++)
    capacity *= s->hi[b + k] - s->lo[b + k] + 1;
  if(capacity < n_lanes || b + k >= s->var_len)
  {
    fprintf(stderr,"lanes_init: not enough variable bytes for %d lanes\n",n_lanes);
    return -1;
  }
  memset(L,0,sizeof(*L));
  L->n_lanes = n_lanes;
  L->base = b;
  L->k = k;
  s->first = b + k; // the odometer no longer touches the lane digits
  for(int p = b;p < b + k;p++)
    search_set_byte(s,p,s->lo[p]); // lane digits of the common tail are at lo[]
  for(int j = 0;j < n_lanes;j++)
  {
    u32_t crc_delta = 0u;

    for(int p = b;p < b + k;p++)
    {
      u32_t x = s->lo[p] ^ (s->lo[p] + lane_digit(s,b,k,j,p)); // byte change of lane digit p
      int off = s->var_off + p;

      L->delta[off >> 2][j] ^= x << (24 - 8 * (off & 3));
      if(s->crc_off >= 0)
        crc_delta ^= s->crc_tab[p][x];
    }
    if(s->crc_off >= 0)
      for(int y = 0;y < 4;y++)
      {
        int off = s->crc_off + y;

        L->delta[off >> 2][j] ^= ((crc_delta >> (24 - 8 * y)) & 0xFFu) << (24 - 8 * (off & 3));
      }
  }
  last = s->var_off + s->var_len - 1;
  if(s->crc_off >= 0 && s->crc_off + 3 > last)
    last = s->crc_off + 3;
  L->wfirst = s->var_off >> 2;
  L->wlast = last >> 2;
  return 0;
}


//
// put the lane digits of lane j into the common tail (j = 0 restores them to lo[])
//

__attribute__((unused))
static void lanes_apply(search_t *s,const lanes_t *L,int j)
{
  for(int p = L->base;p < L->base + L->k;p++)
    search_set_byte(s,p,s->lo[p] + lane_digit(s,L->base,L->k,j,p));
}


//
// engine template (C has no templates, so a macro is used, as in aad_sha1_cpu_tests.c)
//   NAME     --- engine name
//   T        --- vector data type
//   N_LANES  --- number of lanes
//   BCAST(x) --- broadcast a u32_t to all lanes
//   UPDATE   --- SHA1_UPDATE wrapper from aad_sha1_cpu.h
//
// run_NAME()  --- search / benchmark
// test_NAME() --- compare every lane against the scalar engine and the full-file SHA1
//

#define DEFINE_SIMD_ENGINE(NAME,T,N_LANES,BCAST,UPDATE)                                          \
                                                                                                 \
static void setup_ ## NAME(search_t *s,lanes_t *L,T *vec,T *dv,T *st)                            \
{                                                                                                \
  if(lanes_init(s,L,N_LANES) != 0)                                                               \
    exit(1);                                                                                     \
  for(int w = 0;w < 16 * s->n_chunks;w++)                                                        \
  {                                                                                              \
    memcpy(&dv[w],&L->delta[w][0],sizeof(T));                                                   \
    vec[w] = BCAST(s->w[w]) ^ dv[w];                                                             \
  }                                                                                              \
  for(int i = 0;i < 5;i++)                                                                       \
    st[i] = BCAST(s->state[i]);                                                                  \
}                                                                                                \
                                                                                                 \
static inline void hash_ ## NAME(const search_t *s,const lanes_t *L,T *vec,const T *dv,          \
                                 const T *st,T *hash)                                            \
{                                                                                                \
  for(int w = L->wfirst;w <= L->wlast;w++)                                                       \
    vec[w] = BCAST(s->w[w]) ^ dv[w]; /* only the words that change */                           \
  for(int i = 0;i < 5;i++)                                                                       \
    hash[i] = st[i];                                                                             \
  for(int c = 0;c < s->n_chunks;c++)                                                             \
    UPDATE(&vec[16 * c],hash);                                                                   \
}                                                                                                \
                                                                                                 \
__attribute__((unused))                                                                          \
static void run_ ## NAME(search_t *s,double max_seconds,int bench,u32_t mask,result_t *res)      \
{                                                                                                \
  lanes_t L; /* on the stack: one per OpenMP thread */                                           \
  T vec[16 * MAX_TAIL],dv[16 * MAX_TAIL],st[5],hash[5],z;                                        \
  double t0 = now();                                                                             \
                                                                                                 \
  setup_ ## NAME(s,&L,vec,dv,st);                                                                \
  for(;;)                                                                                        \
  {                                                                                              \
    for(u32_t it = 0u;it < CHECK_PERIOD / N_LANES;it++)                                          \
    {                                                                                            \
      hash_ ## NAME(s,&L,vec,dv,st,hash);                                                        \
      res->attempts += N_LANES;                                                                  \
      z = hash[4] & BCAST(mask);                                                                 \
      for(int j = 0;j < N_LANES;j++)                                                             \
        if(((const u32_t *)&z)[j] == 0u)                                                         \
        { /* lane j has a solution */                                                            \
          res->n_found++;                                                                        \
          if(!bench)                                                                             \
          {                                                                                      \
            lanes_apply(s,&L,j);                                                                 \
            res->found = 1;                                                                      \
            res->seconds = now() - t0;                                                           \
            return;                                                                              \
          }                                                                                      \
        }                                                                                        \
      if(!search_next(s))                                                                        \
      {                                                                                          \
        res->exhausted = 1;                                                                      \
        res->seconds = now() - t0;                                                               \
        return;                                                                                  \
      }                                                                                          \
    }                                                                                            \
    if((res->seconds = now() - t0) >= max_seconds || search_stopped())                           \
      return;                                                                                    \
  }                                                                                              \
}                                                                                                \
                                                                                                 \
__attribute__((unused))                                                                          \
static int test_ ## NAME(search_t *s,file_template_t *t,int n_tests)                             \
{                                                                                                \
  lanes_t L;                                                                                     \
  T vec[16 * MAX_TAIL],dv[16 * MAX_TAIL],st[5],hash[5];                                          \
  u32_t h[5],g[5];                                                                               \
                                                                                                 \
  setup_ ## NAME(s,&L,vec,dv,st);                                                                \
  for(int n = 0;n < n_tests;n++)                                                                 \
  {                                                                                              \
    hash_ ## NAME(s,&L,vec,dv,st,hash);                                                          \
    for(int j = 0;j < N_LANES;j++)                                                               \
    {                                                                                            \
      lanes_apply(s,&L,j);                                                                       \
      search_hash(s,h);             /* scalar engine, same counter */                            \
      search_store(s,t);                                                                         \
      sha1_buffer(t->data,t->size,g); /* SHA1 of the whole file */                               \
      for(int i = 0;i < 5;i++)                                                                   \
        if(((const u32_t *)&hash[i])[j] != h[i] || h[i] != g[i])                                 \
        {                                                                                        \
          fprintf(stderr,"test_" # NAME ": mismatch (test %d, lane %d, word %d)\n",n,j,i);       \
          return -1;                                                                             \
        }                                                                                        \
    }                                                                                            \
    lanes_apply(s,&L,0);                                                                         \
    for(int r = 1 + (n * 7919) % 1000;r > 0;r--) /* jump ahead, exercising the carries */        \
      (void)search_next(s);                                                                      \
  }                                                                                              \
  printf("test_" # NAME ": %d tests x %d lanes passed\n",n_tests,N_LANES);                       \
  return 0;                                                                                      \
}


//
// instantiations
//

#if defined(__AVX__)
# define BCAST_AVX(x)  ((v4si){ FOUR(x) })
DEFINE_SIMD_ENGINE(avx,v4si,4,BCAST_AVX,sha1_update_avx)
#endif

#if defined(__AVX2__)
# define BCAST_AVX2(x)  ((v8si){ FOUR(x),FOUR(x) })
DEFINE_SIMD_ENGINE(avx2,v8si,8,BCAST_AVX2,sha1_update_avx2)
#endif

#if defined(__AVX512F__)
# define BCAST_AVX512F(x)  ((v16si){ FOUR(x),FOUR(x),FOUR(x),FOUR(x) })
DEFINE_SIMD_ENGINE(avx512f,v16si,16,BCAST_AVX512F,sha1_update_avx512f)
#endif

#if defined(__ARM_NEON)
# define BCAST_NEON(x)  vdupq_n_u32(x)
DEFINE_SIMD_ENGINE(neon,uint32x4_t,4,BCAST_NEON,sha1_update_neon)
#endif

#if defined(__wasm_simd128__)
//
// WebAssembly SIMD (128 bits, emcc -msimd128): same SHA1_UPDATE macro, generic 4 x u32 vectors
//
typedef unsigned int v4w __attribute__((vector_size(16)));

static void sha1_update_wasm(v4w *interleaved4_chunk_data,v4w *interleaved4_hash)
{ // four interleaved messages -> four interleaved SHA1 secure hashes
# define T                v4w
# define C(c)             (v4w){ (c),(c),(c),(c) }
# define ROTATE(x,n)      (((x) << (n)) | ((x) >> (32 - (n))))
# define CHUNK_DATA(idx)  interleaved4_chunk_data[idx]
# define HASH(idx)        interleaved4_hash[idx]
  SHA1_UPDATE();
# undef T
# undef C
# undef ROTATE
# undef CHUNK_DATA
# undef HASH
}

# define BCAST_WASM(x)  ((v4w){ (x),(x),(x),(x) })
DEFINE_SIMD_ENGINE(wasm_simd,v4w,4,BCAST_WASM,sha1_update_wasm)
#endif

#endif
