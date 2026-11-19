/* Copyright 2026 ChrisColeTech
   SPDX-License-Identifier: MIT */
/* runtime/nsp.c on its own.
   usage: test_nsp_runtime <marker>                               the 4-byte "test" image of tests/run_nsp.sh
          test_nsp_runtime <marker> <expected image> [auto|portable]   every byte and unaligned ranges of the image
                                                                  (tests/test_nsp_combined.py makes both) */
#include "rt.h"
#include <stdarg.h>
#include <sys/stat.h>

void logf_(int lvl, const char* fmt, ...){ (void)lvl; va_list ap; va_start(ap,fmt); vfprintf(stderr,fmt,ap); va_end(ap); }
int plat_pread(FILE* f, void* buf, u64 size, u64 off){
#if defined(_WIN32)
  if(_fseeki64(f,(long long)off,SEEK_SET)) return -1;
#else
  if(fseeko(f,(off_t)off,SEEK_SET)) return -1;
#endif
  return (int)fread(buf,1,(size_t)size,f); }
u64 plat_file_size(const char* path){ struct stat info; return stat(path,&info)?0:(u64)info.st_size; }

int main(int argc,char** argv){
  if(argc<2){ fprintf(stderr,"usage: test_nsp_runtime marker [expected image [auto|portable]]\n"); return 2; }
  if(argc>3 && !strcmp(argv[3],"portable")) putenv("SWR_NO_AESNI=1");
  Nsp* n=nsp_open_marker(argv[1]); if(!n){ fprintf(stderr,"FAIL open\n"); return 1; }
  if(!nsp_path(n)[0]){ fprintf(stderr,"FAIL path\n"); return 1; }
  if(argc==2){
    char data[4]; if(nsp_size(n)!=4||nsp_pread(n,data,4,0)||memcmp(data,"test",4)){ fprintf(stderr,"FAIL read\n"); return 1; }
    nsp_close(n); puts("PASS direct NSP runtime reads"); return 0; }
  FILE* f=fopen(argv[2],"rb"); if(!f){ fprintf(stderr,"FAIL expected image\n"); return 1; }
  u64 size=plat_file_size(argv[2]); u8* want=malloc((size_t)size),*got=malloc((size_t)size);
  if(fread(want,1,(size_t)size,f)!=size){ fprintf(stderr,"FAIL expected image\n"); return 1; }
  fclose(f);
  if(nsp_size(n)!=size){ fprintf(stderr,"FAIL size %llu, expected %llu\n",(unsigned long long)nsp_size(n),(unsigned long long)size); return 1; }
  if(nsp_pread(n,got,size,0)||memcmp(got,want,(size_t)size)){ fprintf(stderr,"FAIL whole image\n"); return 1; }
  /* ranges that start and end inside AES blocks and cross every boundary of the tables */
  u32 seed=12345;
  for(int i=0;i<4000;i++){ seed=seed*1664525u+1013904223u; u64 off=(seed>>8)%size;
    seed=seed*1664525u+1013904223u; u64 len=(seed>>8)%(i&1? 70 : 9000); if(len>size-off) len=size-off;
    memset(got,0xEE,(size_t)len);
    if(nsp_pread(n,got,len,off)||memcmp(got,want+off,(size_t)len)){ fprintf(stderr,"FAIL range %llu +%llu\n",(unsigned long long)off,(unsigned long long)len); return 1; } }
  if(!nsp_pread(n,got,2,size-1)){ fprintf(stderr,"FAIL read past the end\n"); return 1; }
  nsp_close(n); free(want); free(got); puts("PASS in-place NSP runtime reads"); return 0; }
