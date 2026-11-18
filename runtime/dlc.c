/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the GNU Affero General Public License v3.0, see LICENSE.txt and NOTICE.txt.
   Forks and derived works have to keep the credit to SwitchRecomp by MZ Labs and the startup screen. */
/* add-on content (DLC). A game sees DLC as two things: aoc:u says which add-on titles are installed, and fsp-srv
   OpenDataStorageByDataId hands out each title's own RomFS (it is never layered over the game's rom:).
   Both come from <data>/dlc.manifest, written by switchrecomp.py --install-dlc (dlc.py), one title a line:

     <title id, 16 hex> <version> <data|stub> <the package it came from>

   and the title's RomFS in <data>/dlc/<title id>/, looked for in this order:
     romfs.nsp   a marker (the SWRNSP1 format of the game's own romfs.nsp): a plaintext RomFS read in place
     romfs.bin   a RomFS image (decrypted, or unpacked from an NSZ / XCZ, when the package was installed)
     romfs/      a folder of files
   A title listed without any of them is not offered to the game (its package was moved, or a drive is offline): a
   game that is told about DLC it then cannot read can hang (Mario Kart 8 Deluxe does, on the loading screen).
   A line marked stub is the exception, for diagnostics: that title is listed and gets an empty RomFS.
   No manifest means no DLC, as before. */
#include "rt.h"
#include <ctype.h>
#include <errno.h>

#define DLC_MAX 256
typedef struct { u64 id; u32 version; int stub; char source[1024]; } DlcEnt;

static DlcEnt g_dlc[DLC_MAX]; static int g_ndlc, g_dlc_loaded;

static int image_ok(FILE* f, u64 size);

/* does <folder of the manifest>/dlc/<title id>/ hold a RomFS the runtime can serve? */
static int dlc_has_data(const char* manifest, u64 id){
  char dir[1200], p[1400]; snprintf(dir,sizeof dir,"%s",manifest);
  char* cut=dir+strlen(dir); while(cut>dir && cut[-1]!='/' && cut[-1]!='\\') cut--;
  *cut=0;
  snprintf(p,sizeof p,"%sdlc/%016llx/romfs.nsp",dir,(unsigned long long)id);
  Nsp* n=nsp_open_marker(p); if(n){ nsp_close(n); return 1; }
  snprintf(p,sizeof p,"%sdlc/%016llx/romfs.bin",dir,(unsigned long long)id);
  FILE* f=fopen(p,"rb"); if(f){ int ok=image_ok(f,plat_file_size(p)); fclose(f); if(ok) return 1; }
  snprintf(p,sizeof p,"%sdlc/%016llx/romfs",dir,(unsigned long long)id);
  return plat_is_dir(p); }

/* 0100152000022000 owns 0100152000023000..37ff: bit 12 set, the low 11 bits are the add-on content index */
static int dlc_owned(u64 id, u64 app){ return (id&~0x1FFFull)==(app&~0x1FFFull) && (id&0x1000); }

static int dlc_cmp(const void* a, const void* b){ const DlcEnt* x=a,*y=b; return x->id<y->id? -1 : x->id>y->id; }

int dlc_load(const char* manifest){
  g_ndlc=0; g_dlc_loaded=1;
  FILE* f=fopen(manifest,"rb"); if(!f) return 0;
  char line[2048];
  while(fgets(line,sizeof line,f)){
    char* p=line; while(*p==' '||*p=='\t') p++;
    size_t n=strlen(p); while(n && isspace((unsigned char)p[n-1])) p[--n]=0;
    if(!*p || *p=='#') continue;
    char* end; errno=0; u64 id=strtoull(p,&end,16);
    if(errno || end-p!=16 || (*end!=' ' && *end!='\t')){ WARN("dlc: manifest line ignored: %s\n",p); continue; }
    p=end; errno=0; unsigned long version=strtoul(p,&end,10);
    if(errno || end==p || (*end && *end!=' ' && *end!='\t')){ WARN("dlc: manifest line ignored (version): %016llx\n",(unsigned long long)id); continue; }
    p=end; while(*p==' '||*p=='\t') p++;
    int stub=0;   /* the storage word is optional: a line can go straight to the package path */
    if(!strncmp(p,"stub",4) && (!p[4]||p[4]==' '||p[4]=='\t')){ stub=1; p+=4; }
    else if(!strncmp(p,"data",4) && (!p[4]||p[4]==' '||p[4]=='\t')) p+=4;
    while(*p==' '||*p=='\t') p++;
    int dup=0; for(int i=0;i<g_ndlc;i++) if(g_dlc[i].id==id) dup=1;
    if(dup || g_ndlc==DLC_MAX) continue;
    if(!stub && !dlc_has_data(manifest,id)){
      WARN("dlc: %016llx is installed but its data is missing (%s): not offered to the game. Install it again.\n",(unsigned long long)id,p);
      continue; }
    DlcEnt* e=&g_dlc[g_ndlc++]; e->id=id; e->version=(u32)version; e->stub=stub; snprintf(e->source,sizeof e->source,"%s",p); }
  fclose(f);
  /* by title id, so a game always indexes its add-on content in the same order */
  qsort(g_dlc,g_ndlc,sizeof(DlcEnt),dlc_cmp);
  return g_ndlc; }

static void dlc_ensure(void){
  if(g_dlc_loaded) return;
  char p[1200]; snprintf(p,sizeof p,"%sdlc.manifest",g_datadir);
  if(dlc_load(p)) LOG("dlc: %d add-on content title%s in %s\n",g_ndlc,g_ndlc==1?"":"s",p); }

int dlc_count(u64 app){ dlc_ensure(); int n=0; for(int i=0;i<g_ndlc;i++) n+=dlc_owned(g_dlc[i].id,app); return n; }

/* the add-on content indexes (title id & 0x7ff) of app's titles from the offset-th on; -1 when offset is past the end */
int dlc_list(u64 app, u32 offset, u32 max, u32* out){
  dlc_ensure(); u32 k=0, n=0;
  for(int i=0;i<g_ndlc;i++){ if(!dlc_owned(g_dlc[i].id,app)) continue;
    if(k++>=offset && n<max) out[n++]=(u32)(g_dlc[i].id&0x7FF); }
  return offset>k? -1 : (int)n; }

/* 1 when the manifest lists this title (version and stub are optional outputs) */
int dlc_find(u64 id, u32* version, int* stub){
  dlc_ensure();
  for(int i=0;i<g_ndlc;i++) if(g_dlc[i].id==id){ if(version) *version=g_dlc[i].version; if(stub) *stub=g_dlc[i].stub; return 1; }
  return 0; }

/* ============================================================ the title's RomFS */
typedef struct { char* name; int parent, child, sib, file; u32 off; } TDir;

typedef struct { char* name; char* host; int parent, sib; u32 off; u64 size, data; } TFile;

struct DlcStore { const char* kind; u64 size; Nsp* nsp; FILE* image;
  u8* meta; u64 meta_size; TDir* d; int nd, cd; TFile* f; int nf, cf; int* order; int norder; };

static int t_dir(DlcStore* s, const char* name, int parent){
  if(s->nd==s->cd){ s->cd=s->cd? s->cd*2 : 64; s->d=realloc(s->d,sizeof(TDir)*s->cd); }
  TDir* d=&s->d[s->nd]; d->name=strdup(name); d->parent=parent; d->child=d->sib=d->file=-1; d->off=0; return s->nd++; }

static int t_file(DlcStore* s, const char* name, int parent, u64 size, const char* host){
  if(s->nf==s->cf){ s->cf=s->cf? s->cf*2 : 256; s->f=realloc(s->f,sizeof(TFile)*s->cf); }
  TFile* f=&s->f[s->nf]; f->name=strdup(name); f->host=strdup(host); f->parent=parent; f->sib=-1; f->size=size; f->off=0; f->data=0; return s->nf++; }

static int t_cmp(const void* a, const void* b){ return strcmp(*(char* const*)a,*(char* const*)b); }

/* folders first, then files, each sorted by name: the same stable layout the game's own romfs/ folder gets */
static void t_scan(DlcStore* s, const char* host, int di){
  PDir d; if(plat_dir_open(&d,host)) return;
  char** names=NULL; int* isdir=NULL; u64* sizes=NULL; int n=0, cap=0;
  while(plat_dir_next(&d)){
    if(n==cap){ cap=cap? cap*2 : 64; names=realloc(names,sizeof(char*)*cap); isdir=realloc(isdir,sizeof(int)*cap); sizes=realloc(sizes,8*cap); }
    names[n]=strdup(d.name); isdir[n]=d.is_dir; sizes[n]=d.size; n++; }
  plat_dir_close(&d);
  for(int pass=0;pass<2;pass++){
    int last=-1, m=0; char** idx=malloc(sizeof(char*)*(n? n : 1));
    for(int i=0;i<n;i++) if(isdir[i]==(pass==0)) idx[m++]=names[i];
    qsort(idx,m,sizeof(char*),t_cmp);
    for(int k=0;k<m;k++){ int i=0; while(names[i]!=idx[k]) i++;
      char p[2048]; snprintf(p,sizeof p,"%s/%s",host,names[i]);
      if(pass==0){ int c=t_dir(s,names[i],di); if(last<0) s->d[di].child=c; else s->d[last].sib=c; last=c; t_scan(s,p,c); }
      else { int f=t_file(s,names[i],di,sizes[i],p); if(last<0) s->d[di].file=f; else s->f[last].sib=f; last=f; } }
    free(idx); }
  for(int i=0;i<n;i++) free(names[i]);
  free(names); free(isdir); free(sizes); }

static u32 t_hash(u32 parent, const char* s){ u32 h=parent^123456789u; for(;*s;s++){ h=(h>>5)|(h<<27); h^=(u8)*s; } return h; }

static u32 t_buckets(u32 n){ if(n<3) return 3; if(n<19) return n|1; u32 c=n; while(!(c%2)||!(c%3)||!(c%5)||!(c%7)||!(c%11)||!(c%13)||!(c%17)) c++; return c; }

static void t_emit(DlcStore* s){
  u32 doff=0, foff=0;
  for(int i=0;i<s->nd;i++){ s->d[i].off=doff; doff+=0x18+(((u32)strlen(s->d[i].name)+3)&~3u); }
  for(int i=0;i<s->nf;i++){ s->f[i].off=foff; foff+=0x20+(((u32)strlen(s->f[i].name)+3)&~3u); }
  u32 dhc=t_buckets((u32)s->nd), fhc=t_buckets((u32)s->nf);
  u64 dht=0x50, dmt=dht+dhc*4, fht=dmt+doff, fmt=fht+fhc*4, data_at=(fmt+foff+0x1FF)&~0x1FFull, data=0;
  s->meta_size=data_at; s->meta=calloc(1,(size_t)data_at);
  u64 H[10]={0x50,dht,dhc*4ull,dmt,doff,fht,fhc*4ull,fmt,foff,data_at}; memcpy(s->meta,H,0x50);
  u32* dh=(u32*)(s->meta+dht); for(u32 i=0;i<dhc;i++) dh[i]=0xFFFFFFFFu;
  u32* fh=(u32*)(s->meta+fht); for(u32 i=0;i<fhc;i++) fh[i]=0xFFFFFFFFu;
  s->order=malloc(sizeof(int)*(s->nf? s->nf : 1));
  for(int i=0;i<s->nf;i++){ TFile* f=&s->f[i]; data=(data+0xF)&~0xFull; f->data=data_at+data;
    if(f->size) s->order[s->norder++]=i;      /* an empty file shares its offset with the next one: never read */
    u8* e=s->meta+fmt+f->off; u32 w[2]={s->d[f->parent].off, f->sib>=0? s->f[f->sib].off : 0xFFFFFFFFu};
    u32 h=t_hash(s->d[f->parent].off,f->name)%fhc, nl=(u32)strlen(f->name);
    memcpy(e,w,8); memcpy(e+8,&data,8); memcpy(e+16,&f->size,8); memcpy(e+0x18,&fh[h],4); fh[h]=f->off;
    memcpy(e+0x1C,&nl,4); memcpy(e+0x20,f->name,nl); data+=f->size; }
  for(int i=0;i<s->nd;i++){ TDir* d=&s->d[i]; u8* e=s->meta+dmt+d->off;
    u32 w[4]={s->d[d->parent].off, d->sib>=0? s->d[d->sib].off : 0xFFFFFFFFu, d->child>=0? s->d[d->child].off : 0xFFFFFFFFu,
      d->file>=0? s->f[d->file].off : 0xFFFFFFFFu};
    u32 h=t_hash(i? s->d[d->parent].off : 0,d->name)%dhc, nl=(u32)strlen(d->name);
    memcpy(e,w,16); memcpy(e+16,&dh[h],4); dh[h]=d->off; memcpy(e+20,&nl,4); memcpy(e+24,d->name,nl); }
  s->size=data_at+data; }

/* an empty but valid RomFS: one root folder, no files */
static void t_empty(DlcStore* s){
  s->meta_size=s->size=0x200; s->meta=calloc(1,0x200);
  u64 H[10]={0x50, 0x50,4, 0x54,0x18, 0x6C,4, 0x70,0, 0x200}; memcpy(s->meta,H,0x50);
  u32 none=0xFFFFFFFFu, root[6]={0,none,none,none,none,0};
  memcpy(s->meta+0x50,&none,4); memcpy(s->meta+0x54,root,0x18); memcpy(s->meta+0x6C,&none,4); }

static int image_ok(FILE* f, u64 size){ u64 h[10];
  return size>=0x50 && plat_pread(f,h,0x50,0)==0x50 && h[0]==0x50 && h[9]<=size && h[3]<=size && h[7]<=size; }

DlcStore* dlc_store_open(const char* datadir, u64 id){
  int stub=0; if(!dlc_find(id,NULL,&stub)) return NULL;
  DlcStore* s=calloc(1,sizeof *s); if(!s) return NULL;
  char p[1400];
  if(!stub){
    snprintf(p,sizeof p,"%sdlc/%016llx/romfs.nsp",datadir,(unsigned long long)id);
    s->nsp=nsp_open_marker(p);
    if(s->nsp){ s->size=nsp_size(s->nsp); s->kind="package, read in place"; return s; }
    snprintf(p,sizeof p,"%sdlc/%016llx/romfs.bin",datadir,(unsigned long long)id);
    s->image=fopen(p,"rb");
    if(s->image){ s->size=plat_file_size(p);
      if(image_ok(s->image,s->size)){ s->kind="image"; return s; }
      WARN("dlc: %s is not a RomFS image\n",p); fclose(s->image); s->image=NULL; }
    snprintf(p,sizeof p,"%sdlc/%016llx/romfs",datadir,(unsigned long long)id);
    if(plat_is_dir(p)){ t_dir(s,"",0); t_scan(s,p,0); t_emit(s); s->kind="folder"; return s; }
    WARN("dlc: the data of %016llx in %sdlc/ went missing while the game runs: an empty one is provided\n",(unsigned long long)id,datadir); }
  t_empty(s); s->kind="empty"; return s; }

const char* dlc_store_kind(DlcStore* s){ return s->kind; }
u64 dlc_store_size(DlcStore* s){ return s->size; }

int dlc_store_read(DlcStore* s, u64 off, void* dst, u64 size){
  u8* d=dst;
  if(off>s->size || size>s->size-off) return -1;
  if(s->nsp) return nsp_pread(s->nsp,d,size,off);
  if(s->image){ while(size){ int want=size>0x40000000? 0x40000000 : (int)size, got=plat_pread(s->image,d,want,off);
      if(got<=0) return -1; d+=got; off+=(u64)got; size-=(u64)got; }
    return 0; }
  if(off<s->meta_size){ u64 n=s->meta_size-off; if(n>size) n=size; memcpy(d,s->meta+off,n); d+=n; off+=n; size-=n; }
  while(size){
    int lo=0, hi=s->norder-1, k=-1;
    while(lo<=hi){ int m=(lo+hi)/2; if(s->f[s->order[m]].data<=off){ k=m; lo=m+1; } else hi=m-1; }
    if(k<0){ memset(d,0,size); break; }
    TFile* f=&s->f[s->order[k]];
    if(off>=f->data+f->size){   /* the padding up to the next file */
      u64 next= k+1<s->norder? s->f[s->order[k+1]].data : s->size, n= next>off? next-off : size;
      if(n>size) n=size; memset(d,0,n); d+=n; off+=n; size-=n; continue; }
    u64 n=f->data+f->size-off; if(n>size) n=size;
    FILE* fp=fopen(f->host,"rb");
    if(!fp || plat_pread(fp,d,n,off-f->data)<(int)n) memset(d,0,n);
    if(fp) fclose(fp);
    d+=n; off+=n; size-=n; }
  return 0; }

void dlc_store_close(DlcStore* s){
  if(!s) return;
  if(s->nsp) nsp_close(s->nsp);
  if(s->image) fclose(s->image);
  for(int i=0;i<s->nd;i++) free(s->d[i].name);
  for(int i=0;i<s->nf;i++){ free(s->f[i].name); free(s->f[i].host); }
  free(s->d); free(s->f); free(s->order); free(s->meta); free(s); }
