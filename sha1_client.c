//
// Arquiteturas de Alto Desempenho 2026/2027 --- assignment A1
//
// client: receives the precomputed search state from sha1_server, then repeatedly asks for a job,
// runs it (SIMD engine + OpenMP threads) and reports the result
//
// usage: sha1_client [-e engine] [-t threads] [host [port]]      (defaults: avx2 if available, all threads, localhost)
//

#include <time.h>
#include <poll.h>
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
#include "sha1_net.h"

static int connect_to(const char *host,const char *port)
{
  struct addrinfo hints,*ai,*p;
  int fd = -1,one = 1;

  memset(&hints,0,sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  if(getaddrinfo(host,port,&hints,&ai) != 0)
    return -1;
  for(p = ai;p != NULL;p = p->ai_next)
  {
    if((fd = socket(p->ai_family,p->ai_socktype,p->ai_protocol)) < 0)
      continue;
    if(connect(fd,p->ai_addr,p->ai_addrlen) == 0)
      break;
    close(fd);
    fd = -1;
  }
  freeaddrinfo(ai);
  if(fd >= 0)
    (void)setsockopt(fd,IPPROTO_TCP,TCP_NODELAY,&one,sizeof(one));
  return fd;
}

int main(int argc,char **argv)
{
  const char *engine = NULL;
  const char *host = "localhost",*port = NET_PORT;
  const engine_t *e;
  int n_threads = 0,opt,fd,n_jobs_done = 0;
  uint64_t total = 0ull;
  msg_hello_t hello;
  msg_init_t init;
  msg_report_t rep;
  msg_job_t job;
  search_t *base,*s;
  double t0;

  while((opt = getopt(argc,argv,"e:t:")) != -1)
    switch(opt)
    {
      case 'e': engine = optarg; break;
      case 't': n_threads = atoi(optarg); break;
      default:
        fprintf(stderr,"usage: %s [-e engine] [-t threads] [host [port]]\n",argv[0]);
        list_engines(stderr);
        return 1;
    }
  if(engine == NULL) // best available SIMD engine
    engine = (find_engine("avx2") != NULL) ? "avx2" : (find_engine("neon") != NULL) ? "neon" : "scalar";
  if(optind < argc)
    host = argv[optind++];
  if(optind < argc)
    port = argv[optind++];
  if((e = find_engine(engine)) == NULL || e->n_threads > 1)
  {
    fprintf(stderr,"bad engine %s\n",engine);
    list_engines(stderr);
    return 1;
  }
#ifdef _OPENMP
  if(n_threads <= 0)
    n_threads = omp_get_max_threads();
#else
  n_threads = 1;
#endif
  signal(SIGPIPE,SIG_IGN);
  if((fd = connect_to(host,port)) < 0)
  {
    fprintf(stderr,"unable to connect to %s:%s\n",host,port);
    return 1;
  }
  // hello -> init + precomputed state
  memset(&hello,0,sizeof(hello));
  hello.magic = NET_MAGIC;
  hello.n_threads = n_threads;
  hello.n_lanes = e->n_lanes;
  snprintf(hello.engine,sizeof(hello.engine),"%s",e->name);
  if(gethostname(hello.host,sizeof(hello.host) - 1) != 0)
    strcpy(hello.host,"unknown");
  base = (search_t *)malloc(sizeof(search_t));
  s = (search_t *)malloc(sizeof(search_t));
  if(base == NULL || s == NULL || send_all(fd,&hello,sizeof(hello)) != 0 || recv_all(fd,&init,sizeof(init)) != 0 ||
     init.magic != NET_MAGIC || init.search_size != (uint32_t)sizeof(search_t) || recv_all(fd,base,sizeof(search_t)) != 0)
  {
    fprintf(stderr,"handshake failed (different program version or ABI?)\n");
    return 1;
  }
  printf("connected to %s:%s, %s with %d threads, %llu jobs\n",host,port,e->name,n_threads,(unsigned long long)init.n_jobs);
  memset(&rep,0,sizeof(rep));
  t0 = now();
  // job loop
  for(;;)
  {
    par_t par;
    result_t res;

    if(send_all(fd,&rep,sizeof(rep)) != 0 || recv_all(fd,&job,sizeof(job)) != 0 || job.stop)
      break; // the server is done (or gone)
    memcpy(s,base,sizeof(search_t));
    if(search_split(s,init.n_jobs,job.job) < 0 || par_init(&par,s,n_threads) != 0)
      return 1;
    memset(&res,0,sizeof(res));
    uint64_t reported = 0ull;
    do
    { // 1 s slices: report progress, and stop if the server closed the connection (it found a solution)
      struct pollfd pfd = { fd,POLLIN,0 };

      par_run(&par,e,1.0,0,init.mask,&res,s);
      if(!res.found && !res.exhausted)
      {
        memset(&rep,0,sizeof(rep));
        rep.attempts = res.attempts - reported;
        rep.progress = 1u;
        reported = res.attempts;
        if(poll(&pfd,1,0) > 0 || send_all(fd,&rep,sizeof(rep)) != 0)
        {
          printf("server gone, stopping\n");
          par_free(&par);
          close(fd);
          return 0;
        }
      }
    }
    while(!res.found && !res.exhausted);
    par_free(&par);
    memset(&rep,0,sizeof(rep));
    rep.attempts = res.attempts - reported;
    rep.found = (u32_t)res.found;
    rep.exhausted = (u32_t)res.exhausted;
    if(res.found)
      for(int i = 0;i < s->var_len;i++)
        rep.var[i] = (u08_t)tail_get(s->w,s->var_off + i);
    total += res.attempts;
    n_jobs_done++;
    if(res.found)
      printf("job %llu: solution found\n",(unsigned long long)job.job);
  }
  printf("%d jobs, %llu attempts in %.1f s = %.3e attempts/min\n",n_jobs_done,(unsigned long long)total,now() - t0,
         60.0 * (double)total / (now() - t0));
  close(fd);
  return 0;
}
