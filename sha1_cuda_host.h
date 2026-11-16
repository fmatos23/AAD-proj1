//
// Arquiteturas de Alto Desempenho 2026/2027 --- assignment A1
//
// CUDA engine (host side), see sha1_search_cuda_kernel.cu
//
// transfers: the parameter array (state, tail, delta and CRC tables) is copied to the device once;
// per launch only storage_area[0] is reset on the device (cuMemsetD32, no copy) and read back
// (4 bytes); the whole 4 KiB storage area is copied back only when solutions were found
//
// requires: sha1_search.h, a result_t / now() definition (see sha1_find.c)
//

#ifndef SHA1_CUDA_HOST
#define SHA1_CUDA_HOST

#include "aad_cuda_utilities.h"

#ifndef CUDA_THREADS
# define CUDA_THREADS  (1u << 16) // threads per launch
#endif
#define INNER_TARGET   4096u      // minimum number of attempts per thread
#define CUBIN_NAME     "sha1_search_cuda_kernel.cubin"

// layout of the parameter array (must match sha1_search_cuda_kernel.cu)
enum { P_STATE = 0,P_WI0 = 6,P_NW,P_NINNER,P_MASK,P_NTD,P_CRC,P_DELTA,P_CRCTAB,P_TAIL = 16,P_TD = 48 };

static char cuda_device_name[256];

#define RADIX(s,p)  ((u32_t)((s)->hi[p] - (s)->lo[p]) + 1u)


//
// put the counter of thread g, inner index i, into the search state
//

static void cuda_decode(search_t *s,int m,u64_t g,u32_t i)
{
  for(int p = s->var_len - 1;p >= s->var_len - m;p--)
  {
    search_set_byte(s,p,s->lo[p] + i % RADIX(s,p));
    i /= RADIX(s,p);
  }
  for(int p = s->var_len - m - 1;p >= 0;p--)
  {
    search_set_byte(s,p,s->lo[p] + (u32_t)(g % RADIX(s,p)));
    g /= RADIX(s,p);
  }
}


//
// search (verify != 0: check every reported solution on the CPU; returns the number of bad ones)
//

static int cuda_search(search_t *s,double max_seconds,int bench,u32_t mask,result_t *res,int verify)
{
  u32_t n_inner = 1u,*prm,*storage;
  u64_t n_outer = 1ul,base;
  int m,n_td,wi0,wlast,nw,n_bad = 0;
  double t0 = now();
  cuda_data_t cd;

  if(s->n_chunks > 2)
  {
    fprintf(stderr,"cuda_search: at most 2 tail chunks are supported\n");
    exit(1);
  }
  // inner digits: the last m variable bytes, at least INNER_TARGET values
  for(m = 0;m < s->var_len - 1 && n_inner < INNER_TARGET;m++)
    n_inner *= RADIX(s,s->var_len - 1 - m);
  n_td = s->var_len - m;
  for(int p = 0;p < n_td;p++)
    n_outer = (n_outer > (1ul << 50)) ? (1ul << 58) : n_outer * RADIX(s,p); // capped, more than enough
  // tail words changed by the inner digits (and by the CRC)
  wi0 = (s->var_off + n_td) >> 2;
  wlast = s->var_off + s->var_len - 1;
  if(s->crc_off >= 0 && s->crc_off + 3 > wlast)
    wlast = s->crc_off + 3;
  wlast >>= 2;
  nw = wlast - wi0 + 1;
  // CUDA initialization
  memset(&cd,0,sizeof(cd));
  cd.device_number = 0;
  cd.cubin_file_name = CUBIN_NAME;
  cd.kernel_name = (s->n_chunks == 1) ? "sha1_search_kernel_1" : "sha1_search_kernel_2";
  u32_t n_words = (u32_t)(P_TD + 4 * n_td) + n_inner * (u32_t)nw + ((s->crc_off >= 0) ? 256u * (u32_t)n_td : 0u);
  cd.data_size[0] = n_words * (u32_t)sizeof(u32_t);
  cd.data_size[1] = 1024u * (u32_t)sizeof(u32_t);
  initialize_cuda(&cd);
  snprintf(cuda_device_name,sizeof(cuda_device_name),"%s",cd.device_name);
  // parameter array
  prm = (u32_t *)cd.host_data[0];
  storage = (u32_t *)cd.host_data[1];
  memset(prm,0,cd.data_size[0]);
  for(int k = 0;k < 5;k++)
    prm[P_STATE + k] = s->state[k];
  prm[P_WI0] = (u32_t)wi0;
  prm[P_NW] = (u32_t)nw;
  prm[P_NINNER] = n_inner;
  prm[P_MASK] = mask;
  prm[P_NTD] = (u32_t)n_td;
  prm[P_CRC] = (s->crc_off >= 0) ? (u32_t)s->crc_off : 0xFFFFFFFFu;
  prm[P_DELTA] = (u32_t)(P_TD + 4 * n_td);
  prm[P_CRCTAB] = prm[P_DELTA] + n_inner * (u32_t)nw;
  for(int w = 0;w < 16 * s->n_chunks;w++)
    prm[P_TAIL + w] = s->w[w];
  for(int q = 0;q < n_td;q++)
  { // least significant thread digit first
    int p = n_td - 1 - q;

    prm[P_TD + 4 * q + 0] = RADIX(s,p);
    prm[P_TD + 4 * q + 1] = s->lo[p];
    prm[P_TD + 4 * q + 2] = (u32_t)(s->var_off + p);
    if(s->crc_off >= 0)
      for(int x = 0;x < 256;x++)
        prm[prm[P_CRCTAB] + 256u * (u32_t)q + (u32_t)x] = s->crc_tab[p][x];
  }
  for(u32_t i = 0u;i < n_inner;i++)
  { // delta[i]: xor mask of words wi0..wlast for inner value i
    u32_t *d = &prm[prm[P_DELTA] + i * (u32_t)nw],crc_delta = 0u,v = i;

    for(int p = s->var_len - 1;p >= n_td;p--)
    {
      u32_t x = s->lo[p] ^ (s->lo[p] + v % RADIX(s,p));
      int off = s->var_off + p;

      v /= RADIX(s,p);
      d[(off >> 2) - wi0] ^= x << (24 - 8 * (off & 3));
      if(s->crc_off >= 0)
        crc_delta ^= s->crc_tab[p][x];
    }
    if(s->crc_off >= 0)
      for(int b = 0;b < 4;b++)
      {
        int off = s->crc_off + b;

        d[(off >> 2) - wi0] ^= ((crc_delta >> (24 - 8 * b)) & 0xFFu) << (24 - 8 * (off & 3));
      }
  }
  host_to_device_copy(&cd,0); // the only big transfer
  printf("cuda: %u threads/launch x %u attempts/thread, %d thread digits, %d inner digits\n",CUDA_THREADS,n_inner,n_td,m);
  // launches
  cd.grid_dim_x = CUDA_THREADS / RECOMENDED_CUDA_BLOCK_SIZE;
  cd.block_dim_x = RECOMENDED_CUDA_BLOCK_SIZE;
  cd.n_kernel_arguments = 3;
  cd.arg[0] = &cd.device_data[0];
  cd.arg[1] = &cd.device_data[1];
  cd.arg[2] = &base;
  for(base = 0ul;base < n_outer;base += CUDA_THREADS)
  {
    u32_t n_used;

    CU_CALL( cuMemsetD32 , (cd.device_data[1],1u,(size_t)1) ); // storage_area[0] = 1
    launch_kernel(&cd);
    CU_CALL( cuMemcpyDtoH , ((void *)storage,cd.device_data[1],sizeof(u32_t)) );
    res->attempts += (u64_t)n_inner * ((n_outer - base < CUDA_THREADS) ? n_outer - base : CUDA_THREADS);
    if((n_used = storage[0]) > 1u)
    {
      u32_t n_sol = (n_used - 1u) / 3u,n_stored = (n_used > 1024u) ? 341u : n_sol;

      device_to_host_copy(&cd,1);
      res->n_found += n_sol;
      for(u32_t k = 0u;k < n_stored && (verify || !bench);k++)
      {
        u32_t h[5];

        cuda_decode(s,m,(u64_t)storage[1 + 3 * k] | ((u64_t)storage[2 + 3 * k] << 32),storage[3 + 3 * k]);
        search_hash(s,h);
        if((h[4] & mask) != 0u)
          n_bad++;
        if(!verify)
        {
          res->found = 1;
          break;
        }
      }
      if(res->found)
        break;
    }
    if((res->seconds = now() - t0) >= max_seconds)
      break;
  }
  if(base >= n_outer)
    res->exhausted = 1;
  res->seconds = now() - t0;
  terminate_cuda(&cd);
  return n_bad;
}

static void run_cuda(search_t *s,double max_seconds,int bench,u32_t mask,result_t *res)
{
  (void)cuda_search(s,max_seconds,bench,mask,res,0);
}


//
// test: every solution of a weaker condition (22 zero bits) reported by the GPU is checked on the CPU
//

static int test_cuda(search_t *s,file_template_t *t,int n_tests)
{
  result_t res;
  int n_bad;

  (void)t;
  memset(&res,0,sizeof(res));
  n_bad = cuda_search(s,(double)n_tests,1,(1u << 22) - 1u,&res,1);
  printf("test_cuda: %llu attempts, %llu solutions with 22 zero bits (expected about %.0f), %d bad\n",
         (unsigned long long)res.attempts,(unsigned long long)res.n_found,(double)res.attempts / 4194304.0,n_bad);
  return (n_bad == 0 && res.n_found > 0ul) ? 0 : -1;
}

#endif
