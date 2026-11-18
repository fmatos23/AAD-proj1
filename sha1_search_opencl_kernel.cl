//
// Arquiteturas de Alto Desempenho 2026/2027 --- assignment A1
//
// OpenCL search kernel: a port of sha1_search_cuda_kernel.cu (same parameter array, same search
// space split, same storage area), so that the GPUs of AMD, Intel, Apple, ... can also be used
//
// the host (sha1_opencl_host.h) prepends aad_sha1.h to this file, so SHA1_UPDATE is available
//
// search space split (no overlap):
//   * the first variable bytes ("thread digits") encode g = base + global work-item number
//   * the last variable bytes ("inner digits") are enumerated by the loop index i, identically in all
//     work-items; delta[i] holds the xor mask of the words they touch (including the PNG CRC change)
//   * solutions go to storage_area (storage_area[0] = first free index, reserved with atomic_add)
//

typedef uint  u32_t;
typedef ulong u64_t;

// layout of the parameter array (must match sha1_opencl_host.h and sha1_cuda_host.h)
#define P_STATE    0  // 5 words: SHA1 state of the fixed chunks
#define P_WI0      6  // first tail word changed by the inner digits
#define P_NW       7  // number of tail words changed by the inner digits
#define P_NINNER   8  // number of inner counter values
#define P_MASK     9  // hash[4] & mask == 0 means success
#define P_NTD     10  // number of thread digits
#define P_CRC     11  // tail byte offset of the CRC (0xFFFFFFFF if none)
#define P_DELTA   12  // offset of the delta table (n_inner * nw words)
#define P_CRCTAB  13  // offset of the CRC tables of the thread digits (n_td * 256 words)
#define P_TAIL    16  // 32 words: tail chunks (all digits at their lo value)
#define P_TD      48  // 4 words per thread digit (least significant first): radix, lo, tail byte offset, unused

#define SOLUTION_WORDS 3u // g (low, high) and i


//
// one kernel per number of tail chunks K (OpenCL C has no templates)
//

#define DEFINE_SEARCH_KERNEL(K)                                                                    \
__kernel __attribute__((reqd_work_group_size(64,1,1)))                                          \
void sha1_search_kernel_ ## K(__global const u32_t * restrict p,                                   \
                                       __global volatile u32_t *storage_area,                      \
                                       u64_t base)                                                 \
{                                                                                                  \
  u32_t tw[16 * K],hh[5],st[5];                                                                    \
  u64_t g0 = base + (u64_t)get_global_id(0),g = g0;                                                \
                                                                                                   \
  /* this work-item's tail: template + thread digits */                                            \
  _Pragma("unroll")                                                                                \
  for(int idx = 0;idx < 16 * K;idx++)                                                              \
    tw[idx] = p[P_TAIL + idx];                                                                     \
  _Pragma("unroll")                                                                                \
  for(int idx = 0;idx < 5;idx++)                                                                   \
    st[idx] = p[P_STATE + idx];                                                                    \
  u32_t crc_delta = 0u;                                                                            \
  const u32_t n_td = p[P_NTD];                                                                     \
  for(u32_t q = 0u;q < n_td;q++)                                                                   \
  {                                                                                                \
    const u32_t radix = p[P_TD + 4u * q],lo = p[P_TD + 4u * q + 1u],off = p[P_TD + 4u * q + 2u];   \
    const u32_t x = lo ^ (lo + (u32_t)(g % (u64_t)radix)); /* byte change of this digit */         \
    const u32_t v = x << (24u - 8u * (off & 3u));                                                  \
                                                                                                   \
    g /= (u64_t)radix;                                                                             \
    _Pragma("unroll")                                                                              \
    for(int idx = 0;idx < 16 * K;idx++)                                                            \
      tw[idx] ^= ((u32_t)idx == (off >> 2)) ? v : 0u; /* branchless: tw[] stays in registers */   \
    if(p[P_CRC] != 0xFFFFFFFFu)                                                                    \
      crc_delta ^= p[p[P_CRCTAB] + 256u * q + x]; /* CRC linearity */                              \
  }                                                                                                \
  if(g != 0ul)                                                                                     \
    return; /* beyond the end of the search space */                                               \
  if(p[P_CRC] != 0xFFFFFFFFu)                                                                      \
  { /* the CRC may straddle two words */                                                           \
    const u32_t co = p[P_CRC],sh = 8u * (co & 3u);                                                 \
                                                                                                   \
    _Pragma("unroll")                                                                              \
    for(int idx = 0;idx < 16 * K;idx++)                                                            \
    {                                                                                              \
      tw[idx] ^= ((u32_t)idx == (co >> 2)) ? crc_delta >> sh : 0u;                               \
      tw[idx] ^= (sh != 0u && (u32_t)idx == (co >> 2) + 1u) ? crc_delta << (32u - sh) : 0u;        \
    }                                                                                              \
  }                                                                                                \
  /* inner loop: one attempt per iteration */                                                      \
  const u32_t wi0 = p[P_WI0],nw = p[P_NW],n_inner = p[P_NINNER],mask = p[P_MASK];                  \
  __global const u32_t *delta = &p[p[P_DELTA]];                                                    \
                                                                                                   \
  for(u32_t i = 0u;i < n_inner;i++,delta += nw)                                                    \
  {                                                                                                \
    _Pragma("unroll")                                                                              \
    for(int idx = 0;idx < 5;idx++)                                                                 \
      hh[idx] = st[idx];                                                                           \
    _Pragma("unroll")                                                                              \
    for(int ck = 0;ck < K;ck++)                                                                    \
    {                                                                                              \
      SHA1_UPDATE();                                                                               \
    }                                                                                              \
    if((hh[4] & mask) == 0u)                                                                       \
    { /* found one: reserve space in the shared storage area */                                    \
      u32_t sidx = atomic_add(storage_area,SOLUTION_WORDS);                                        \
                                                                                                   \
      if(sidx <= 1024u - SOLUTION_WORDS)                                                           \
      {                                                                                            \
        storage_area[sidx + 0u] = (u32_t)g0;                                                       \
        storage_area[sidx + 1u] = (u32_t)(g0 >> 32);                                               \
        storage_area[sidx + 2u] = i;                                                               \
      }                                                                                            \
    }                                                                                              \
  }                                                                                                \
}

// customization of SHA1_UPDATE (aad_sha1.h) for the kernels
#define T                u32_t
#define C(c)             (c)
#define ROTATE(x,n)      rotate((u32_t)(x),(u32_t)(n))
#define CHUNK_DATA(idx)  (tw[16 * ck + (idx)] ^ (((u32_t)(16 * ck + (idx)) - wi0 < nw) ? delta[16 * ck + (idx) - wi0] : 0u))
#define HASH(idx)        hh[idx]

DEFINE_SEARCH_KERNEL(1)
DEFINE_SEARCH_KERNEL(2)
