//
// Arquiteturas de Alto Desempenho 2026/2027 --- assignment A1
//
// SIMD + OpenMP + MPI version
//
// usage:
//   mpirun -np N sha1_find_mpi [-e engine] [-t threads] [-z bits] input output
//   mpirun -np N sha1_find_mpi [-e engine] [-t threads] -b seconds [-c results.csv] input
//
// * rank 0 builds the template and precomputes the SHA1 state; MPI_Bcast sends it to every rank
//   (the input file only needs to exist on the node of rank 0)
// * each rank freezes its own counter digits (search_split by rank), then splits the rest among its
//   OpenMP threads and SIMD lanes
// * the ranks search in time slices; after each slice MPI_Allreduce tells whether some rank found a
//   solution (the lowest such rank wins), whether everything was visited, or whether time is up
// * the winner broadcasts its variable bytes; rank 0 verifies them and writes the file
//

#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <mpi.h>
#include "aad_data_types.h"
#include "aad_sha1_cpu.h"
#include "aad_png_crc.h"
#include "aad_ppm.h"
#include "sha1_search.h"
#include "file_formats.h"
#include "sha1_engines.h"
#include "sha1_report.h"

#define SLICE_SECONDS  0.5

static void usage(int rank,const char *prog)
{
  if(rank == 0)
  {
    fprintf(stderr,"usage: mpirun -np N %s [-e engine] [-t threads] [-z bits] input output\n",prog);
    fprintf(stderr,"       mpirun -np N %s [-e engine] [-t threads] -b seconds [-c results.csv] input\n",prog);
    list_engines(stderr);
  }
  MPI_Finalize();
  exit(1);
}

int main(int argc,char **argv)
{
  const char *engine = "avx2",*csv = NULL;
  const engine_t *e;
  double bench_seconds = 0.0,t0,e0 = -1.0;
  int rank,size,provided,opt,zero_bits = 32,n_threads = 1,winner,all_done,time_up;
  char label[64];
  u08_t var[MAX_VAR_LEN];
  file_template_t t;
  search_t *s,*base;
  result_t res,tot;
  par_t par;
  u32_t mask;

  MPI_Init_thread(&argc,&argv,MPI_THREAD_FUNNELED,&provided); // only the main thread calls MPI
  memset(&t,0,sizeof(t));
  memset(&tot,0,sizeof(tot));
  MPI_Comm_rank(MPI_COMM_WORLD,&rank);
  MPI_Comm_size(MPI_COMM_WORLD,&size);
  while((opt = getopt(argc,argv,"e:t:z:b:c:")) != -1)
    switch(opt)
    {
      case 'e': engine = optarg; break;
      case 't': n_threads = atoi(optarg); break;
      case 'z': zero_bits = atoi(optarg); break;
      case 'b': bench_seconds = atof(optarg); break;
      case 'c': csv = optarg; break;
      default: usage(rank,argv[0]);
    }
  e = find_engine(engine);
  if(e == NULL || e->n_threads > 1 || zero_bits < 1 || zero_bits > 32 || n_threads < 0 || argc - optind != ((bench_seconds > 0.0) ? 1 : 2))
    usage(rank,argv[0]);
  if(n_threads == 0)
    n_threads = omp_get_max_threads();
  mask = (zero_bits == 32) ? 0xFFFFFFFFu : (1u << zero_bits) - 1u;
  snprintf(label,sizeof(label),"%s+omp+mpi",engine);
  base = (search_t *)malloc(sizeof(search_t));
  s = (search_t *)malloc(sizeof(search_t));
  if(base == NULL || s == NULL)
    MPI_Abort(MPI_COMM_WORLD,1);
  // rank 0: template and precomputed state, then broadcast it
  if(rank == 0)
  {
    if(build_template(argv[optind],&t) != 0 || search_init(base,&t) != 0)
      MPI_Abort(MPI_COMM_WORLD,1);
    printf("%s: %ld bytes, %d variable bytes, %d ranks x %d threads x %d lanes (%s)\n",t.format,t.size,t.var_len,size,n_threads,e->n_lanes,engine);
  }
  MPI_Bcast(base,(int)sizeof(search_t),MPI_BYTE,0,MPI_COMM_WORLD);
  memcpy(s,base,sizeof(search_t));
  if(search_split(s,(uint64_t)size,(uint64_t)rank) < 0 || par_init(&par,s,n_threads) != 0)
    MPI_Abort(MPI_COMM_WORLD,1);
  // search in time slices
  memset(&res,0,sizeof(res));
  MPI_Barrier(MPI_COMM_WORLD);
  if(rank == 0)
    e0 = rapl_uj(); // energy of the node of rank 0 only
  t0 = now();
  do
  {
    int local[2],global[2];

    par_run(&par,e,SLICE_SECONDS,bench_seconds > 0.0,mask,&res,s);
    local[0] = res.found ? rank : size; // lowest rank with a solution
    local[1] = res.exhausted ? 1 : 0;
    MPI_Allreduce(&local[0],&global[0],1,MPI_INT,MPI_MIN,MPI_COMM_WORLD);
    MPI_Allreduce(&local[1],&global[1],1,MPI_INT,MPI_MIN,MPI_COMM_WORLD);
    winner = global[0];
    all_done = global[1];
    time_up = (rank == 0 && bench_seconds > 0.0 && now() - t0 >= bench_seconds);
    MPI_Bcast(&time_up,1,MPI_INT,0,MPI_COMM_WORLD); // rank 0's clock decides
  }
  while(winner == size && !all_done && !time_up);
  res.seconds = now() - t0;
  par_free(&par);
  // totals
  MPI_Reduce(&res.attempts,&tot.attempts,1,MPI_UINT64_T,MPI_SUM,0,MPI_COMM_WORLD);
  MPI_Reduce(&res.n_found,&tot.n_found,1,MPI_UINT64_T,MPI_SUM,0,MPI_COMM_WORLD);
  MPI_Reduce(&res.seconds,&tot.seconds,1,MPI_DOUBLE,MPI_MAX,0,MPI_COMM_WORLD);
  // solution: the winner sends its variable bytes
  if(winner < size)
  {
    if(rank == winner)
      for(int i = 0;i < s->var_len;i++)
        var[i] = (u08_t)tail_get(s->w,s->var_off + i);
    MPI_Bcast(var,MAX_VAR_LEN,MPI_BYTE,winner,MPI_COMM_WORLD);
  }
  if(rank == 0)
  {
    double e1 = rapl_uj();

    print_rate(label,size * n_threads,&tot,base->n_chunks);
    if(bench_seconds > 0.0)
    {
      if(csv_append(csv,label,t.format,NULL,size * n_threads,e->n_lanes,base->n_chunks,&tot,(e0 >= 0.0 && e1 >= e0) ? e1 - e0 : -1.0) != 0)
        MPI_Abort(MPI_COMM_WORLD,1);
    }
    else if(winner == size)
      fprintf(stderr,"no solution found\n");
    else
    {
      printf("rank %d found the solution\n",winner);
      for(int i = 0;i < base->var_len;i++)
        search_set_byte(base,i,var[i]);
      if(save_solution(base,&t,mask,argv[optind + 1]) != 0)
        MPI_Abort(MPI_COMM_WORLD,1);
    }
  }
  MPI_Finalize();
  return 0;
}
