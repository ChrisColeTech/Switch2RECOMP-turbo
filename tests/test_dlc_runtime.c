/* Copyright 2026 ChrisColeTech
   SPDX-License-Identifier: MIT */
/* runtime/dlc.c on its own: the manifest, the aoc:u count / list answers and the four kinds of title storage.
   usage: test_dlc_runtime <data folder with a trailing slash>   (tests/test_nsp_dlc.py makes the folder) */
#include "rt.h"
#include <dirent.h>
#include <stdarg.h>
#include <sys/stat.h>

char g_datadir[1024];
void logf_(int lvl, const char* fmt, ...){ (void)lvl; va_list ap; va_start(ap,fmt); vfprintf(stderr,fmt,ap); va_end(ap); }

int plat_pread(FILE* f, void* buf, u64 size, u64 off){
#if defined(_WIN32)
  if(_fseeki64(f,(long long)off,SEEK_SET)) return -1;
#else
  if(fseeko(f,(off_t)off,SEEK_SET)) return -1;
#endif
  return (int)fread(buf,1,(size_t)size,f); }
u64 plat_file_size(const char* path){ struct stat s; return stat(path,&s)? 0 : (u64)s.st_size; }
int plat_is_dir(const char* path){ struct stat s; return !stat(path,&s) && S_ISDIR(s.st_mode); }
typedef struct { DIR* dir; char path[2048]; } Dir;
int plat_dir_open(PDir* d, const char* path){
  Dir* x=calloc(1,sizeof *x); x->dir=opendir(path); if(!x->dir){ free(x); return -1; }
  snprintf(x->path,sizeof x->path,"%s",path); d->h=x; d->first=0; return 0; }
int plat_dir_next(PDir* d){ Dir* x=d->h;
  for(;;){ struct dirent* e=readdir(x->dir); if(!e) return 0;
    if(!strcmp(e->d_name,".")||!strcmp(e->d_name,"..")) continue;
    char p[4096]; snprintf(p,sizeof p,"%s/%s",x->path,e->d_name); struct stat s; if(stat(p,&s)) continue;
    snprintf(d->name,sizeof d->name,"%s",e->d_name); d->is_dir=S_ISDIR(s.st_mode); d->size=(u64)s.st_size; return 1; } }
void plat_dir_close(PDir* d){ Dir* x=d->h; closedir(x->dir); free(x); }

#define CHECK(x) do{ if(!(x)){ fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); return 1; } }while(0)

/* a file's (offset, size) in a RomFS image, by its path from the root: a walk of the tables like a game does */
static int rom_find(DlcStore* s, const char* dir, const char* name, u64* off, u64* size){
  u64 h[10]; if(dlc_store_read(s,0,h,0x50)) return -1;
  u8* dm=malloc(h[4]+1), *fm=malloc(h[8]+1); dlc_store_read(s,h[3],dm,h[4]); dlc_store_read(s,h[7],fm,h[8]);
  u32 d=0;
  if(dir){ u32 c; memcpy(&c,dm+8,4); d=0xFFFFFFFFu;
    while(c!=0xFFFFFFFFu){ u32 nl; memcpy(&nl,dm+c+20,4); if(nl==strlen(dir) && !memcmp(dm+c+24,dir,nl)){ d=c; break; } memcpy(&c,dm+c+4,4); }
    if(d==0xFFFFFFFFu){ free(dm); free(fm); return -1; } }
  u32 f; memcpy(&f,dm+d+12,4); int r=-1;
  while(f!=0xFFFFFFFFu){ u32 nl; memcpy(&nl,fm+f+0x1C,4);
    if(nl==strlen(name) && !memcmp(fm+f+0x20,name,nl)){ u64 o; memcpy(&o,fm+f+8,8); memcpy(size,fm+f+16,8); *off=h[9]+o; r=0; break; }
    memcpy(&f,fm+f+4,4); }
  free(dm); free(fm); return r; }

static int file_is(DlcStore* s, const char* dir, const char* name, const char* text){
  u64 off=0, size=0; char buf[256]={0};
  if(rom_find(s,dir,name,&off,&size) || size!=strlen(text) || size>=sizeof buf) return 0;
  return !dlc_store_read(s,off,buf,size) && !memcmp(buf,text,size); }

int main(int argc, char** argv){
  if(argc!=2){ fprintf(stderr,"usage: test_dlc_runtime <data folder/>\n"); return 2; }
  snprintf(g_datadir,sizeof g_datadir,"%s",argv[1]);
  const u64 app=0x0100152000022000ull, other=0x0100AAAA00002000ull;
  /* before any manifest is read: nothing */
  char none[1200]; snprintf(none,sizeof none,"%sno-such.manifest",g_datadir);
  CHECK(dlc_load(none)==0); CHECK(dlc_count(app)==0);
  u32 ids[8]={0}; CHECK(dlc_list(app,0,8,ids)==0); CHECK(dlc_list(app,1,8,ids)==-1);
  CHECK(!dlc_store_open(g_datadir,0x0100152000023001ull));

  char m[1200]; snprintf(m,sizeof m,"%sdlc.manifest",g_datadir);
  /* four titles: the comment, the bad line and the duplicate are dropped, and so is the title whose data is gone */
  CHECK(dlc_load(m)==4);
  CHECK(dlc_count(app)==4); CHECK(dlc_count(other)==0); CHECK(dlc_count(0x0100BBBB00004000ull)==0);
  /* aoc:u ListAddOnContent: indexes in title id order, offset and count honored */
  CHECK(dlc_list(app,0,8,ids)==4); CHECK(ids[0]==1 && ids[1]==2 && ids[2]==3 && ids[3]==0x7FF);
  CHECK(dlc_list(app,1,2,ids)==2); CHECK(ids[0]==2 && ids[1]==3);
  CHECK(dlc_list(app,4,8,ids)==0); CHECK(dlc_list(app,5,8,ids)==-1);
  CHECK(dlc_list(other,0,8,ids)==0);          /* listed in the manifest, no data folder: the game is not told about it */
  CHECK(!dlc_find(0x0100AAAA00003005ull,NULL,NULL)); CHECK(!dlc_store_open(g_datadir,0x0100AAAA00003005ull));
  u32 version=0; int stub=-1;
  CHECK(dlc_find(0x0100152000023001ull,&version,&stub) && version==65536 && stub==0);
  CHECK(dlc_find(0x0100152000023003ull,&version,&stub) && version==7 && stub==1);
  CHECK(!dlc_find(0x0100152000023009ull,NULL,NULL));
  CHECK(!dlc_store_open(g_datadir,0x0100152000023009ull));   /* not in the manifest: not ours to serve */

  /* 3001: a folder of files */
  DlcStore* s=dlc_store_open(g_datadir,0x0100152000023001ull); CHECK(s); CHECK(!strcmp(dlc_store_kind(s),"folder"));
  CHECK(file_is(s,NULL,"version.txt","10002")); CHECK(file_is(s,"sub","deep.bin","deep data"));
  CHECK(file_is(s,NULL,"empty.bin",""));
  u8 tail[4]; CHECK(dlc_store_read(s,dlc_store_size(s),tail,1)==-1); CHECK(dlc_store_read(s,dlc_store_size(s)-1,tail,1)==0);
  dlc_store_close(s);
  /* 3002: a RomFS image */
  s=dlc_store_open(g_datadir,0x0100152000023002ull); CHECK(s); CHECK(!strcmp(dlc_store_kind(s),"image"));
  CHECK(file_is(s,NULL,"file.bin","test")); dlc_store_close(s);
  /* 3003: marked stub: an empty RomFS even though a folder exists */
  s=dlc_store_open(g_datadir,0x0100152000023003ull); CHECK(s); CHECK(!strcmp(dlc_store_kind(s),"empty"));
  CHECK(dlc_store_size(s)==0x200); u64 off, size; CHECK(rom_find(s,NULL,"version.txt",&off,&size)==-1); dlc_store_close(s);
  /* 37ff: a marker, read in place from a package */
  s=dlc_store_open(g_datadir,0x01001520000237FFull); CHECK(s); CHECK(!strcmp(dlc_store_kind(s),"package, read in place"));
  CHECK(file_is(s,NULL,"file.bin","test")); dlc_store_close(s);
  /* a marker whose package is gone and an image that is not a RomFS are not data either */
  char lost[1200]; snprintf(lost,sizeof lost,"%slost/dlc.manifest",g_datadir);
  CHECK(dlc_load(lost)==0); CHECK(dlc_count(app)==0);
  CHECK(dlc_load(m)==4);
  puts("PASS add-on content runtime (manifest, list, storages)"); return 0; }
