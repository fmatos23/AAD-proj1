//
// Arquiteturas de Alto Desempenho 2026/2027 --- assignment A1
//
// generic search engine, shared by all file formats and all versions (scalar, SIMD, CUDA, ...)
//
// idea:
//   * a file template holds the whole file, the position of the variable bytes and the allowed
//     range [lo,hi] of each variable byte (and, for PNG, where the chunk CRC lives)
//   * the SHA1 state of every 64-byte chunk before the first variable byte is computed only once
//   * the remaining chunks (the "tail": variable bytes + fixed suffix + SHA1 padding) are kept as
//     big-endian 32-bit words, ready to be fed to SHA1_UPDATE
//   * the variable bytes form a mixed-radix counter (an odometer) updated directly in the tail words
//   * the PNG CRC is updated incrementally using the linearity of the CRC
//
// requires: <stdio.h>, <stdlib.h>, <string.h>, aad_data_types.h, aad_sha1_cpu.h, aad_png_crc.h
//
// note: uint64_t is used for 64-bit values (u64_t is only 32 bits wide on wasm32)
//

#ifndef SHA1_SEARCH
#define SHA1_SEARCH

#include <stdint.h>

#define MAX_VAR_LEN  64 // maximum number of variable bytes
#define MAX_TAIL      4 // maximum number of tail chunks (1 is the goal)


//
// file template (built by the format modules, see file_formats.h)
//

typedef struct
{
  char format[8];          // "pdf", "png", "ppm", ...
  u08_t *data;             // whole file contents
  long size;               // file size (bytes)
  long var_pos;            // file offset of the first variable byte
  int var_len;             // number of variable bytes
  u08_t lo[MAX_VAR_LEN];   // smallest value of each variable byte
  u08_t hi[MAX_VAR_LEN];   // largest value of each variable byte
  long crc_from;           // CRC32 covers data[crc_from .. crc_pos-1] (PNG only)
  long crc_pos;            // file offset of the big-endian CRC32 (-1 if there is no CRC)
}
file_template_t;


//
// search state
//

typedef struct
{
  u32_t state[5];                  // SHA1 state after all chunks that precede the tail
  u32_t w[16 * MAX_TAIL];          // tail chunks, as big-endian words, with the SHA1 padding
  int n_chunks;                    // number of tail chunks (SHA1_UPDATEs per attempt)
  long tail_pos;                   // file offset of the first tail byte
  int var_off;                     // tail offset of the first variable byte
  int var_len;                     // number of variable bytes
  int first;                       // the odometer uses variable bytes first..var_len-1 (the others are split among
                                   // clients/ranks, threads and SIMD lanes, see search_split())
  int lane_base;                   // first SIMD lane digit (-1: not yet chosen)
  u08_t lo[MAX_VAR_LEN];           // allowed range of each variable byte
  u08_t hi[MAX_VAR_LEN];
  int crc_off;                     // tail offset of the CRC32 (-1 if none)
  u32_t crc;                       // current CRC32 value
  u32_t crc_tab[MAX_VAR_LEN][256]; // crc_tab[i][x]: CRC change when variable byte i is xored with x
}
search_t;


//
// byte access inside an array of big-endian words (tail offset -> word index and shift)
//

static inline u32_t tail_get(const u32_t *w,int off)
{
  return (w[off >> 2] >> (24 - 8 * (off & 3))) & 0xFFu;
}

static inline void tail_set(u32_t *w,int off,u32_t b)
{
  int sh = 24 - 8 * (off & 3);

  w[off >> 2] = (w[off >> 2] & ~(0xFFu << sh)) | (b << sh);
}

static inline u32_t be32(const u08_t *p)
{
  return ((u32_t)p[0] << 24) | ((u32_t)p[1] << 16) | ((u32_t)p[2] << 8) | (u32_t)p[3];
}


//
// full SHA1 of a buffer (independent of the search engine, used only for verification)
//

__attribute__((unused))
static void sha1_buffer(const u08_t *data,long size,u32_t h[5])
{
  u08_t last[128];
  u32_t w[16];
  long n_full = size / 64;
  int r = (int)(size % 64),n_last = (r <= 55) ? 1 : 2;
  uint64_t n_bits = 8ull * (uint64_t)size;

  h[0] = initial_H0; h[1] = initial_H1; h[2] = initial_H2; h[3] = initial_H3; h[4] = initial_H4;
  for(long c = 0;c < n_full;c++)
  {
    for(int j = 0;j < 16;j++)
      w[j] = be32(&data[64 * c + 4 * j]);
    sha1_update(w,h);
  }
  memset(last,0,sizeof(last));
  memcpy(last,&data[64 * n_full],(size_t)r);
  last[r] = 0x80;
  for(int k = 0;k < 8;k++)
    last[64 * n_last - 1 - k] = (u08_t)(n_bits >> (8 * k));
  for(int c = 0;c < n_last;c++)
  {
    for(int j = 0;j < 16;j++)
      w[j] = be32(&last[64 * c + 4 * j]);
    sha1_update(w,h);
  }
}


//
// write the current CRC into the tail (one store when it is word-aligned)
//

static inline void search_put_crc(search_t *s)
{
  if((s->crc_off & 3) == 0)
    s->w[s->crc_off >> 2] = s->crc;
  else
    for(int k = 0;k < 4;k++)
      tail_set(s->w,s->crc_off + k,(s->crc >> (24 - 8 * k)) & 0xFFu);
}


//
// initialize the search: precompute the SHA1 state of the fixed chunks and build the tail
// the variable bytes start at their lo[] values
//
// return value: 0 (success), -1 (failure)
//

__attribute__((unused))
static int search_init(search_t *s,const file_template_t *t)
{
  long padded = (t->size + 9 + 63) / 64 * 64; // message + 0x80 + 64-bit length, rounded up
  uint64_t n_bits = 8ull * (uint64_t)t->size;
  u32_t w[16];

  memset(s,0,sizeof(*s));
  s->lane_base = -1;
  if(t->var_len < 1 || t->var_len > MAX_VAR_LEN || t->var_pos < 0 || t->var_pos + t->var_len > t->size)
  {
    fprintf(stderr,"search_init: bad variable region\n");
    return -1;
  }
  s->tail_pos = t->var_pos / 64 * 64; // the tail starts at the chunk holding the first variable byte
  s->n_chunks = (int)((padded - s->tail_pos) / 64);
  if(s->n_chunks > MAX_TAIL)
  {
    fprintf(stderr,"search_init: the variable bytes are too far from the end of the file\n");
    return -1;
  }
  // SHA1 state of the fixed chunks (computed only once)
  s->state[0] = initial_H0; s->state[1] = initial_H1; s->state[2] = initial_H2; s->state[3] = initial_H3; s->state[4] = initial_H4;
  for(long pos = 0;pos < s->tail_pos;pos += 64)
  {
    for(int j = 0;j < 16;j++)
      w[j] = be32(&t->data[pos + 4 * j]);
    sha1_update(w,s->state);
  }
  // tail data + SHA1 padding (0x80, zeros, length in bits)
  for(long i = s->tail_pos;i < t->size;i++)
    tail_set(s->w,(int)(i - s->tail_pos),t->data[i]);
  tail_set(s->w,(int)(t->size - s->tail_pos),0x80u);
  s->w[16 * s->n_chunks - 2] = (u32_t)(n_bits >> 32);
  s->w[16 * s->n_chunks - 1] = (u32_t)n_bits;
  // variable bytes (start at lo[])
  s->var_off = (int)(t->var_pos - s->tail_pos);
  s->var_len = t->var_len;
  for(int i = 0;i < t->var_len;i++)
  {
    if(t->lo[i] > t->hi[i])
    {
      fprintf(stderr,"search_init: bad range for variable byte %d\n",i);
      return -1;
    }
    s->lo[i] = t->lo[i];
    s->hi[i] = t->hi[i];
    tail_set(s->w,s->var_off + i,t->lo[i]);
  }
  // CRC (PNG): value for the initial counter + contribution tables
  s->crc_off = -1;
  if(t->crc_pos >= 0)
  {
    long n = t->crc_pos - t->crc_from;
    char *buf;

    if(t->crc_from > t->var_pos || t->var_pos + t->var_len > t->crc_pos || t->crc_pos + 4 > t->size || t->crc_pos < s->tail_pos)
    {
      fprintf(stderr,"search_init: bad CRC region\n");
      return -1;
    }
    if((buf = (char *)malloc((size_t)n)) == NULL)
      return -1;
    memcpy(buf,&t->data[t->crc_from],(size_t)n);
    for(int i = 0;i < t->var_len;i++)
      buf[t->var_pos - t->crc_from + i] = (char)t->lo[i];
    s->crc = compute_png_crc(buf,(int)n);
    free(buf);
    // the CRC is linear over GF(2): crc(M ^ D) = crc(M) ^ L(D), where L is the CRC with zero
    // initial value and no final xor; crc_tab[i][x] = L(x at variable byte i, zeros elsewhere)
    for(int i = 0;i < t->var_len;i++)
    {
      long d = t->crc_pos - (t->var_pos + i) - 1; // bytes after variable byte i
      const char zero = 0;

      for(int x = 0;x < 256;x++)
      {
        char b = (char)x;
        u32_t c = update_png_crc(0u,&b,1);

        for(long k = 0;k < d;k++)
          c = update_png_crc(c,&zero,1);
        s->crc_tab[i][x] = c;
      }
    }
    s->crc_off = (int)(t->crc_pos - s->tail_pos);
    search_put_crc(s);
  }
  return 0;
}


//
// advance the counter (variable byte var_len-1 changes fastest, bytes before first are not touched)
//
// return value: 1 (ok), 0 (wrapped around, the whole search space was visited)
//

static inline int search_next(search_t *s)
{
  for(int i = s->var_len - 1;i >= s->first;i--)
  {
    int off = s->var_off + i;
    u32_t old = tail_get(s->w,off);
    u32_t nw = (old < s->hi[i]) ? old + 1u : s->lo[i];

    tail_set(s->w,off,nw);
    if(s->crc_off >= 0)
      s->crc ^= s->crc_tab[i][old ^ nw]; // incremental CRC update
    if(nw != s->lo[i])
    { // no carry
      if(s->crc_off >= 0)
        search_put_crc(s);
      return 1;
    }
  }
  if(s->crc_off >= 0)
    search_put_crc(s);
  return 0;
}


//
// set one variable byte (keeps the CRC consistent)
//

__attribute__((unused))
static void search_set_byte(search_t *s,int i,u32_t v)
{
  u32_t old = tail_get(s->w,s->var_off + i);

  tail_set(s->w,s->var_off + i,v);
  if(s->crc_off >= 0)
  {
    s->crc ^= s->crc_tab[i][old ^ v];
    search_put_crc(s);
  }
}


//
// set the counter from an integer (mixed radix, least significant digit is the last variable byte)
// used to split the search space among threads, clients, ... (bytes before first are not touched)
//
// return value: 1 (ok), 0 (n is too large)
//

__attribute__((unused))
static int search_set_counter(search_t *s,uint64_t n)
{
  for(int i = s->var_len - 1;i >= s->first;i--)
  {
    uint64_t radix = (uint64_t)(s->hi[i] - s->lo[i]) + 1ull;
    u32_t old = tail_get(s->w,s->var_off + i);
    u32_t nw = s->lo[i] + (u32_t)(n % radix);

    n /= radix;
    tail_set(s->w,s->var_off + i,nw);
    if(s->crc_off >= 0)
      s->crc ^= s->crc_tab[i][old ^ nw];
  }
  if(s->crc_off >= 0)
    search_put_crc(s);
  return (n == 0ull) ? 1 : 0;
}


//
// split the search space: the next variable bytes (from first on) are frozen to encode part,
// 0 <= part < n_parts (mixed radix); used for MPI ranks/clients, OpenMP threads, ...
// the odometer then only uses the bytes after them, so different parts never overlap
//
// return value: number of bytes used (0 if n_parts == 1), -1 if there are not enough variable bytes
//

__attribute__((unused))
static int search_split(search_t *s,uint64_t n_parts,uint64_t part)
{
  uint64_t capacity = 1ull;
  int k;

  for(k = 0;s->first + k < s->var_len - 1 && capacity < n_parts;k++)
    capacity *= (uint64_t)(s->hi[s->first + k] - s->lo[s->first + k]) + 1ull;
  if(capacity < n_parts || part >= n_parts)
  {
    fprintf(stderr,"search_split: not enough variable bytes for %llu parts\n",(unsigned long long)n_parts);
    return -1;
  }
  for(int p = s->first + k - 1;p >= s->first;p--)
  {
    uint64_t radix = (uint64_t)(s->hi[p] - s->lo[p]) + 1ull;

    search_set_byte(s,p,s->lo[p] + (u32_t)(part % radix));
    part /= radix;
  }
  s->first += k;
  return k;
}


//
// SHA1 of the whole file for the current counter (only the tail chunks are processed)
//

static inline void search_hash(const search_t *s,u32_t h[5])
{
  h[0] = s->state[0]; h[1] = s->state[1]; h[2] = s->state[2]; h[3] = s->state[3]; h[4] = s->state[4];
  for(int c = 0;c < s->n_chunks;c++)
    sha1_update((u32_t *)&s->w[16 * c],h);
}


//
// copy the current counter (and CRC) back into the file template
//

__attribute__((unused))
static void search_store(const search_t *s,file_template_t *t)
{
  for(int i = 0;i < s->var_len;i++)
    t->data[t->var_pos + i] = (u08_t)tail_get(s->w,s->var_off + i);
  if(t->crc_pos >= 0)
    for(int k = 0;k < 4;k++)
      t->data[t->crc_pos + k] = (u08_t)(s->crc >> (24 - 8 * k));
}

#endif
