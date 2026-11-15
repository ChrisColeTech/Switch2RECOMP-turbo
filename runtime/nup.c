/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the GNU Affero General Public License v3.0, see LICENSE.txt and NOTICE.txt.
   Forks and derived works have to keep the credit to SwitchRecomp by MZ Labs and the startup screen. */
/* Switch 2 NUP package: validated RomFS tree, raw range reads and lazy zstd decoding. */
#include "rt.h"
#include "edenvk/deps/zstd/lib/zstd.h"
#include <limits.h>

#define NUP_NONE 0xFFFFFFFFu
enum { NUP_TOC_MAX=256<<20, NUP_READ=1<<20, NUP_DIR_SIZE=24, NUP_FILE_SIZE=80, NUP_TITLE_SIZE=64 };

typedef struct { u64 a,b,stored,used; u32 name; u16 nlen; u8 type,codec; u8* cache; } NupEnt;
typedef struct { u32 source,out,depth; } DirJob;
typedef struct { u32 source; const char* name; u32 nlen; u8 type; } ChildRef;

struct Nup { FILE* file; NupEnt* ent; char* names; u32 count; size_t cache_bytes,cache_limit; u64 tick; };

static u32 rd32(const void* p){ u32 v; memcpy(&v,p,4); return v; }
static u64 rd64(const void* p){ u64 v; memcpy(&v,p,8); return v; }
static int add64(u64 a,u64 b,u64* out){ *out=a+b; return *out>=a; }

static int read_full(FILE* f,void* dst,size_t size,u64 off){
  size_t done=0; while(done<size){ size_t want=size-done; if(want>INT_MAX) want=INT_MAX;
    int got=plat_pread(f,(u8*)dst+done,want,off+done); if(got<=0) return 0; done+=(size_t)got; }
  return 1;
}

static int child_cmp(const void* av,const void* bv){ const ChildRef* a=av,*b=bv; u32 n=a->nlen<b->nlen?a->nlen:b->nlen;
  int c=memcmp(a->name,b->name,n); return c?c:(a->nlen>b->nlen)-(a->nlen<b->nlen); }

static int valid_name(const char* name,u32 n){ return n && n<=0x300 && !memchr(name,0,n) && !memchr(name,'/',n)
  && !(n==1 && name[0]=='.') && !(n==2 && name[0]=='.' && name[1]=='.'); }

Nup* nup_open(const char* path){
  FILE* f=fopen(path,"rb"); u8 head[48],*table=NULL,*seen_dir=NULL,*seen_file=NULL; DirJob* jobs=NULL; Nup* n=NULL;
  if(!f) return NULL;
  u64 file_size=plat_file_size(path);
  if(file_size<48 || !read_full(f,head,sizeof head,0) || memcmp(head,"NUP\1",4)) goto bad;
  u64 toc_at=rd32(head+8); if(toc_at>file_size-48 || !read_full(f,head,sizeof head,toc_at) || memcmp(head,"TOC\1",4)) goto bad;
  u64 nt=rd32(head+4),nd=rd64(head+8),nf=rd64(head+16),pool_at=rd64(head+24),pool_size=rd64(head+32),table_size;
  if(!nt || nt>4096 || !nd || nd>UINT32_MAX || nf>UINT32_MAX || pool_at>NUP_TOC_MAX || pool_size>NUP_TOC_MAX
    || !add64(pool_at,pool_size,&table_size) || table_size>NUP_TOC_MAX || table_size>file_size-toc_at || table_size>SIZE_MAX) goto bad;
  u64 dirs_at,files_at,children_at;
  if(!add64(48,nt*NUP_TITLE_SIZE,&dirs_at) || !add64(dirs_at,nd*NUP_DIR_SIZE,&files_at)
    || !add64(files_at,nf*NUP_FILE_SIZE,&children_at) || children_at>pool_at) goto bad;
  table=malloc((size_t)table_size); if(!table || !read_full(f,table,(size_t)table_size,toc_at)) goto bad;
  u32 root=NUP_NONE;
  for(u64 i=nt;i>0;i--){ const u8* title=table+48+(i-1)*NUP_TITLE_SIZE;
    u32 exefs=rd32(title+12),romfs=rd32(title+16); if(exefs<nd && romfs<nd){ root=romfs; break; } }
  if(root>=nd || nd+nf>UINT32_MAX || !(n=calloc(1,sizeof *n)) || !(n->ent=calloc((size_t)(nd+nf),sizeof *n->ent))
    || !(n->names=malloc(pool_size?(size_t)pool_size:1)) || !(seen_dir=calloc((size_t)nd,1))
    || (nf && !(seen_file=calloc((size_t)nf,1))) || !(jobs=malloc((size_t)nd*sizeof *jobs))) goto bad;
  n->file=f; n->count=1; n->cache_limit=256u<<20; seen_dir[root]=1; jobs[0]=(DirJob){root,0,0};
  u64 njob=1,next_job=0,names_used=0,names_cap=pool_size?pool_size:1;
  while(next_job<njob){ DirJob job=jobs[next_job++]; const u8* d=table+dirs_at+(u64)job.source*NUP_DIR_SIZE;
    u32 first=rd32(d+8),count=rd32(d+12); u64 child_count=(pool_at-children_at)/4;
    if(job.depth>64 || first>child_count || count>child_count-first) goto bad;
    ChildRef* children=count?malloc((size_t)count*sizeof *children):NULL; if(count && !children) goto bad;
    int ok=1;
    for(u32 j=0;j<count;j++){ u32 source=rd32(table+children_at+(u64)(first+j)*4); u8 type=source>=nd; u64 index=type?source-nd:source;
      if((!type && index>=nd) || (type && index>=nf)){ ok=0; break; }
      const u8* row=table+(type?files_at+index*NUP_FILE_SIZE:dirs_at+index*NUP_DIR_SIZE); u32 no=rd32(row),nl=rd32(row+4);
      if(no>pool_size || nl>pool_size-no || !valid_name((const char*)table+pool_at+no,nl)){ ok=0; break; }
      children[j]=(ChildRef){source,(const char*)table+pool_at+no,nl,type}; }
    if(!ok){ free(children); goto bad; }
    qsort(children,count,sizeof *children,child_cmp); for(u32 j=1;j<count;j++) if(!child_cmp(children+j-1,children+j)){ free(children); goto bad; }
    if(n->count>nd+nf-count){ free(children); goto bad; }
    n->ent[job.out].a=n->count; n->ent[job.out].b=count;
    for(u32 j=0;j<count;j++){ ChildRef* c=children+j; u32 out=n->count++; NupEnt* x=n->ent+out;
      if(c->nlen>UINT16_MAX || names_used>UINT32_MAX-c->nlen){ free(children); goto bad; }
      if(names_used+c->nlen>names_cap){ u64 cap=names_cap; while(cap<names_used+c->nlen) cap=cap<UINT32_MAX/2?cap*2:UINT32_MAX;
        char* grown=realloc(n->names,(size_t)cap); if(!grown){ free(children); goto bad; } n->names=grown; names_cap=cap; }
      x->name=(u32)names_used; x->nlen=(u16)c->nlen; x->type=c->type; memcpy(n->names+names_used,c->name,c->nlen); names_used+=c->nlen;
      if(!c->type){ if(seen_dir[c->source] || njob>=nd){ free(children); goto bad; }
        seen_dir[c->source]=1; jobs[njob++]=(DirJob){c->source,out,job.depth+1}; }
      else { u32 fi=c->source-(u32)nd; const u8* row=table+files_at+(u64)fi*NUP_FILE_SIZE;
        if(seen_file[fi]){ free(children); goto bad; } seen_file[fi]=1; x->b=rd64(row+8); x->a=rd64(row+16); x->stored=rd64(row+24); x->codec=row[64];
        if(x->codec>1 || (!x->codec && x->stored!=x->b) || x->b>SIZE_MAX || x->a>file_size || x->stored>file_size-x->a){ free(children); goto bad; } }
    }
    free(children);
  }
  free(jobs); free(seen_file); free(seen_dir); free(table); return n;
bad:
  free(jobs); free(seen_file); free(seen_dir); free(table);
  if(n){ free(n->names); free(n->ent); free(n); }
  fclose(f); return NULL;
}

void nup_close(Nup* n){ if(!n) return; for(u32 i=0;i<n->count;i++) free(n->ent[i].cache);
  fclose(n->file); free(n->names); free(n->ent); free(n); }

int nup_is_file(Nup* n,u32 entry){ return n && entry<n->count && n->ent[entry].type; }
const char* nup_name(Nup* n,u32 entry,u32* length){ if(!n || entry>=n->count) return NULL;
  if(length) *length=n->ent[entry].nlen;
  return n->names+n->ent[entry].name; }
int nup_children(Nup* n,u32 entry,u32* first,u32* count){ if(!n || entry>=n->count || n->ent[entry].type) return -1;
  if(first) *first=(u32)n->ent[entry].a;
  if(count) *count=(u32)n->ent[entry].b;
  return 0; }
u64 nup_file_size(Nup* n,u32 entry){ return nup_is_file(n,entry)?n->ent[entry].b:0; }

static u8* decode(Nup* n,NupEnt* x){ u8* out=malloc(x->b?(size_t)x->b:1),*in=malloc(NUP_READ); ZSTD_DCtx* dc=ZSTD_createDCtx();
  if(!out || !in || !dc){ free(out); free(in); ZSTD_freeDCtx(dc); return NULL; }
  if(ZSTD_isError(ZSTD_DCtx_setParameter(dc,ZSTD_d_windowLogMax,31))) goto bad;
  ZSTD_inBuffer input={in,0,0}; ZSTD_outBuffer output={out,(size_t)x->b,0}; u64 left=x->stored,at=x->a; size_t hint=1;
  while(hint){ if(input.pos==input.size && left){ input.size=left<NUP_READ?(size_t)left:NUP_READ; input.pos=0;
      if(!read_full(n->file,in,input.size,at)) goto bad;
      at+=input.size; left-=input.size; }
    size_t ip=input.pos,op=output.pos; hint=ZSTD_decompressStream(dc,&output,&input);
    if(ZSTD_isError(hint) || (input.pos==ip && output.pos==op) || (!left && input.pos==input.size && hint)) goto bad; }
  if(left || input.pos!=input.size || output.pos!=output.size) goto bad;
  free(in); ZSTD_freeDCtx(dc); return out;
bad: free(out); free(in); ZSTD_freeDCtx(dc); return NULL;
}

int nup_pread(Nup* n,u32 entry,void* dst,u64 size,u64 off){
  if(!nup_is_file(n,entry)) return -1;
  NupEnt* x=n->ent+entry;
  if(off>x->b || size>x->b-off) return -1;
  if(!x->codec) return read_full(n->file,dst,(size_t)size,x->a+off)?0:-1;
  u8* data=x->cache,*temporary=NULL;
  if(!data){ data=temporary=decode(n,x); if(!data) return -1;
      if(x->b<=n->cache_limit){ while(n->cache_bytes>n->cache_limit-(size_t)x->b){ u64 oldest=UINT64_MAX; u32 victim=NUP_NONE;
        for(u32 i=0;i<n->count;i++) if(n->ent[i].cache && n->ent[i].used<oldest){ oldest=n->ent[i].used; victim=i; }
        if(victim==NUP_NONE) break;
        n->cache_bytes-=(size_t)n->ent[victim].b; free(n->ent[victim].cache); n->ent[victim].cache=NULL; }
      if(n->cache_bytes<=n->cache_limit-(size_t)x->b){ x->cache=data; temporary=NULL; n->cache_bytes+=(size_t)x->b; } }
  }
  x->used=++n->tick; memcpy(dst,data+off,(size_t)size); free(temporary); return 0;
}
