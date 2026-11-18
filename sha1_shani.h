//
// Arquiteturas de Alto Desempenho 2026/2027 --- assignment A1
//
// SHA1 hardware instructions engines
//   * sha_ni    --- Intel/AMD SHA extensions (sha1rnds4, sha1nexte, sha1msg1, sha1msg2), gcc -msha
//                   (available, for example, on AMD Zen and on Intel Goldmont, Ice Lake and newer)
//   * neon_sha1 --- ARMv8 crypto extensions (sha1c, sha1p, sha1m, sha1h, sha1su0, sha1su1)
//
// these instructions compute the SHA1 of ONE message, but they have a latency of several clock
// cycles; to keep the execution units busy SHA_LANES independent messages are processed in an
// interleaved way (instruction level parallelism instead of data level parallelism)
//
// the lane scheme is the one of sha1_simd.h (lanes_init(), lanes_apply()): lane j's tail word is
// the common tail word xored with delta[word][j] (lane digits and, for PNG, the CRC change)
//
// requires: sha1_simd.h (lanes_t) and the result_t / CHECK_PERIOD / now() / search_stopped()
// definitions of sha1_engines.h
//

#ifndef SHA1_SHANI
#define SHA1_SHANI

#ifndef SHA_LANES
# define SHA_LANES  4 // number of interleaved messages (2, 4 or 8)
#endif


//
// engine template (run_NAME() and test_NAME()), shared by the Intel/AMD and ARM versions
//   NAME   --- engine name
//   M      --- 128-bit register type holding 4 message words
//   PACK   --- M PACK(w0,w1,w2,w3): build a message register from 4 consecutive message words
//   UPDATE --- void UPDATE(const M msg[][4],u32_t hash[][5]): SHA1_LANES interleaved chunk updates
//              (msg[lane][q] holds the message words 4q..4q+3 of that lane)
//

#define DEFINE_SHA_ENGINE(NAME,M,PACK,UPDATE)                                                      \
                                                                                                   \
static void setup_ ## NAME(search_t *s,lanes_t *L,M dq[][4 * MAX_TAIL],M msg[][4 * MAX_TAIL])                    \
{                                                                                                  \
  if(lanes_init(s,L,SHA_LANES) != 0)                                                               \
    exit(1);                                                                                       \
  for(int j = 0;j < SHA_LANES;j++)                                                                 \
    for(int q = 0;q < 4 * s->n_chunks;q++)                                                         \
      dq[j][q] = PACK(L->delta[4 * q][j],L->delta[4 * q + 1][j],L->delta[4 * q + 2][j],         \
                         L->delta[4 * q + 3][j]);                                                  \
  for(int j = 0;j < SHA_LANES;j++)                                                                 \
    for(int q = 0;q < 4 * s->n_chunks;q++)                                                         \
      msg[j][q] = PACK(s->w[4 * q],s->w[4 * q + 1],s->w[4 * q + 2],s->w[4 * q + 3]) ^ dq[j][q];    \
}                                                                                                  \
                                                                                                   \
static inline void hash_ ## NAME(const search_t *s,const lanes_t *L,M dq[][4 * MAX_TAIL],       \
                                 M msg[][4 * MAX_TAIL],u32_t h[][5])                              \
{                                                                                                  \
  for(int q = L->wfirst >> 2;q <= (L->wlast >> 2);q++) /* only the words that change */           \
  {                                                                                                \
    const u32_t *w = &s->w[4 * q];                                                                 \
    M common = PACK(w[0],w[1],w[2],w[3]);                                                          \
                                                                                                   \
    for(int j = 0;j < SHA_LANES;j++)                                                               \
      msg[j][q] = common ^ dq[j][q];                                                               \
  }                                                                                                \
  for(int j = 0;j < SHA_LANES;j++)                                                                 \
    for(int i = 0;i < 5;i++)                                                                       \
      h[j][i] = s->state[i];                                                                       \
  for(int c = 0;c < s->n_chunks;c++)                                                               \
  {                                                                                                \
    M m[SHA_LANES][4];                                                                             \
                                                                                                   \
    for(int j = 0;j < SHA_LANES;j++)                                                               \
      for(int q = 0;q < 4;q++)                                                                     \
        m[j][q] = msg[j][4 * c + q];                                                               \
    UPDATE(m,h);                                                                                   \
  }                                                                                                \
}                                                                                                  \
                                                                                                   \
__attribute__((unused))                                                                            \
static void run_ ## NAME(search_t *s,double max_seconds,int bench,u32_t mask,result_t *res)        \
{                                                                                                  \
  lanes_t L;                                                                                       \
  M dq[SHA_LANES][4 * MAX_TAIL],msg[SHA_LANES][4 * MAX_TAIL];                                \
  u32_t h[SHA_LANES][5];                                                                           \
  double t0 = now();                                                                               \
                                                                                                   \
  setup_ ## NAME(s,&L,dq,msg);                                                                         \
  for(;;)                                                                                          \
  {                                                                                                \
    for(u32_t it = 0u;it < CHECK_PERIOD / SHA_LANES;it++)                                          \
    {                                                                                              \
      hash_ ## NAME(s,&L,dq,msg,h);                                                                       \
      res->attempts += SHA_LANES;                                                                  \
      for(int j = 0;j < SHA_LANES;j++)                                                             \
        if((h[j][4] & mask) == 0u)                                                                 \
        { /* lane j has a solution */                                                              \
          res->n_found++;                                                                          \
          if(!bench)                                                                               \
          {                                                                                        \
            lanes_apply(s,&L,j);                                                                   \
            res->found = 1;                                                                        \
            res->seconds = now() - t0;                                                             \
            return;                                                                                \
          }                                                                                        \
        }                                                                                          \
      if(!search_next(s))                                                                          \
      {                                                                                            \
        res->exhausted = 1;                                                                        \
        res->seconds = now() - t0;                                                                 \
        return;                                                                                    \
      }                                                                                            \
    }                                                                                              \
    if((res->seconds = now() - t0) >= max_seconds || search_stopped())                             \
      return;                                                                                      \
  }                                                                                                \
}                                                                                                  \
                                                                                                   \
__attribute__((unused))                                                                            \
static int test_ ## NAME(search_t *s,file_template_t *t,int n_tests)                               \
{                                                                                                  \
  lanes_t L;                                                                                       \
  M dq[SHA_LANES][4 * MAX_TAIL],msg[SHA_LANES][4 * MAX_TAIL];                                \
  u32_t hh[SHA_LANES][5],h[5],g[5];                                                                \
                                                                                                   \
  setup_ ## NAME(s,&L,dq,msg);                                                                         \
  for(int n = 0;n < n_tests;n++)                                                                   \
  {                                                                                                \
    hash_ ## NAME(s,&L,dq,msg,hh);                                                                        \
    for(int j = 0;j < SHA_LANES;j++)                                                               \
    {                                                                                              \
      lanes_apply(s,&L,j);                                                                         \
      search_hash(s,h);             /* scalar engine, same counter */                              \
      search_store(s,t);                                                                           \
      sha1_buffer(t->data,t->size,g); /* SHA1 of the whole file */                                 \
      for(int i = 0;i < 5;i++)                                                                     \
        if(hh[j][i] != h[i] || h[i] != g[i])                                                       \
        {                                                                                          \
          fprintf(stderr,"test_" # NAME ": mismatch (test %d, lane %d, word %d)\n",n,j,i);         \
          return -1;                                                                               \
        }                                                                                          \
    }                                                                                              \
    lanes_apply(s,&L,0);                                                                           \
    for(int r = 1 + (n * 7919) % 1000;r > 0;r--) /* jump ahead, exercising the carries */          \
      (void)search_next(s);                                                                        \
  }                                                                                                \
  printf("test_" # NAME ": %d tests x %d lanes passed\n",n_tests,SHA_LANES);                       \
  return 0;                                                                                        \
}


//
// Intel/AMD SHA extensions
//
// register layout: a message register holds words w0..w3 with w0 in the most significant 32 bits
// (lane 3), the state register ABCD holds A in lane 3, and E lives in lane 3 of its own register
//

#if defined(__SHA__) && defined(__SSE4_1__)
# include <immintrin.h>

# define SHANI_PACK(w0,w1,w2,w3)  _mm_set_epi32((int)(w0),(int)(w1),(int)(w2),(int)(w3))

//
// four rounds (group g, 0 <= g <= 19) of all lanes
//   * message schedule for g >= 4: W[g] = msg2(msg1(W[g-4],W[g-3]) ^ W[g-2],W[g-1]) (in place, W[g%4])
//   * E: E0 + W[0] for g = 0, nexte(ABCD before the previous group,W[g]) otherwise
//   * the round function/constant (immediate operand) is g / 5
//

# define SHANI_GROUP(g)                                                                           \
  for(int j = 0;j < SHA_LANES;j++)                                                                \
  {                                                                                               \
    if((g) >= 4)                                                                                  \
      W[j][(g) & 3] = _mm_sha1msg2_epu32(_mm_xor_si128(_mm_sha1msg1_epu32(W[j][(g) & 3],          \
                                         W[j][((g) + 1) & 3]),W[j][((g) + 2) & 3]),W[j][((g) + 3) & 3]); \
    E[j] = ((g) == 0) ? _mm_add_epi32(E[j],W[j][0]) : _mm_sha1nexte_epu32(P[j],W[j][(g) & 3]);    \
    P[j] = ABCD[j];                                                                               \
    ABCD[j] = _mm_sha1rnds4_epu32(ABCD[j],E[j],(g) / 5);                                          \
  }

static inline void sha1_update_shani(const __m128i msg[][4],u32_t h[][5])
{
  __m128i ABCD[SHA_LANES],E[SHA_LANES],P[SHA_LANES],W[SHA_LANES][4],ABCD0[SHA_LANES],E00[SHA_LANES];

  for(int j = 0;j < SHA_LANES;j++)
  {
    ABCD[j] = ABCD0[j] = _mm_set_epi32((int)h[j][0],(int)h[j][1],(int)h[j][2],(int)h[j][3]);
    E[j] = E00[j] = _mm_set_epi32((int)h[j][4],0,0,0);
    for(int q = 0;q < 4;q++)
      W[j][q] = msg[j][q];
  }
  SHANI_GROUP( 0) SHANI_GROUP( 1) SHANI_GROUP( 2) SHANI_GROUP( 3) SHANI_GROUP( 4)
  SHANI_GROUP( 5) SHANI_GROUP( 6) SHANI_GROUP( 7) SHANI_GROUP( 8) SHANI_GROUP( 9)
  SHANI_GROUP(10) SHANI_GROUP(11) SHANI_GROUP(12) SHANI_GROUP(13) SHANI_GROUP(14)
  SHANI_GROUP(15) SHANI_GROUP(16) SHANI_GROUP(17) SHANI_GROUP(18) SHANI_GROUP(19)
  for(int j = 0;j < SHA_LANES;j++)
  {
    E[j] = _mm_sha1nexte_epu32(P[j],E00[j]); // rotate(A of round 76,30) + initial E
    ABCD[j] = _mm_add_epi32(ABCD[j],ABCD0[j]);
    h[j][0] = (u32_t)_mm_extract_epi32(ABCD[j],3);
    h[j][1] = (u32_t)_mm_extract_epi32(ABCD[j],2);
    h[j][2] = (u32_t)_mm_extract_epi32(ABCD[j],1);
    h[j][3] = (u32_t)_mm_extract_epi32(ABCD[j],0);
    h[j][4] = (u32_t)_mm_extract_epi32(E[j],3);
  }
}

# undef SHANI_GROUP

DEFINE_SHA_ENGINE(sha_ni,__m128i,SHANI_PACK,sha1_update_shani)
#endif


//
// ARMv8 SHA1 instructions (gcc/clang -march=armv8-a+crypto, or -march=native on a CPU that has them)
//
// register layout: a message register holds w0..w3 in lanes 0..3, ABCD holds A in lane 0, E is a
// scalar; each sha1c/sha1p/sha1m instruction does four rounds with the message words + constant
//

#if defined(__ARM_NEON) && (defined(__ARM_FEATURE_SHA2) || defined(__ARM_FEATURE_CRYPTO))
# include <arm_neon.h>

static inline uint32x4_t neon_pack(u32_t w0,u32_t w1,u32_t w2,u32_t w3)
{
  uint32x4_t v = { w0,w1,w2,w3 };

  return v;
}

# define NEONSHA_PACK(w0,w1,w2,w3)  neon_pack(w0,w1,w2,w3)

# define NEONSHA_K(g)  vdupq_n_u32(((g) < 5) ? 0x5A827999u : ((g) < 10) ? 0x6ED9EBA1u : ((g) < 15) ? 0x8F1BBCDCu : 0xCA62C1D6u)

# define NEONSHA_GROUP(g)                                                                         \
  for(int j = 0;j < SHA_LANES;j++)                                                                \
  {                                                                                               \
    if((g) >= 4)                                                                                  \
      W[j][(g) & 3] = vsha1su1q_u32(vsha1su0q_u32(W[j][(g) & 3],W[j][((g) + 1) & 3],W[j][((g) + 2) & 3]), \
                                    W[j][((g) + 3) & 3]);                                         \
    uint32x4_t wk = vaddq_u32(W[j][(g) & 3],NEONSHA_K(g));                                        \
    u32_t e_next = vsha1h_u32(vgetq_lane_u32(ABCD[j],0)); /* rotate(A,30): E of the next group */ \
    if((g) < 5)                                                                                   \
      ABCD[j] = vsha1cq_u32(ABCD[j],E[j],wk);                                                     \
    else if((g) < 10 || (g) >= 15)                                                                \
      ABCD[j] = vsha1pq_u32(ABCD[j],E[j],wk);                                                     \
    else                                                                                          \
      ABCD[j] = vsha1mq_u32(ABCD[j],E[j],wk);                                                     \
    E[j] = e_next;                                                                                \
  }

static inline void sha1_update_neon_sha1(const uint32x4_t msg[][4],u32_t h[][5])
{
  uint32x4_t ABCD[SHA_LANES],ABCD0[SHA_LANES],W[SHA_LANES][4];
  u32_t E[SHA_LANES];

  for(int j = 0;j < SHA_LANES;j++)
  {
    ABCD[j] = ABCD0[j] = neon_pack(h[j][0],h[j][1],h[j][2],h[j][3]);
    E[j] = h[j][4];
    for(int q = 0;q < 4;q++)
      W[j][q] = msg[j][q];
  }
  NEONSHA_GROUP( 0) NEONSHA_GROUP( 1) NEONSHA_GROUP( 2) NEONSHA_GROUP( 3) NEONSHA_GROUP( 4)
  NEONSHA_GROUP( 5) NEONSHA_GROUP( 6) NEONSHA_GROUP( 7) NEONSHA_GROUP( 8) NEONSHA_GROUP( 9)
  NEONSHA_GROUP(10) NEONSHA_GROUP(11) NEONSHA_GROUP(12) NEONSHA_GROUP(13) NEONSHA_GROUP(14)
  NEONSHA_GROUP(15) NEONSHA_GROUP(16) NEONSHA_GROUP(17) NEONSHA_GROUP(18) NEONSHA_GROUP(19)
  for(int j = 0;j < SHA_LANES;j++)
  {
    ABCD[j] = vaddq_u32(ABCD[j],ABCD0[j]);
    h[j][0] = vgetq_lane_u32(ABCD[j],0);
    h[j][1] = vgetq_lane_u32(ABCD[j],1);
    h[j][2] = vgetq_lane_u32(ABCD[j],2);
    h[j][3] = vgetq_lane_u32(ABCD[j],3);
    h[j][4] += E[j];
  }
}

# undef NEONSHA_GROUP

DEFINE_SHA_ENGINE(neon_sha1,uint32x4_t,NEONSHA_PACK,sha1_update_neon_sha1)
#endif

#endif
