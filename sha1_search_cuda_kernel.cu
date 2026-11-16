//
// Arquiteturas de Alto Desempenho 2026/2027 --- assignment A1
//
// CUDA search kernel: each thread tests n_inner counters
//
// search space split (no overlap):
//   * the first variable bytes ("thread digits") encode g = base + global thread number (mixed radix);
//     base is a kernel argument, advanced by the host after each launch
//   * the last variable bytes ("inner digits") are enumerated by the loop index i, identically in all
//     threads; delta[i] holds the xor mask of the words they touch (including the PNG CRC change),
//     so all threads of a warp read the same address (broadcast)
//   * solutions go to storage_area (storage_area[0] = first free index, reserved with atomicAdd)
//

#include "aad_sha1.h"

typedef unsigned int u32_t;
typedef unsigned long long u64_t;

// layout of the parameter array (must match sha1_cuda_host.h)
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

template <int K>
__device__ __forceinline__ void search(const u32_t * __restrict__ p,u32_t *storage_area,u64_t base)
{
  u32_t tw[16 * K],hh[5],st[5];
  u64_t g0 = base + (u64_t)threadIdx.x + (u64_t)blockDim.x * (u64_t)blockIdx.x,g = g0;

  // this thread's tail: template + thread digits (fully unrolled loops keep tw[] in registers)
# pragma unroll
  for(int idx = 0;idx < 16 * K;idx++)
    tw[idx] = p[P_TAIL + idx];
# pragma unroll
  for(int idx = 0;idx < 5;idx++)
    st[idx] = p[P_STATE + idx];
  u32_t crc_delta = 0u;
  const u32_t n_td = p[P_NTD];
  for(u32_t q = 0u;q < n_td;q++)
  {
    const u32_t radix = p[P_TD + 4u * q],lo = p[P_TD + 4u * q + 1u],off = p[P_TD + 4u * q + 2u];
    const u32_t x = lo ^ (lo + (u32_t)(g % radix)); // byte change of this digit
    const u32_t v = x << (24u - 8u * (off & 3u));

    g /= radix;
#   pragma unroll
    for(int idx = 0;idx < 16 * K;idx++)
      if((u32_t)idx == (off >> 2))
        tw[idx] ^= v;
    if(p[P_CRC] != 0xFFFFFFFFu)
      crc_delta ^= p[p[P_CRCTAB] + 256u * q + x]; // CRC linearity
  }
  if(g != 0ul)
    return; // beyond the end of the search space
  if(p[P_CRC] != 0xFFFFFFFFu)
  { // the CRC may straddle two words
    const u32_t co = p[P_CRC],sh = 8u * (co & 3u);

#   pragma unroll
    for(int idx = 0;idx < 16 * K;idx++)
    {
      if((u32_t)idx == (co >> 2))
        tw[idx] ^= crc_delta >> sh;
      if(sh != 0u && (u32_t)idx == (co >> 2) + 1u)
        tw[idx] ^= crc_delta << (32u - sh);
    }
  }
  // inner loop: one attempt per iteration
  const u32_t wi0 = p[P_WI0],nw = p[P_NW],n_inner = p[P_NINNER],mask = p[P_MASK];
  const u32_t *delta = &p[p[P_DELTA]];

  for(u32_t i = 0u;i < n_inner;i++,delta += nw)
  {
#   pragma unroll
    for(int idx = 0;idx < 5;idx++)
      hh[idx] = st[idx];
#   pragma unroll
    for(int ck = 0;ck < K;ck++)
    {
#     define T                u32_t
#     define C(c)             (c)
#     define ROTATE(x,n)      (((x) << (n)) | ((x) >> (32 - (n))))
#     define CHUNK_DATA(idx)  (tw[16 * ck + (idx)] ^ (((u32_t)(16 * ck + (idx)) - wi0 < nw) ? delta[16 * ck + (idx) - wi0] : 0u))
#     define HASH(idx)        hh[idx]
      SHA1_UPDATE();
#     undef T
#     undef C
#     undef ROTATE
#     undef CHUNK_DATA
#     undef HASH
    }
    if((hh[4] & mask) == 0u)
    { // found one: reserve space in the shared storage area
      u32_t idx = atomicAdd(storage_area,SOLUTION_WORDS);

      if(idx <= 1024u - SOLUTION_WORDS)
      {
        storage_area[idx + 0u] = (u32_t)g0;
        storage_area[idx + 1u] = (u32_t)(g0 >> 32);
        storage_area[idx + 2u] = i;
      }
    }
  }
}

extern "C" __global__ __launch_bounds__(RECOMENDED_CUDA_BLOCK_SIZE,1)
void sha1_search_kernel_1(const u32_t *params,u32_t *storage_area,u64_t base)
{
  search<1>(params,storage_area,base);
}

extern "C" __global__ __launch_bounds__(RECOMENDED_CUDA_BLOCK_SIZE,1)
void sha1_search_kernel_2(const u32_t *params,u32_t *storage_area,u64_t base)
{
  search<2>(params,storage_area,base);
}
