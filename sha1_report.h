//
// Arquiteturas de Alto Desempenho 2026/2027 --- assignment A1
//
// reporting helpers shared by sha1_find, sha1_server and sha1_find_mpi: CPU name, RAPL energy,
// CSV line, final verification and output of the modified file
//
// CSV columns (run_benchmarks.sh writes the header and appends gpu_energy_J):
//   version,format,host,device,threads,lanes,tail_chunks,seconds,attempts,attempts_per_min,
//   chunk_updates_per_min,energy_J
//

#ifndef SHA1_REPORT
#define SHA1_REPORT

__attribute__((unused))
static void cpu_name(char *name,size_t size)
{ // "model name" (x86 and some ARM kernels), else "Hardware" (Android) or "CPU part" (other ARM)
  static const char *keys[3] = { "model name","Hardware","CPU part" };
  char line[256];

  snprintf(name,size,"unknown");
  for(int k = 0;k < 3;k++)
  {
    FILE *fp = fopen("/proc/cpuinfo","r");
    int found = 0;

    while(fp != NULL && !found && fgets(line,sizeof(line),fp) != NULL)
      if(strncmp(line,keys[k],strlen(keys[k])) == 0 && strchr(line,':') != NULL)
      {
        snprintf(name,size,"%s%s",(k == 2) ? "ARM CPU part " : "",strchr(line,':') + 2);
        name[strcspn(name,"\n")] = '\0';
        found = 1;
      }
    if(fp != NULL)
      fclose(fp);
    if(found)
      return;
  }
}

// CPU package energy (RAPL, microjoules); -1 if not readable (usually root only)
__attribute__((unused))
static double rapl_uj(void)
{
  FILE *fp = fopen("/sys/class/powercap/intel-rapl:0/energy_uj","r");
  double e = -1.0;

  if(fp != NULL)
  {
    if(fscanf(fp,"%lf",&e) != 1)
      e = -1.0;
    fclose(fp);
  }
  return e;
}

__attribute__((unused))
static int trailing_zero_bits(const u32_t h[5])
{
  int n = 0;

  for(int i = 4;i >= 0;i--)
  {
    if(h[i] != 0u)
      return n + __builtin_ctz(h[i]);
    n += 32;
  }
  return n;
}

__attribute__((unused))
static void print_rate(const char *label,int n_threads,const result_t *res,int n_chunks)
{
  printf("%s (%d thread%s): %llu attempts (%llu chunk updates) in %.3f s = %.4e attempts/min, %llu solution%s\n",
         label,n_threads,(n_threads == 1) ? "" : "s",(unsigned long long)res->attempts,(unsigned long long)res->attempts * (unsigned long long)n_chunks,
         res->seconds,60.0 * (double)res->attempts / res->seconds,(unsigned long long)res->n_found,(res->n_found == 1) ? "" : "s");
}

//
// append one benchmark line (device == NULL: CPU name; energy_uj < 0: unknown)
//

__attribute__((unused))
static int csv_append(const char *csv,const char *label,const char *format,const char *device,int n_threads,int n_lanes,
                      int n_chunks,const result_t *res,double energy_uj)
{
  char cpu[256],host[64];
  FILE *fp;

  if(csv == NULL)
    return 0;
  if((fp = fopen(csv,"a")) == NULL)
  {
    fprintf(stderr,"unable to open %s\n",csv);
    return -1;
  }
  if(device != NULL && device[0] != '\0')
    snprintf(cpu,sizeof(cpu),"%s",device);
  else
    cpu_name(cpu,sizeof(cpu));
  if(gethostname(host,sizeof(host)) != 0)
    strcpy(host,"unknown");
  fprintf(fp,"%s,%s,%s,\"%s\",%d,%d,%d,%.3f,%llu,%.6e,%.6e,",label,format,host,cpu,n_threads,n_lanes,n_chunks,res->seconds,
          (unsigned long long)res->attempts,60.0 * (double)res->attempts / res->seconds,
          60.0 * (double)res->attempts * (double)n_chunks / res->seconds);
  if(energy_uj >= 0.0)
    fprintf(fp,"%.3f",energy_uj * 1.0e-6); // joules
  fprintf(fp,"\n");
  fclose(fp);
  return 0;
}

//
// copy the solution into the file, verify it with an independent full SHA1 and write the file
//

__attribute__((unused))
static int save_solution(const search_t *s,file_template_t *t,u32_t mask,const char *out_name)
{
  u32_t h[5];

  search_store(s,t);
  sha1_buffer(t->data,t->size,h);
  printf("sha1: %08x%08x%08x%08x%08x (%d trailing zero bits)\n",h[0],h[1],h[2],h[3],h[4],trailing_zero_bits(h));
  if((h[4] & mask) != 0u)
  {
    fprintf(stderr,"internal error: the solution does not verify\n");
    return -1;
  }
  return write_whole_file(out_name,t->data,t->size);
}

#endif
