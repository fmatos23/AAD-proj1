//
// Arquiteturas de Alto Desempenho 2026/2027 --- assignment A1
//
// server/client protocol (TCP), see sha1_server.c and sha1_client.c
//
//   client -> server : msg_hello_t                       (once, after connect)
//   server -> client : msg_init_t + search_t             (once: precomputed SHA1 state + tail, ~70 KiB)
//   client -> server : msg_report_t                      (asks for work, reports the previous job)
//   server -> client : msg_job_t                         (job number, or stop)
//   client -> server : msg_report_t with progress = 1    (every second while a job runs, no reply)
//   ... report/job repeat ...
//
// a job freezes the first job_digits variable bytes (search_split(s,n_jobs,job)); the client then
// splits the rest among its threads and SIMD lanes, so no counter is ever tested twice
//
// messages are sent in the native byte order and the search_t is sent as raw bytes: the server and the
// clients must share the ABI (64-bit little-endian Linux/macOS: x86-64, ARM64)
//

#ifndef SHA1_NET
#define SHA1_NET

#include <errno.h>
#include <netdb.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#define NET_MAGIC     0x31444141u // "AAD1"
#define NET_PORT      "5123"      // default TCP port
#define JOB_TARGET    (1ull << 30) // nominal counters per job (before the thread/lane splits); small jobs
                                   // waste less work when a client leaves and make the benchmark sum more exact

typedef struct
{
  uint32_t magic;
  int32_t n_threads;   // client threads
  int32_t n_lanes;     // client SIMD lanes
  char engine[16];     // client engine
  char host[64];       // client host name
}
msg_hello_t;

typedef struct
{
  uint32_t magic;
  uint32_t mask;          // success: hash[4] & mask == 0
  uint32_t search_size;   // sizeof(search_t), ABI check
  uint32_t pad;
  uint64_t n_jobs;        // number of jobs
}
msg_init_t;

typedef struct
{
  uint64_t attempts;        // attempts done since the previous report
  uint32_t found;           // the previous job found a solution
  uint32_t exhausted;       // the previous job was completed
  uint32_t progress;        // 1: progress report only (the job is still running, no reply)
  uint32_t pad;
  u08_t var[MAX_VAR_LEN];   // variable bytes of the solution
}
msg_report_t;

typedef struct
{
  uint64_t job;    // job number
  uint32_t stop;   // 1: no more work, disconnect
  uint32_t pad;
}
msg_job_t;


//
// send/receive exactly n bytes (return 0 on success, -1 on error or disconnect)
//

static int send_all(int fd,const void *buf,size_t n)
{
  const char *p = (const char *)buf;

  while(n > 0)
  {
    ssize_t k = send(fd,p,n,0);

    if(k < 0 && errno == EINTR)
      continue;
    if(k <= 0)
      return -1;
    p += k;
    n -= (size_t)k;
  }
  return 0;
}

static int recv_all(int fd,void *buf,size_t n)
{
  char *p = (char *)buf;

  while(n > 0)
  {
    ssize_t k = recv(fd,p,n,0);

    if(k < 0 && errno == EINTR)
      continue;
    if(k <= 0)
      return -1;
    p += k;
    n -= (size_t)k;
  }
  return 0;
}

#endif
