//
// Arquiteturas de Alto Desempenho 2026/2027 --- assignment A1
//
// file format modules: each one builds a file template (whole file + where the counter lives)
//
// layout rule (all formats): the variable bytes and every byte after them must fit in the last
// 64-byte SHA1 chunk, so that each attempt costs a single SHA1_UPDATE; the last chunk holds at
// most 55 file bytes (0x80 + 8-byte length are also there), so we need
//   var_len + suffix_len <= size % 64 <= 55
// when the format allows it, a few fixed filler bytes are added before the counter to reach this
//
// requires: sha1_search.h, aad_ppm.h
//

#ifndef FILE_FORMATS
#define FILE_FORMATS

#define COUNTER_LEN  16 // number of variable decimal digits (PDF and PNG): 10^16 attempts


//
// read a whole file into memory (extra bytes are allocated for insertions)
//

__attribute__((unused))
static int read_whole_file(const char *name,u08_t **data,long *size,long extra)
{
  FILE *fp = fopen(name,"rb");
  long n;

  *data = NULL;
  if(fp == NULL || fseek(fp,0l,SEEK_END) != 0 || (n = ftell(fp)) < 0 || fseek(fp,0l,SEEK_SET) != 0)
  {
    fprintf(stderr,"read_whole_file(\"%s\"): unable to read file\n",name);
    if(fp != NULL)
      fclose(fp);
    return -1;
  }
  *data = (u08_t *)malloc((size_t)(n + extra));
  if(*data == NULL || fread(*data,(size_t)1,(size_t)n,fp) != (size_t)n)
  {
    fprintf(stderr,"read_whole_file(\"%s\"): read error\n",name);
    fclose(fp);
    return -1;
  }
  fclose(fp);
  *size = n;
  return 0;
}

__attribute__((unused))
static int write_whole_file(const char *name,const u08_t *data,long size)
{
  FILE *fp = fopen(name,"wb");

  if(fp == NULL || fwrite(data,(size_t)1,(size_t)size,fp) != (size_t)size || fclose(fp) != 0)
  {
    fprintf(stderr,"write_whole_file(\"%s\"): write error\n",name);
    return -1;
  }
  return 0;
}


//
// smallest number of filler bytes f such that a file of size base_len + f has its last
// var_len + suffix_len bytes in the last SHA1 chunk; the counter end (= suffix start) is also
// placed at a multiple of align inside that chunk
//
// return value: f (0..63), or 0 if impossible (the engine then uses two tail chunks)
//

__attribute__((unused))
static int choose_filler(long base_len,int var_len,int suffix_len,int align)
{
  for(int f = 0;f < 64;f++)
  {
    int r = (int)((base_len + f) % 64);

    if(r <= 55 && r >= var_len + suffix_len && (r - suffix_len) % align == 0)
      return f;
  }
  return 0;
}


//
// PDF: insert "%<filler zeros><COUNTER_LEN digits>\n" just before the last "startxref"
//   * the comment is placed after the xref table, so no object offset changes (startxref stays valid)
//   * suffix after the counter: "\n" + "startxref\n<offset>\n%%EOF\n" (about 22 bytes)
//

__attribute__((unused))
static int pdf_template(const char *name,file_template_t *t)
{
  u08_t *in;
  long n,p,o;
  int eol,f;

  if(read_whole_file(name,&in,&n,0) != 0)
    return -1;
  for(p = n - 9;p >= 0 && memcmp(&in[p],"startxref",9) != 0;p--)
    ;
  if(p < 0)
  {
    fprintf(stderr,"pdf_template: startxref not found\n");
    free(in);
    return -1;
  }
  eol = (p > 0 && (in[p - 1] == '\n' || in[p - 1] == '\r')) ? 0 : 1; // the comment must start a line
  f = choose_filler(n + eol + 2 + COUNTER_LEN,COUNTER_LEN,(int)(n - p) + 1,1);
  t->size = n + eol + 2 + f + COUNTER_LEN;
  if((t->data = (u08_t *)malloc((size_t)t->size)) == NULL)
  {
    free(in);
    return -1;
  }
  memcpy(t->data,in,(size_t)p);
  o = p;
  if(eol)
    t->data[o++] = '\n';
  t->data[o++] = '%';
  memset(&t->data[o],'0',(size_t)(f + COUNTER_LEN));
  o += f;
  t->var_pos = o;
  o += COUNTER_LEN;
  t->data[o++] = '\n';
  memcpy(&t->data[o],&in[p],(size_t)(n - p));
  free(in);
  strcpy(t->format,"pdf");
  t->var_len = COUNTER_LEN;
  memset(t->lo,'0',sizeof(t->lo));
  memset(t->hi,'9',sizeof(t->hi));
  t->crc_from = t->crc_pos = -1;
  return 0;
}


//
// PNG: insert a tEXt chunk "Comment\0<filler zeros><COUNTER_LEN digits>" just before IEND
//   * suffix after the counter: CRC (4 bytes) + IEND chunk (12 bytes) = 16 bytes
//   * the filler also places the CRC at a word boundary of the last chunk (one store per attempt)
//

#define PNG_KEYWORD  "Comment"

__attribute__((unused))
static int png_template(const char *name,file_template_t *t)
{
  static const u08_t sig[8] = { 137,80,78,71,13,10,26,10 };
  const int kw_len = (int)strlen(PNG_KEYWORD);
  u08_t *in;
  long n,idx,iend = -1,o;
  int f,data_len;

  if(read_whole_file(name,&in,&n,0) != 0)
    return -1;
  for(idx = 8;n >= 8 && memcmp(in,sig,8) == 0 && idx + 12 <= n;idx += 12 + (long)be32(&in[idx]))
    if(memcmp(&in[idx + 4],"IEND",4) == 0)
    {
      iend = idx;
      break;
    }
  if(iend < 0 || iend + 12 != n)
  {
    fprintf(stderr,"png_template: bad PNG file (IEND must be the last chunk)\n");
    free(in);
    return -1;
  }
  // chunk = length(4) + "tEXt"(4) + keyword + '\0' + text + CRC(4)
  f = choose_filler(n + 12 + kw_len + 1 + COUNTER_LEN,COUNTER_LEN,16,4);
  data_len = kw_len + 1 + f + COUNTER_LEN;
  t->size = n + 12 + data_len;
  if((t->data = (u08_t *)malloc((size_t)t->size)) == NULL)
  {
    free(in);
    return -1;
  }
  memcpy(t->data,in,(size_t)iend);
  o = iend;
  for(int k = 0;k < 4;k++)
    t->data[o++] = (u08_t)((u32_t)data_len >> (24 - 8 * k));
  memcpy(&t->data[o],"tEXt",4);
  t->crc_from = o;
  o += 4;
  memcpy(&t->data[o],PNG_KEYWORD,(size_t)kw_len + 1); // includes the '\0' separator
  o += kw_len + 1;
  memset(&t->data[o],'0',(size_t)(f + COUNTER_LEN));
  o += f;
  t->var_pos = o;
  o += COUNTER_LEN;
  t->crc_pos = o; // filled in by the search engine
  memset(&t->data[o],0,4);
  o += 4;
  memcpy(&t->data[o],&in[iend],12);
  free(in);
  strcpy(t->format,"png");
  t->var_len = COUNTER_LEN;
  memset(t->lo,'0',sizeof(t->lo));
  memset(t->hi,'9',sizeof(t->hi));
  return 0;
}


//
// PPM (P6): change only the least significant bit of the last RGB bytes (file size unchanged)
//   * r = size % 64 bytes live in the last chunk; if 36 <= r <= 55 all of them are used
//     (2^r >= 2^36 attempts, one SHA1_UPDATE each)
//   * otherwise the last 48 bytes are used and the engine needs two tail chunks
//

__attribute__((unused))
static int ppm_template(const char *name,file_template_t *t)
{
  ppm_t ppm;
  int r;

  if(read_ppm_file(name,&ppm) != 0)
  {
    free(ppm.data);
    return -1;
  }
  // the header written by aad_ppm.h is canonical ("P6\nW H\n255\n"); warn if the original was not
  {
    u08_t *raw;
    long n;

    if(read_whole_file(name,&raw,&n,0) == 0 && (n != (long)ppm.file_size || memcmp(raw,ppm.data,(size_t)ppm.header_size) != 0))
      fprintf(stderr,"ppm_template: warning: non-canonical header, it will be rewritten\n");
    free(raw);
  }
  t->data = ppm.data; // the SHA1 trailer appended by aad_ppm.h is not needed (the engine builds its own)
  t->size = ppm.file_size;
  r = (int)(t->size % 64);
  t->var_len = (r >= 36 && r <= 55) ? r : 48;
  if(t->var_len > ppm.rgb_size)
    t->var_len = ppm.rgb_size;
  t->var_pos = t->size - t->var_len;
  for(int i = 0;i < t->var_len;i++)
  {
    t->lo[i] = t->data[t->var_pos + i] & 0xFEu;
    t->hi[i] = t->lo[i] | 0x01u;
  }
  strcpy(t->format,"ppm");
  t->crc_from = t->crc_pos = -1;
  return 0;
}


//
// ZIP (and DOCX, XLSX, ODT, JAR, EPUB, ...): the archive ends with the End Of Central Directory
// record (EOCD, 22 bytes) followed by a free comment; the comment becomes "<filler zeros><digits>"
//   * nothing comes after the counter (suffix length 0), so it always fits in the last chunk
//   * the central directory is before the EOCD, so no offset changes
//   * an existing comment is replaced
//

__attribute__((unused))
static int zip_template(const char *name,file_template_t *t)
{
  u08_t *in;
  long n,p;
  int f;

  if(read_whole_file(name,&in,&n,0) != 0)
    return -1;
  // EOCD signature "PK\5\6", its comment must end exactly at the end of the file
  for(p = n - 22;p >= 0 && p >= n - 22 - 65535;p--)
    if(in[p] == 'P' && in[p + 1] == 'K' && in[p + 2] == 5 && in[p + 3] == 6 && p + 22 + (in[p + 20] | (in[p + 21] << 8)) == n)
      break;
  if(p < 0 || p < n - 22 - 65535)
  {
    fprintf(stderr,"zip_template: end of central directory record not found\n");
    free(in);
    return -1;
  }
  f = choose_filler(p + 22 + COUNTER_LEN,COUNTER_LEN,0,1);
  t->size = p + 22 + f + COUNTER_LEN;
  if((t->data = (u08_t *)malloc((size_t)t->size)) == NULL)
  {
    free(in);
    return -1;
  }
  memcpy(t->data,in,(size_t)(p + 22));
  free(in);
  t->data[p + 20] = (u08_t)(f + COUNTER_LEN); // comment length (little-endian)
  t->data[p + 21] = 0;
  memset(&t->data[p + 22],'0',(size_t)(f + COUNTER_LEN));
  t->var_pos = p + 22 + f;
  strcpy(t->format,"zip");
  t->var_len = COUNTER_LEN;
  memset(t->lo,'0',sizeof(t->lo));
  memset(t->hi,'9',sizeof(t->hi));
  t->crc_from = t->crc_pos = -1;
  return 0;
}


//
// detect the format from the first bytes of the file and build the template
//

__attribute__((unused))
static int build_template(const char *name,file_template_t *t)
{
  u08_t magic[8] = { 0 };
  FILE *fp = fopen(name,"rb");

  memset(t,0,sizeof(*t));
  if(fp == NULL)
  {
    fprintf(stderr,"build_template: unable to open %s\n",name);
    return -1;
  }
  if(fread(magic,(size_t)1,sizeof(magic),fp) < 2)
    magic[0] = 0;
  fclose(fp);
  if(memcmp(magic,"%PDF",4) == 0)
    return pdf_template(name,t);
  if(magic[0] == 137 && memcmp(&magic[1],"PNG",3) == 0)
    return png_template(name,t);
  if(magic[0] == 'P' && magic[1] == '6')
    return ppm_template(name,t);
  if(magic[0] == 'P' && magic[1] == 'K' && ((magic[2] == 3 && magic[3] == 4) || (magic[2] == 5 && magic[3] == 6)))
    return zip_template(name,t);
  fprintf(stderr,"build_template: unknown file format (%s)\n",name);
  return -1;
}

#endif
