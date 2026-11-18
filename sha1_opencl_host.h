//
// Arquiteturas de Alto Desempenho 2026/2027 --- assignment A1
//
// OpenCL engine (host side), see sha1_search_opencl_kernel.cl
//
// it mirrors the CUDA engine (sha1_cuda_host.h): same parameter array, same split of the search
// space, same storage area (storage_area[0] = first free index, reserved with atomic_add);
// per launch only storage_area[0] is reset (4 bytes) and read back (4 bytes); the whole 4 KiB
// storage area is copied back only when solutions were found
//
// device selection (environment variables):
//   AAD_OPENCL_PLATFORM=p  AAD_OPENCL_DEVICE=d   use device d of platform p (default: the first GPU found,
//                                                 or else the first device of the first platform)
//   AAD_OPENCL_THREADS=n                         work-items of the first launch (default 1024); afterwards the
//                                                 number is adapted so that each launch takes about 0.1-0.4 s
//                                                 (short kernels keep integrated GPUs responsive and avoid
//                                                 GPU watchdog resets; long enough to amortize the launch cost)
// run "sha1_find_opencl -L" to list the platforms and devices
//
// requires: sha1_search.h, a result_t / now() definition (see sha1_engines.h)
//

#ifndef SHA1_OPENCL_HOST
#define SHA1_OPENCL_HOST

#define CL_TARGET_OPENCL_VERSION 120
#include <CL/cl.h>

#define OCL_KERNEL_FILE   "sha1_search_opencl_kernel.cl"
#define OCL_HEADER_FILE   "aad_sha1.h"
#define OCL_INNER_TARGET  4096u      // minimum number of attempts per work-item
#define OCL_THREADS_DEF   (1u << 10) // work-items of the first launch (then adapted)

// layout of the parameter array (must match sha1_search_opencl_kernel.cl)
enum { Q_STATE = 0,Q_WI0 = 6,Q_NW,Q_NINNER,Q_MASK,Q_NTD,Q_CRC,Q_DELTA,Q_CRCTAB,Q_TAIL = 16,Q_TD = 48 };

static char opencl_device_name[256];
static size_t opencl_last_global; // work-items of the last launch (reported in the CSV file)

#define OCL_RADIX(s,p)  ((u32_t)((s)->hi[p] - (s)->lo[p]) + 1u)

#define CL_CALL(f,args)                                                                    \
  do                                                                                       \
  {                                                                                        \
    cl_int e_ = f args;                                                                    \
    if(e_ != CL_SUCCESS)                                                                   \
    {                                                                                      \
      fprintf(stderr,"%s:%d: %s returned error %d\n",__FILE__,__LINE__,#f,(int)e_);         \
      exit(1);                                                                             \
    }                                                                                      \
  }                                                                                        \
  while(0)


//
// read a whole text file (NUL terminated)
//

static char *ocl_read_text(const char *name)
{
  FILE *fp = fopen(name,"rb");
  long n;
  char *buf;

  if(fp == NULL || fseek(fp,0l,SEEK_END) != 0 || (n = ftell(fp)) < 0 || fseek(fp,0l,SEEK_SET) != 0)
  {
    fprintf(stderr,"opencl: unable to read %s (run the program in the directory where it is)\n",name);
    exit(1);
  }
  if((buf = (char *)malloc((size_t)n + 1u)) == NULL || fread(buf,(size_t)1,(size_t)n,fp) != (size_t)n)
  {
    fprintf(stderr,"opencl: read error (%s)\n",name);
    exit(1);
  }
  buf[n] = '\0';
  fclose(fp);
  return buf;
}

static int ocl_env(const char *name,int def)
{
  const char *v = getenv(name);

  return (v != NULL && v[0] != '\0') ? atoi(v) : def;
}


//
// list all platforms and devices
//

__attribute__((unused))
static void opencl_list_devices(void)
{
  cl_platform_id plat[16];
  cl_device_id dev[16];
  cl_uint n_plat = 0u,n_dev;
  char name[256];

  if(clGetPlatformIDs(16u,plat,&n_plat) != CL_SUCCESS || n_plat == 0u)
  {
    printf("no OpenCL platforms (is an OpenCL driver/ICD installed?)\n");
    return;
  }
  for(cl_uint p = 0u;p < n_plat;p++)
  {
    CL_CALL( clGetPlatformInfo , (plat[p],CL_PLATFORM_NAME,sizeof(name),name,NULL) );
    printf("platform %u: %s\n",p,name);
    if(clGetDeviceIDs(plat[p],CL_DEVICE_TYPE_ALL,16u,dev,&n_dev) != CL_SUCCESS)
      continue;
    for(cl_uint d = 0u;d < n_dev;d++)
    {
      cl_device_type type;
      cl_uint n_cu;

      CL_CALL( clGetDeviceInfo , (dev[d],CL_DEVICE_NAME,sizeof(name),name,NULL) );
      CL_CALL( clGetDeviceInfo , (dev[d],CL_DEVICE_TYPE,sizeof(type),&type,NULL) );
      CL_CALL( clGetDeviceInfo , (dev[d],CL_DEVICE_MAX_COMPUTE_UNITS,sizeof(n_cu),&n_cu,NULL) );
      printf("  device %u: %s (%s, %u compute units)\n",d,name,(type & CL_DEVICE_TYPE_GPU) ? "GPU" : (type & CL_DEVICE_TYPE_CPU) ? "CPU" : "other",n_cu);
    }
  }
}


//
// choose a device (see the environment variables above)
//

static cl_device_id opencl_pick_device(void)
{
  cl_platform_id plat[16];
  cl_device_id dev[16];
  cl_uint n_plat = 0u,n_dev = 0u;
  int want_p = ocl_env("AAD_OPENCL_PLATFORM",-1),want_d = ocl_env("AAD_OPENCL_DEVICE",-1);

  if(clGetPlatformIDs(16u,plat,&n_plat) != CL_SUCCESS || n_plat == 0u)
  {
    fprintf(stderr,"opencl: no platforms found (install an OpenCL driver, see the README)\n");
    exit(1);
  }
  if(want_p >= 0 || want_d >= 0)
  { // explicit choice
    if(want_p < 0)
      want_p = 0;
    if(want_d < 0)
      want_d = 0;
    if((cl_uint)want_p >= n_plat || clGetDeviceIDs(plat[want_p],CL_DEVICE_TYPE_ALL,16u,dev,&n_dev) != CL_SUCCESS || (cl_uint)want_d >= n_dev)
    {
      fprintf(stderr,"opencl: there is no device %d on platform %d (see sha1_find_opencl -L)\n",want_d,want_p);
      exit(1);
    }
    return dev[want_d];
  }
  for(cl_uint p = 0u;p < n_plat;p++) // first GPU
    if(clGetDeviceIDs(plat[p],CL_DEVICE_TYPE_GPU,16u,dev,&n_dev) == CL_SUCCESS && n_dev > 0u)
      return dev[0];
  for(cl_uint p = 0u;p < n_plat;p++) // anything
    if(clGetDeviceIDs(plat[p],CL_DEVICE_TYPE_ALL,16u,dev,&n_dev) == CL_SUCCESS && n_dev > 0u)
      return dev[0];
  fprintf(stderr,"opencl: no devices found\n");
  exit(1);
}


//
// put the counter of work-item g, inner index i, into the search state
//

static void opencl_decode(search_t *s,int m,uint64_t g,u32_t i)
{
  for(int p = s->var_len - 1;p >= s->var_len - m;p--)
  {
    search_set_byte(s,p,s->lo[p] + i % OCL_RADIX(s,p));
    i /= OCL_RADIX(s,p);
  }
  for(int p = s->var_len - m - 1;p >= 0;p--)
  {
    search_set_byte(s,p,s->lo[p] + (u32_t)(g % OCL_RADIX(s,p)));
    g /= OCL_RADIX(s,p);
  }
}


//
// search (verify != 0: check every reported solution on the CPU; returns the number of bad ones)
//

static int opencl_search(search_t *s,double max_seconds,int bench,u32_t mask,result_t *res,int verify)
{
  u32_t n_inner = 1u,*prm,storage[1024],n_threads;
  uint64_t n_outer = 1ull;
  cl_ulong base;
  int m,n_td,wi0,wlast,nw,n_bad = 0;
  double t0 = now();
  cl_int err;

  if(s->n_chunks > 2)
  {
    fprintf(stderr,"opencl_search: at most 2 tail chunks are supported\n");
    exit(1);
  }
  n_threads = (u32_t)ocl_env("AAD_OPENCL_THREADS",(int)OCL_THREADS_DEF);
  if(n_threads < 64u)
    n_threads = 64u;
  n_threads = (n_threads + 63u) & ~63u; // a multiple of the work-group size (64)
  // inner digits: the last m variable bytes, at least OCL_INNER_TARGET values
  for(m = 0;m < s->var_len - 1 && n_inner < OCL_INNER_TARGET;m++)
    n_inner *= OCL_RADIX(s,s->var_len - 1 - m);
  n_td = s->var_len - m;
  for(int p = 0;p < n_td;p++)
    n_outer = (n_outer > (1ull << 50)) ? (1ull << 58) : n_outer * OCL_RADIX(s,p); // capped, more than enough
  // tail words changed by the inner digits (and by the CRC)
  wi0 = (s->var_off + n_td) >> 2;
  wlast = s->var_off + s->var_len - 1;
  if(s->crc_off >= 0 && s->crc_off + 3 > wlast)
    wlast = s->crc_off + 3;
  wlast >>= 2;
  nw = wlast - wi0 + 1;
  // parameter array (exactly as in the CUDA engine)
  u32_t n_words = (u32_t)(Q_TD + 4 * n_td) + n_inner * (u32_t)nw + ((s->crc_off >= 0) ? 256u * (u32_t)n_td : 0u);
  if((prm = (u32_t *)calloc((size_t)n_words,sizeof(u32_t))) == NULL)
    exit(1);
  for(int k = 0;k < 5;k++)
    prm[Q_STATE + k] = s->state[k];
  prm[Q_WI0] = (u32_t)wi0;
  prm[Q_NW] = (u32_t)nw;
  prm[Q_NINNER] = n_inner;
  prm[Q_MASK] = mask;
  prm[Q_NTD] = (u32_t)n_td;
  prm[Q_CRC] = (s->crc_off >= 0) ? (u32_t)s->crc_off : 0xFFFFFFFFu;
  prm[Q_DELTA] = (u32_t)(Q_TD + 4 * n_td);
  prm[Q_CRCTAB] = prm[Q_DELTA] + n_inner * (u32_t)nw;
  for(int w = 0;w < 16 * s->n_chunks;w++)
    prm[Q_TAIL + w] = s->w[w];
  for(int q = 0;q < n_td;q++)
  { // least significant thread digit first
    int p = n_td - 1 - q;

    prm[Q_TD + 4 * q + 0] = OCL_RADIX(s,p);
    prm[Q_TD + 4 * q + 1] = s->lo[p];
    prm[Q_TD + 4 * q + 2] = (u32_t)(s->var_off + p);
    if(s->crc_off >= 0)
      for(int x = 0;x < 256;x++)
        prm[prm[Q_CRCTAB] + 256u * (u32_t)q + (u32_t)x] = s->crc_tab[p][x];
  }
  for(u32_t i = 0u;i < n_inner;i++)
  { // delta[i]: xor mask of words wi0..wlast for inner value i
    u32_t *d = &prm[prm[Q_DELTA] + i * (u32_t)nw],crc_delta = 0u,v = i;

    for(int p = s->var_len - 1;p >= n_td;p--)
    {
      u32_t x = s->lo[p] ^ (s->lo[p] + v % OCL_RADIX(s,p));
      int off = s->var_off + p;

      v /= OCL_RADIX(s,p);
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
  // OpenCL initialization: device, context, queue, program (aad_sha1.h + kernel source)
  cl_device_id dev = opencl_pick_device();
  CL_CALL( clGetDeviceInfo , (dev,CL_DEVICE_NAME,sizeof(opencl_device_name),opencl_device_name,NULL) );
  cl_context ctx = clCreateContext(NULL,1u,&dev,NULL,NULL,&err);
  CL_CALL( (cl_int) , (err) );
  cl_command_queue queue = clCreateCommandQueue(ctx,dev,0,&err);
  CL_CALL( (cl_int) , (err) );
  const char *src[2] = { ocl_read_text(OCL_HEADER_FILE),ocl_read_text(OCL_KERNEL_FILE) };
  cl_program prog = clCreateProgramWithSource(ctx,2u,src,NULL,&err);
  CL_CALL( (cl_int) , (err) );
  if(clBuildProgram(prog,1u,&dev,"-cl-std=CL1.2",NULL,NULL) != CL_SUCCESS)
  {
    size_t len = 0;
    char *log;

    (void)clGetProgramBuildInfo(prog,dev,CL_PROGRAM_BUILD_LOG,0,NULL,&len);
    log = (char *)malloc(len + 1u);
    (void)clGetProgramBuildInfo(prog,dev,CL_PROGRAM_BUILD_LOG,len,log,NULL);
    log[len] = '\0';
    fprintf(stderr,"opencl: build failed:\n%s\n",log);
    exit(1);
  }
  cl_kernel kernel = clCreateKernel(prog,(s->n_chunks == 1) ? "sha1_search_kernel_1" : "sha1_search_kernel_2",&err);
  CL_CALL( (cl_int) , (err) );
  cl_mem d_prm = clCreateBuffer(ctx,CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,(size_t)n_words * sizeof(u32_t),prm,&err); // the only big transfer
  CL_CALL( (cl_int) , (err) );
  cl_mem d_storage = clCreateBuffer(ctx,CL_MEM_READ_WRITE,sizeof(storage),NULL,&err);
  CL_CALL( (cl_int) , (err) );
  CL_CALL( clSetKernelArg , (kernel,0,sizeof(cl_mem),&d_prm) );
  CL_CALL( clSetKernelArg , (kernel,1,sizeof(cl_mem),&d_storage) );
  printf("opencl: %s, %u work-items in the first launch (adaptive) x %u attempts/work-item, %d thread digits, %d inner digits\n",
         opencl_device_name,n_threads,n_inner,n_td,m);
  // launches
  size_t global = (size_t)n_threads,local = 64u;
  base = 0ull;
  while(base < n_outer)
  {
    const u32_t one = 1u;
    u32_t n_used;
    double t_launch = now();

    CL_CALL( clEnqueueWriteBuffer , (queue,d_storage,CL_FALSE,0,sizeof(u32_t),&one,0,NULL,NULL) ); // storage_area[0] = 1
    CL_CALL( clSetKernelArg , (kernel,2,sizeof(cl_ulong),&base) );
    CL_CALL( clEnqueueNDRangeKernel , (queue,kernel,1u,NULL,&global,&local,0,NULL,NULL) );
    CL_CALL( clEnqueueReadBuffer , (queue,d_storage,CL_TRUE,0,sizeof(u32_t),&n_used,0,NULL,NULL) );
    res->attempts += (uint64_t)n_inner * ((n_outer - base < global) ? n_outer - base : global);
    base += global; // the next launch starts where this one ended
    if(n_used > 1u)
    {
      u32_t n_sol = (n_used - 1u) / 3u,n_stored = (n_used > 1024u) ? 341u : n_sol;

      CL_CALL( clEnqueueReadBuffer , (queue,d_storage,CL_TRUE,0,sizeof(storage),storage,0,NULL,NULL) );
      res->n_found += n_sol;
      for(u32_t k = 0u;k < n_stored && (verify || !bench);k++)
      {
        u32_t h[5];

        opencl_decode(s,m,(uint64_t)storage[1 + 3 * k] | ((uint64_t)storage[2 + 3 * k] << 32),storage[3 + 3 * k]);
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
    opencl_last_global = global;
    if((res->seconds = now() - t0) >= max_seconds)
      break;
    // adapt the size of the next launch (it stays a multiple of the work-group size, 64)
    t_launch = now() - t_launch;
    if(t_launch < 0.1 && global < ((size_t)1 << 24))
      global *= 2u;
    else if(t_launch > 0.4 && global > (size_t)256)
      global /= 2u;
  }
  if(base >= n_outer)
    res->exhausted = 1;
  res->seconds = now() - t0;
  clReleaseMemObject(d_storage);
  clReleaseMemObject(d_prm);
  clReleaseKernel(kernel);
  clReleaseProgram(prog);
  clReleaseCommandQueue(queue);
  clReleaseContext(ctx);
  free((void *)src[0]);
  free((void *)src[1]);
  free(prm);
  return n_bad;
}

static void run_opencl(search_t *s,double max_seconds,int bench,u32_t mask,result_t *res)
{
  (void)opencl_search(s,max_seconds,bench,mask,res,0);
}


//
// test: every solution of a weaker condition (22 zero bits) reported by the device is checked on the CPU
//

static int test_opencl(search_t *s,file_template_t *t,int n_tests)
{
  result_t res;
  int n_bad;

  (void)t;
  memset(&res,0,sizeof(res));
  n_bad = opencl_search(s,(double)n_tests,1,(1u << 22) - 1u,&res,1);
  printf("test_opencl: %llu attempts, %llu solutions with 22 zero bits (expected about %.0f), %d bad\n",
         (unsigned long long)res.attempts,(unsigned long long)res.n_found,(double)res.attempts / 4194304.0,n_bad);
  return (n_bad == 0 && res.n_found > 0ull) ? 0 : -1;
}

#endif
