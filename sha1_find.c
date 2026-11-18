//
// Arquiteturas de Alto Desempenho 2026/2027 --- assignment A1
//
// modify a PDF, PNG, PPM or ZIP file so that its SHA1 secure hash ends with (at least) 32 zero bits
//
// usage:
//   sha1_find [-e engine] [-t threads] [-z bits] input output   search and write the modified file
//   sha1_find [-e engine] [-t threads] -b seconds [-c csv] input
//                                                                benchmark (count attempts during the given time)
//   sha1_find -e engine -T n_tests input                         compare a SIMD engine with the scalar one
//                                                                (cuda/opencl: run n_tests seconds, check every GPU solution on the CPU)
//   sha1_find_opencl -L                                          list the OpenCL platforms and devices
//
// engines: scalar, avx, avx2, avx512f, neon, wasm_simd (the ones supported by the compiler flags),
//          sha_ni (Intel/AMD SHA instructions), neon_sha1 (ARMv8 SHA1 instructions),
//          cuda (only in sha1_find_cuda, compiled with -DWITH_CUDA),
//          opencl (only in sha1_find_opencl, compiled with -DWITH_OPENCL)
// threads: 1 (default), n, or 0 (all available) --- OpenMP, CPU engines only
// formats: PDF, PNG, PPM, ZIP (detected from the file contents)
//

#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "aad_data_types.h"
#include "aad_sha1_cpu.h"
#include "aad_png_crc.h"
#include "aad_ppm.h"
#include "sha1_search.h"
#include "file_formats.h"
#include "sha1_engines.h"
#include "sha1_report.h"


//
// main program
//

static void usage(const char *prog)
{
  fprintf(stderr,"usage: %s [-e engine] [-t threads] [-z bits] input output\n",prog);
  fprintf(stderr,"       %s [-e engine] [-t threads] -b seconds [-c results.csv] input\n",prog);
  fprintf(stderr,"       %s -e engine -T n_tests input\n",prog);
  list_engines(stderr);
  exit(1);
}

#ifdef __EMSCRIPTEN__
# include "sha1_wasm_main.h" // works around a broken emscripten package (see the file)
#endif

int main(int argc,char **argv)
{
  const char *engine = "scalar",*csv = NULL;
  const engine_t *e;
  double bench_seconds = 0.0;
  int n_tests = 0,zero_bits = 32,n_threads = 1;
  char label[64];
  u32_t mask;
  file_template_t t;
  result_t res;
  search_t *s;
  par_t par;
  u32_t h[5],g[5];
  int opt;

  while((opt = getopt(argc,argv,"e:b:c:z:T:t:L")) != -1)
    switch(opt)
    {
#ifdef WITH_OPENCL
      case 'L': opencl_list_devices(); return 0;
#endif
      case 'e': engine = optarg; break;
      case 'b': bench_seconds = atof(optarg); break;
      case 'c': csv = optarg; break;
      case 'z': zero_bits = atoi(optarg); break;
      case 'T': n_tests = atoi(optarg); break;
      case 't': n_threads = atoi(optarg); break;
      default: usage(argv[0]);
    }
  e = find_engine(engine);
  if(e == NULL || zero_bits < 1 || zero_bits > 32 || n_threads < 0 || argc - optind != ((bench_seconds > 0.0 || n_tests > 0) ? 1 : 2))
    usage(argv[0]);
#ifdef _OPENMP
  if(n_threads == 0)
    n_threads = omp_get_max_threads();
#else
  n_threads = 1;
#endif
  if(e->n_threads > 1)
    n_threads = 1; // the GPU engine has its own threads
  snprintf(label,sizeof(label),"%s%s",engine,(n_threads > 1) ? "+omp" : "");
  mask = (zero_bits == 32) ? 0xFFFFFFFFu : (1u << zero_bits) - 1u;
  // build the template and precompute the SHA1 state of the fixed chunks
  if(build_template(argv[optind],&t) != 0)
    return 1;
  if((s = (search_t *)malloc(sizeof(search_t))) == NULL || search_init(s,&t) != 0)
    return 1;
  printf("%s: %ld bytes, %d variable bytes at offset %ld, %d tail chunk%s (precomputed %ld chunks)\n",
         t.format,t.size,t.var_len,t.var_pos,s->n_chunks,(s->n_chunks == 1) ? "" : "s",s->tail_pos / 64);
  // sanity check: precomputed state + tail == SHA1 of the whole file
  search_store(s,&t);
  sha1_buffer(t.data,t.size,g);
  search_hash(s,h);
  if(memcmp(h,g,sizeof(h)) != 0)
  {
    fprintf(stderr,"internal error: tail hash does not match the full file hash\n");
    return 1;
  }
  // test mode
  if(n_tests > 0)
  {
    if(e->test == NULL)
    {
      fprintf(stderr,"engine %s has no test (it is the reference)\n",engine);
      return 1;
    }
    return (e->test(s,&t,n_tests) == 0) ? 0 : 1;
  }
  // search (each thread works on its own part of the search space)
  if(par_init(&par,s,n_threads) != 0)
    return 1;
  memset(&res,0,sizeof(res));
  double e0 = rapl_uj(),t0 = now();
  par_run(&par,e,(bench_seconds > 0.0) ? bench_seconds : 1.0e30,bench_seconds > 0.0,mask,&res,s);
  double e1 = rapl_uj();
  res.seconds = now() - t0;
  par_free(&par);
  print_rate(label,n_threads,&res,s->n_chunks);
  if(bench_seconds > 0.0)
  { // benchmark: append one CSV line
    const char *device = NULL;
#ifdef WITH_CUDA
    if(e->n_threads > 1)
      device = cuda_device_name; // GPU name for the cuda engine
#endif
#ifdef WITH_OPENCL
    if(e->n_threads > 1)
      device = opencl_device_name; // device name for the opencl engine
#endif
    int n_reported = n_threads * e->n_threads;
#ifdef WITH_OPENCL
    if(strcmp(engine,"opencl") == 0)
      n_reported = (int)opencl_last_global; // work-items per launch (adaptive)
#endif
    return (csv_append(csv,label,t.format,device,n_reported,e->n_lanes,s->n_chunks,&res,
                       (e0 >= 0.0 && e1 >= e0) ? e1 - e0 : -1.0) == 0) ? 0 : 1;
  }
  if(!res.found)
  {
    fprintf(stderr,"no solution found (search space exhausted)\n");
    return 1;
  }
  // verify independently and write the file
  if(save_solution(s,&t,mask,argv[optind + 1]) != 0)
    return 1;
  free(t.data);
  free(s);
  return 0;
}
