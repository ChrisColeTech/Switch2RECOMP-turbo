/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the GNU Affero General Public License v3.0, see LICENSE.txt and NOTICE.txt. */
#include "rt.h"
#include <sys/stat.h>
#include <unistd.h>

int plat_pread(FILE* file,void* data,u64 size,u64 offset){ return (int)pread(fileno(file),data,(size_t)size,(off_t)offset); }
u64 plat_file_size(const char* path){ struct stat info; return stat(path,&info)?0:(u64)info.st_size; }

int main(int argc,char** argv){
  if(argc!=2){ fprintf(stderr,"usage: test_nsp_runtime marker\n"); return 2; }
  Nsp* n=nsp_open_marker(argv[1]); if(!n || nsp_size(n)!=4){ fprintf(stderr,"FAIL open\n"); return 1; }
  char data[4]; if(nsp_pread(n,data,4,0)||memcmp(data,"test",4)){ fprintf(stderr,"FAIL read\n"); return 1; }
  if(!nsp_path(n)[0]){ fprintf(stderr,"FAIL path\n"); return 1; }
  nsp_close(n); puts("PASS direct NSP runtime reads"); return 0; }
