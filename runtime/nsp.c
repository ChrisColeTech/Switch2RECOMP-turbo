/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the GNU Affero General Public License v3.0, see LICENSE.txt and NOTICE.txt.
   Forks and derived works have to keep the credit to SwitchRecomp by MZ Labs and the startup screen. */
/* standard application NSP marker: the original package and its plaintext IVFC RomFS range */
#include "rt.h"
#include <errno.h>

struct Nsp { FILE* file; u64 offset,size; char path[2048]; };

static int line(FILE* f,char* out,size_t size){
  if(!fgets(out,(int)size,f)) return 0; size_t n=strlen(out);
  while(n && (out[n-1]=='\r'||out[n-1]=='\n')) out[--n]=0;
  return n>0; }

Nsp* nsp_open_marker(const char* marker){
  FILE* m=fopen(marker,"rb"); char magic[16],path[2048],off[32],size[32],*end; Nsp* n=NULL;
  if(!m) return NULL;
  if(!line(m,magic,sizeof magic)||strcmp(magic,"SWRNSP1")||!line(m,path,sizeof path)
    ||!line(m,off,sizeof off)||!line(m,size,sizeof size)) goto done;
  errno=0; u64 offset=strtoull(off,&end,16); if(errno||*end) goto done;
  errno=0; u64 length=strtoull(size,&end,16); if(errno||*end) goto done;
  u64 total=plat_file_size(path); if(!length||offset>total||length>total-offset) goto done;
  n=calloc(1,sizeof *n); if(!n) goto done;
  n->file=fopen(path,"rb"); if(!n->file){ free(n); n=NULL; goto done; }
  n->offset=offset; n->size=length; snprintf(n->path,sizeof n->path,"%s",path);
done: fclose(m); return n; }

void nsp_close(Nsp* n){ if(!n) return; fclose(n->file); free(n); }
u64 nsp_size(Nsp* n){ return n?n->size:0; }
const char* nsp_path(Nsp* n){ return n?n->path:""; }
int nsp_pread(Nsp* n,void* dst,u64 size,u64 off){
  if(!n||off>n->size||size>n->size-off) return -1;
  size_t done=0; while(done<size){ u64 left=size-done; int want=left>0x40000000?0x40000000:(int)left;
    int got=plat_pread(n->file,(u8*)dst+done,want,n->offset+off+done); if(got<=0) return -1; done+=(size_t)got; }
  return 0; }
