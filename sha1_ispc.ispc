//
// Arquiteturas de Alto Desempenho 2026/2027 --- assignment A1
//
// Intel ISPC (Implicit SPMD Program Compiler) version of the SHA1 search step
//
// ISPC compiles "scalar looking" code for one program instance into SIMD code: programCount
// instances (the gang) run in the SIMD lanes, so here the vectorization is done by the compiler
// instead of by hand (compare with sha1_simd.h, where every lane is managed explicitly)
//
// the lane scheme is the one of sha1_simd.h: program instance j (lane j) uses the common tail
// words xored with delta[word][j] (lane digits and, for PNG, the CRC change)
//
// the SHA1 rounds come from the same SHA1_UPDATE macro (aad_sha1.h) used by all other versions
//

#include "aad_sha1.h"

#define MAX_LANES  16 // must match sha1_simd.h


//
// number of program instances (SIMD lanes) of the target chosen at compile time (--target=...)
//

export uniform int sha1_ispc_lanes()
{
  return programCount;
}


//
// one search step: each program instance computes the SHA1 of its message (n_chunks tail chunks)
//   state[5]               --- SHA1 state of the fixed chunks
//   tail[16 * n_chunks]    --- common tail words
//   delta[word][MAX_LANES] --- per-lane xor masks of each tail word (non-zero only in words wfirst..wlast)
//   mask                   --- success: (hash[4] & mask) == 0
//   hash[5][programCount]  --- output (only if not NULL, used by the tests)
// return value: bit j set if lane j found a solution
//

export uniform int64 sha1_ispc_step(uniform const uint32 state[5],uniform const uint32 tail[],uniform const uint32 delta[],
                                    uniform int n_chunks,uniform int wfirst,uniform int wlast,
                                    uniform uint32 mask,uniform uint32 hash[])
{
  uint32 h[5];

  for(uniform int i = 0;i < 5;i++)
    h[i] = state[i];
  for(uniform int ck = 0;ck < n_chunks;ck++)
  {
    uint32 m[16];

    for(uniform int i = 0;i < 16;i++)
    {
      uniform int wi = 16 * ck + i;

      m[i] = tail[wi]; // the same in all lanes...
      if(wi >= wfirst && wi <= wlast)
        m[i] ^= delta[wi * MAX_LANES + programIndex]; // ...except the words that hold lane digits/CRC (a vector load)
    }
#   define T                uint32
#   define C(c)             ((uint32)(c))
#   define ROTATE(x,n)      (((x) << (n)) | ((x) >> (32 - (n))))
#   define CHUNK_DATA(idx)  m[idx]
#   define HASH(idx)        h[idx]
    SHA1_UPDATE();
#   undef T
#   undef C
#   undef ROTATE
#   undef CHUNK_DATA
#   undef HASH
  }
  if(hash != NULL)
    for(uniform int i = 0;i < 5;i++)
      hash[i * programCount + programIndex] = h[i];
  return packmask((h[4] & mask) == 0);
}
