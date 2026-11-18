//
// Arquiteturas de Alto Desempenho 2026/2027 --- assignment A1
//
// server: builds the file template, precomputes the SHA1 state, hands out jobs to the clients
// (sha1_client) and verifies the solutions they report
//
// usage:
//   sha1_server [-p port] [-z bits] input output                 search and write the modified file
//   sha1_server [-p port] -b seconds [-c results.csv] input      benchmark (sum of the clients' attempts)
//
// benchmark: the clock starts when the first client connects; attempts are counted when a job is
// reported, so the work of the jobs still running at the end is not counted (slight underestimate)
//
// single-threaded event loop (poll); a job handed to a client that disconnects is handed out again
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
#include "sha1_report.h"
#include "sha1_net.h"

#define MAX_CLIENTS  64

typedef struct
{
  int fd;            // socket (-1: free slot)
  int greeted;       // hello received
  int64_t job;       // job in progress (-1: none)
  uint64_t attempts; // attempts reported by this client
  msg_hello_t hello;
}
client_t;

static client_t clients[MAX_CLIENTS];
static uint64_t requeue[MAX_CLIENTS],n_requeue,next_job,n_jobs;

static void drop_client(client_t *c)
{
  if(c->job >= 0 && n_requeue < MAX_CLIENTS)
    requeue[n_requeue++] = (uint64_t)c->job; // unfinished job: hand it out again
  close(c->fd);
  c->fd = -1;
}

static int listen_on(const char *port)
{ // an IPv6 socket that also accepts IPv4 clients; plain IPv4 if the system has no IPv6
  static const int families[2] = { AF_INET6,AF_INET };

  for(int k = 0;k < 2;k++)
  {
    struct addrinfo hints,*ai;
    int fd,one = 1,zero = 0;

    memset(&hints,0,sizeof(hints));
    hints.ai_family = families[k];
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    if(getaddrinfo(NULL,port,&hints,&ai) != 0)
      continue;
    fd = socket(ai->ai_family,ai->ai_socktype,0);
    if(fd >= 0 && families[k] == AF_INET6)
      (void)setsockopt(fd,IPPROTO_IPV6,IPV6_V6ONLY,&zero,sizeof(zero)); // dual stack (the default is not always so)
    if(fd >= 0 && setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&one,sizeof(one)) == 0 && bind(fd,ai->ai_addr,ai->ai_addrlen) == 0 && listen(fd,16) == 0)
    {
      freeaddrinfo(ai);
      return fd;
    }
    if(fd >= 0)
      close(fd);
    freeaddrinfo(ai);
  }
  perror("listen_on");
  return -1;
}

static void usage(const char *prog)
{
  fprintf(stderr,"usage: %s [-p port] [-z bits] input output\n",prog);
  fprintf(stderr,"       %s [-p port] -b seconds [-c results.csv] input\n",prog);
  exit(1);
}

int main(int argc,char **argv)
{
  const char *port = NET_PORT,*csv = NULL;
  double bench_seconds = 0.0,t0,t_print;
  int zero_bits = 32,opt,listen_fd,job_digits,done = 0,n_total_threads = 0,n_lanes = 0;
  uint64_t rest = 1ull;
  file_template_t t;
  search_t *s,*chk;
  result_t res;
  u32_t mask;

  while((opt = getopt(argc,argv,"p:z:b:c:")) != -1)
    switch(opt)
    {
      case 'p': port = optarg; break;
      case 'z': zero_bits = atoi(optarg); break;
      case 'b': bench_seconds = atof(optarg); break;
      case 'c': csv = optarg; break;
      default: usage(argv[0]);
    }
  if(zero_bits < 1 || zero_bits > 32 || argc - optind != ((bench_seconds > 0.0) ? 1 : 2))
    usage(argv[0]);
  mask = (zero_bits == 32) ? 0xFFFFFFFFu : (1u << zero_bits) - 1u;
  signal(SIGPIPE,SIG_IGN);
  // template and precomputed state
  if(build_template(argv[optind],&t) != 0)
    return 1;
  s = (search_t *)malloc(sizeof(search_t));
  chk = (search_t *)malloc(sizeof(search_t));
  if(s == NULL || chk == NULL || search_init(s,&t) != 0)
    return 1;
  // job digits: the first ones, leaving about JOB_TARGET counters per job
  for(job_digits = s->var_len - 1;job_digits > 0 && rest * (uint64_t)(s->hi[job_digits] - s->lo[job_digits] + 1) <= JOB_TARGET;job_digits--)
    rest *= (uint64_t)(s->hi[job_digits] - s->lo[job_digits] + 1);
  job_digits++;
  n_jobs = 1ull;
  for(int p = 0;p < job_digits;p++)
    n_jobs = (n_jobs > (1ull << 50)) ? (1ull << 58) : n_jobs * (uint64_t)(s->hi[p] - s->lo[p] + 1);
  printf("%s: %ld bytes, %d variable bytes, %d job digits, %llu jobs of %llu counters\n",t.format,t.size,t.var_len,job_digits,
         (unsigned long long)n_jobs,(unsigned long long)rest);
  if((listen_fd = listen_on(port)) < 0)
    return 1;
  printf("listening on port %s\n",port);
  for(int i = 0;i < MAX_CLIENTS;i++)
    clients[i].fd = -1;
  memset(&res,0,sizeof(res));
  t0 = t_print = now();
  // event loop
  while(!done)
  {
    struct pollfd pfd[MAX_CLIENTS + 1];
    int idx[MAX_CLIENTS + 1],n = 0;

    if(bench_seconds > 0.0 && n_total_threads > 0 && now() - t0 >= bench_seconds)
      break;
    if(now() - t_print >= 5.0)
    {
      t_print = now();
      printf("  %.0f s: %llu attempts (%.3e/min), %llu jobs handed out\n",t_print - t0,(unsigned long long)res.attempts,
             60.0 * (double)res.attempts / (t_print - t0),(unsigned long long)next_job);
      fflush(stdout);
    }
    pfd[n].fd = listen_fd;
    pfd[n++].events = POLLIN;
    for(int i = 0;i < MAX_CLIENTS;i++)
      if(clients[i].fd >= 0)
      {
        idx[n] = i;
        pfd[n].fd = clients[i].fd;
        pfd[n++].events = POLLIN;
      }
    if(poll(pfd,(nfds_t)n,200) <= 0)
      continue;
    if(pfd[0].revents & POLLIN)
    { // new client
      int fd = accept(listen_fd,NULL,NULL),i;

      for(i = 0;i < MAX_CLIENTS && clients[i].fd >= 0;i++)
        ;
      if(fd >= 0 && i < MAX_CLIENTS)
      {
        int one = 1;

        (void)setsockopt(fd,IPPROTO_TCP,TCP_NODELAY,&one,sizeof(one));
        memset(&clients[i],0,sizeof(client_t));
        clients[i].fd = fd;
        clients[i].job = -1;
      }
      else if(fd >= 0)
        close(fd);
    }
    for(int k = 1;k < n && !done;k++)
    {
      client_t *c = &clients[idx[k]];
      msg_report_t rep;
      msg_job_t job;

      if(!(pfd[k].revents & (POLLIN | POLLHUP | POLLERR)))
        continue;
      if(!c->greeted)
      { // hello -> init + precomputed state
        msg_init_t init = { NET_MAGIC,mask,(uint32_t)sizeof(search_t),0u,n_jobs };

        if(recv_all(c->fd,&c->hello,sizeof(c->hello)) != 0 || c->hello.magic != NET_MAGIC ||
           send_all(c->fd,&init,sizeof(init)) != 0 || send_all(c->fd,s,sizeof(search_t)) != 0)
        {
          drop_client(c);
          continue;
        }
        c->hello.engine[sizeof(c->hello.engine) - 1] = c->hello.host[sizeof(c->hello.host) - 1] = '\0';
        c->greeted = 1;
        if(n_total_threads == 0)
          t0 = t_print = now(); // the benchmark starts with the first client
        n_total_threads += c->hello.n_threads;
        if(c->hello.n_lanes > n_lanes)
          n_lanes = c->hello.n_lanes;
        printf("client %s connected (%s, %d threads)\n",c->hello.host,c->hello.engine,c->hello.n_threads);
        continue;
      }
      if(recv_all(c->fd,&rep,sizeof(rep)) != 0)
      { // disconnected
        printf("client %s disconnected\n",c->hello.host);
        drop_client(c);
        continue;
      }
      res.attempts += rep.attempts;
      c->attempts += rep.attempts;
      if(rep.progress)
        continue; // the job is still running
      // previous job
      c->job = -1;
      if(rep.found)
      { // check the claimed solution on the server
        u32_t h[5];
        int ok = 1;

        memcpy(chk,s,sizeof(search_t));
        for(int i = 0;i < s->var_len;i++)
          if(rep.var[i] < s->lo[i] || rep.var[i] > s->hi[i])
            ok = 0;
          else
            search_set_byte(chk,i,rep.var[i]);
        search_hash(chk,h);
        if(ok && (h[4] & mask) == 0u)
        {
          res.n_found++;
          printf("client %s found a solution\n",c->hello.host);
          if(bench_seconds <= 0.0)
          {
            res.found = 1;
            done = 1;
          }
        }
        else
          printf("client %s reported a bad solution!\n",c->hello.host);
      }
      // next job
      memset(&job,0,sizeof(job));
      if(done || (n_requeue == 0 && next_job >= n_jobs))
        job.stop = 1;
      else
        job.job = (n_requeue > 0) ? requeue[--n_requeue] : next_job++;
      if(send_all(c->fd,&job,sizeof(job)) != 0 || job.stop)
      {
        drop_client(c);
        continue;
      }
      c->job = (int64_t)job.job;
    }
    if(!done && n_requeue == 0 && next_job >= n_jobs)
    {
      int busy = 0;

      for(int i = 0;i < MAX_CLIENTS;i++)
        busy |= (clients[i].fd >= 0 && clients[i].job >= 0);
      if(!busy)
      {
        res.exhausted = 1;
        break;
      }
    }
  }
  res.seconds = now() - t0;
  for(int i = 0;i < MAX_CLIENTS;i++)
    if(clients[i].fd >= 0)
    {
      clients[i].job = -1;
      drop_client(&clients[i]); // the clients notice the closed connection and exit
    }
  close(listen_fd);
  print_rate("server+clients",n_total_threads,&res,s->n_chunks);
  if(bench_seconds > 0.0)
    return (csv_append(csv,"server+clients",t.format,"distributed",n_total_threads,n_lanes,s->n_chunks,&res,-1.0) == 0) ? 0 : 1;
  if(!res.found)
  {
    fprintf(stderr,"no solution found\n");
    return 1;
  }
  return (save_solution(chk,&t,mask,argv[optind + 1]) == 0) ? 0 : 1;
}
