/* Copyright 2026 ChrisColeTech
   SPDX-License-Identifier: MIT */
/* a RomFS image read in place from an application NSP, described by the build's marker (romfs.nsp):
     SWRNSP1  one plaintext byte range of the package
     SWRNSP2  the RomFS section of the Program NCA, AES-CTR encrypted, optionally the update's BKTR patch section
              composed over the base's. The update is in the game's package (a combined one, docs/COMBINED_NSP.md)
              or in a package of its own, named by an "update <path>" row before the patch row (docs/UPDATES.md)
   the marker names the content keys and holds them wrapped, as they are in the package; they are unwrapped here
   with the user's prod.keys */
#include "rt.h"
#include <errno.h>
#include <ctype.h>
#if defined(_WIN32)
#include <windows.h>
#endif

/* the marker is UTF-8 (the build writes it), the runtime's own paths are in the ANSI code page: a package or keys
   path with other characters opens through the wide API. *size gets the file size */
static FILE* open_utf8(const char* path,u64* size){
  FILE* f=NULL;
#if defined(_WIN32)
  wchar_t w[2048];
  if(MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,path,-1,w,2048)>0) f=_wfopen(w,L"rb");
  if(!f) f=fopen(path,"rb");
  if(f && size){ _fseeki64(f,0,SEEK_END); *size=(u64)_ftelli64(f); _fseeki64(f,0,SEEK_SET); }
#else
  f=fopen(path,"rb");
  if(f && size){ fseeko(f,0,SEEK_END); *size=(u64)ftello(f); fseeko(f,0,SEEK_SET); }
#endif
  return f; }
#if defined(__x86_64__) && defined(__GNUC__) && !defined(__TINYC__)
#include <immintrin.h>
#define NSP_AESNI 1
#endif

typedef struct { u8 rk[176]; } Aes;
/* one NCA section inside the package */
typedef struct { int present,crypto; u64 file_off,size,nca_off; u8 upper[8]; Aes aes; FILE* file; } Layer;
typedef struct { u64 virt,phys; u32 patch; } Reloc;      /* [virt, next virt) of the patched section -> base or patch */
typedef struct { u64 off; u32 gen; } Gen;                /* counter generation of the patch section from off on */

struct Nsp { FILE* file; u64 offset,size; char path[2048];
  FILE* ufile; u64 utotal; char upath[2048];           /* the update's own package, when it is not the game's */
  int v2; Layer base,patch; Reloc* rel; u32 nrel; u64 vsize; Gen* gen; u32 ngen; };

/* ---------------------------------------------------------------- AES-128 */
static u8 g_sbox[256],g_inv[256]; static int g_aes_ready,g_aesni;
static u8 xt(u8 a){ return (u8)((a<<1)^((a&0x80)?0x1b:0)); }
static u8 gmul(u8 a,u8 b){ u8 r=0; while(b){ if(b&1) r^=a; a=xt(a); b>>=1; } return r; }
static void aes_tables(void){
  if(g_aes_ready) return;
  u8 p=1,q=1;
  do{ p=(u8)(p^xt(p)); q^=(u8)(q<<1); q^=(u8)(q<<2); q^=(u8)(q<<4); if(q&0x80) q^=0x09;
    u8 x=(u8)(q^(u8)((q<<1)|(q>>7))^(u8)((q<<2)|(q>>6))^(u8)((q<<3)|(q>>5))^(u8)((q<<4)|(q>>4)));
    g_sbox[p]=(u8)(x^0x63); }while(p!=1);
  g_sbox[0]=0x63; for(int i=0;i<256;i++) g_inv[g_sbox[i]]=(u8)i;
#ifdef NSP_AESNI
  __builtin_cpu_init(); g_aesni=__builtin_cpu_supports("aes")&&__builtin_cpu_supports("sse2")&&!getenv("SWR_NO_AESNI");
#endif
  g_aes_ready=1; }

static void aes_key(Aes* a,const u8 key[16]){
  aes_tables(); memcpy(a->rk,key,16); u8 rcon=1;
  for(int i=16;i<176;i+=4){ u8 t[4]; memcpy(t,a->rk+i-4,4);
    if(!(i&15)){ u8 s=t[0]; t[0]=(u8)(g_sbox[t[1]]^rcon); t[1]=g_sbox[t[2]]; t[2]=g_sbox[t[3]]; t[3]=g_sbox[s]; rcon=xt(rcon); }
    for(int k=0;k<4;k++) a->rk[i+k]=(u8)(a->rk[i-16+k]^t[k]); } }

static void aes_enc(const Aes* a,const u8 in[16],u8 out[16]){
  u8 s[16],t[16]; for(int i=0;i<16;i++) s[i]=(u8)(in[i]^a->rk[i]);
  for(int r=1;r<=10;r++){
    for(int i=0;i<16;i++) t[i]=g_sbox[s[(i+4*(i&3))&15]];
    if(r<10) for(int c=0;c<16;c+=4){ u8 x0=t[c],x1=t[c+1],x2=t[c+2],x3=t[c+3],all=(u8)(x0^x1^x2^x3);
      t[c]=(u8)(x0^all^xt((u8)(x0^x1))); t[c+1]=(u8)(x1^all^xt((u8)(x1^x2)));
      t[c+2]=(u8)(x2^all^xt((u8)(x2^x3))); t[c+3]=(u8)(x3^all^xt((u8)(x3^x0))); }
    for(int i=0;i<16;i++) s[i]=(u8)(t[i]^a->rk[r*16+i]); }
  memcpy(out,s,16); }

static void aes_dec(const Aes* a,const u8 in[16],u8 out[16]){
  u8 s[16],t[16]; for(int i=0;i<16;i++) s[i]=(u8)(in[i]^a->rk[160+i]);
  for(int r=9;r>=0;r--){
    for(int i=0;i<16;i++) t[i]=g_inv[s[(i+12*(i&3))&15]];
    for(int i=0;i<16;i++) t[i]^=a->rk[r*16+i];
    if(r>0) for(int c=0;c<16;c+=4){ u8 x0=t[c],x1=t[c+1],x2=t[c+2],x3=t[c+3];
      t[c]=(u8)(gmul(x0,14)^gmul(x1,11)^gmul(x2,13)^gmul(x3,9)); t[c+1]=(u8)(gmul(x0,9)^gmul(x1,14)^gmul(x2,11)^gmul(x3,13));
      t[c+2]=(u8)(gmul(x0,13)^gmul(x1,9)^gmul(x2,14)^gmul(x3,11)); t[c+3]=(u8)(gmul(x0,11)^gmul(x1,13)^gmul(x2,9)^gmul(x3,14)); }
    memcpy(s,t,16); }
  memcpy(out,s,16); }

static void counter(u8 c[16],const u8 upper[8],u64 block){
  memcpy(c,upper,8); for(int i=0;i<8;i++) c[8+i]=(u8)(block>>(56-8*i)); }

#ifdef NSP_AESNI
__attribute__((target("aes,sse2")))
static void ctr_blocks_ni(const Aes* a,const u8 upper[8],u64 block,u8* data,u64 blocks){
  __m128i k[11]; for(int i=0;i<11;i++) k[i]=_mm_loadu_si128((const __m128i*)(a->rk+16*i));
  u8 c[4][16];
  while(blocks>=4){
    for(int j=0;j<4;j++) counter(c[j],upper,block+(u64)j);
    __m128i b0=_mm_xor_si128(_mm_loadu_si128((const __m128i*)c[0]),k[0]),b1=_mm_xor_si128(_mm_loadu_si128((const __m128i*)c[1]),k[0]),
            b2=_mm_xor_si128(_mm_loadu_si128((const __m128i*)c[2]),k[0]),b3=_mm_xor_si128(_mm_loadu_si128((const __m128i*)c[3]),k[0]);
    for(int r=1;r<10;r++){ b0=_mm_aesenc_si128(b0,k[r]); b1=_mm_aesenc_si128(b1,k[r]); b2=_mm_aesenc_si128(b2,k[r]); b3=_mm_aesenc_si128(b3,k[r]); }
    b0=_mm_aesenclast_si128(b0,k[10]); b1=_mm_aesenclast_si128(b1,k[10]); b2=_mm_aesenclast_si128(b2,k[10]); b3=_mm_aesenclast_si128(b3,k[10]);
    __m128i* d=(__m128i*)data;
    _mm_storeu_si128(d,_mm_xor_si128(_mm_loadu_si128(d),b0)); _mm_storeu_si128(d+1,_mm_xor_si128(_mm_loadu_si128(d+1),b1));
    _mm_storeu_si128(d+2,_mm_xor_si128(_mm_loadu_si128(d+2),b2)); _mm_storeu_si128(d+3,_mm_xor_si128(_mm_loadu_si128(d+3),b3));
    data+=64; block+=4; blocks-=4; }
  while(blocks){ counter(c[0],upper,block);
    __m128i b=_mm_xor_si128(_mm_loadu_si128((const __m128i*)c[0]),k[0]);
    for(int r=1;r<10;r++) b=_mm_aesenc_si128(b,k[r]);
    b=_mm_aesenclast_si128(b,k[10]);
    _mm_storeu_si128((__m128i*)data,_mm_xor_si128(_mm_loadu_si128((const __m128i*)data),b));
    data+=16; block++; blocks--; } }
#endif

/* AES-CTR in place: data sits at byte `pos` of the NCA */
static void ctr_xor(const Aes* a,const u8 upper[8],u64 pos,u8* data,u64 size){
  u8 c[16],ks[16]; u64 block=pos>>4; unsigned skip=(unsigned)(pos&15);
  if(skip){ counter(c,upper,block); aes_enc(a,c,ks);
    while(skip<16 && size){ *data++^=ks[skip++]; size--; } block++; }
  u64 whole=size>>4;
#ifdef NSP_AESNI
  if(g_aesni && whole){ ctr_blocks_ni(a,upper,block,data,whole); data+=whole<<4; block+=whole; size-=whole<<4; whole=0; }
#endif
  for(;whole;whole--){ counter(c,upper,block++); aes_enc(a,c,ks); for(int i=0;i<16;i++) data[i]^=ks[i]; data+=16; size-=16; }
  if(size){ counter(c,upper,block); aes_enc(a,c,ks); for(u64 i=0;i<size;i++) data[i]^=ks[i]; } }

/* ---------------------------------------------------------------- marker and keys */
static int line(FILE* f,char* out,size_t size){
  if(!fgets(out,(int)size,f)) return 0; size_t n=strlen(out);
  while(n && (out[n-1]=='\r'||out[n-1]=='\n')) out[--n]=0;
  return n>0; }

static int hex(const char* s,u8* out,size_t n){
  for(size_t i=0;i<n;i++){ unsigned v; if(sscanf(s+2*i,"%2x",&v)!=1) return -1; out[i]=(u8)v; }
  return 0; }

/* the 16-byte key `name` of a prod.keys file */
static int key_from_file(const char* path,const char* name,u8 key[16]){
  FILE* f=open_utf8(path,NULL); if(!f) return -1;
  char row[1024]; size_t nl=strlen(name); int found=-1;
  while(fgets(row,sizeof row,f)){ char* p=row; while(*p==' '||*p=='\t') p++;
    if(strncmp(p,name,nl)) continue; p+=nl; while(*p==' '||*p=='\t') p++;
    if(*p!='=') continue; p++; while(*p==' '||*p=='\t') p++;
    size_t n=0; while(isxdigit((unsigned char)p[n])) n++;
    if(n==32 && !hex(p,key,16)){ found=0; break; } }
  fclose(f); return found; }

/* "<key name>:<wrapped content key>" -> the section's AES key */
static int unwrap(const char* spec,const char* keys,const char* package,Aes* out){
  char name[96]; const char* colon=strchr(spec,':'); u8 kek[16],wrapped[16],key[16];
  if(!colon || (size_t)(colon-spec)>=sizeof name || strlen(colon+1)!=32 || hex(colon+1,wrapped,16)) return -1;
  memcpy(name,spec,(size_t)(colon-spec)); name[colon-spec]=0;
  const char* override=getenv("SWR_PROD_KEYS");
  if((!override || !override[0] || key_from_file(override,name,kek)) && key_from_file(keys,name,kek)){
    WARN("RomFS: %s is encrypted and needs %s from your prod.keys (%s). Install the keys (launcher: Keys > Install keys) "
         "or set SWR_PROD_KEYS, then start the game again.\n",package,name,keys);
    return -1; }
  Aes a; aes_key(&a,kek); aes_dec(&a,wrapped,key); aes_key(out,key);
  memset(kek,0,sizeof kek); memset(key,0,sizeof key); memset(&a,0,sizeof a); return 0; }

/* ---------------------------------------------------------------- sections */
static int raw_read(FILE* f,void* dst,u64 size,u64 off){
  size_t done=0; while(done<size){ u64 left=size-done; int want=left>0x40000000?0x40000000:(int)left;
    int got=plat_pread(f,(u8*)dst+done,want,off+done); if(got<=0) return -1; done+=(size_t)got; }
  return 0; }

/* bytes [off, off+size) of a section, decrypted; gen<0 keeps the section's own counter generation */
static int layer_read(Nsp* n,const Layer* l,void* dst,u64 size,u64 off,int64_t gen){
  if(!l->present || off>l->size || size>l->size-off) return -1;
  if(raw_read(l->file? l->file : n->file,dst,size,l->file_off+off)) return -1;
  if(l->crypto==1) return 0;
  u8 upper[8]; memcpy(upper,l->upper,8);
  if(gen>=0){ upper[4]=(u8)(gen>>24); upper[5]=(u8)(gen>>16); upper[6]=(u8)(gen>>8); upper[7]=(u8)gen; }
  ctr_xor(&l->aes,upper,l->nca_off+off,dst,size); return 0; }

static int patch_read(Nsp* n,u8* dst,u64 size,u64 off){
  u32 lo=0,hi=n->ngen-1;
  while(lo<hi){ u32 m=(lo+hi+1)/2; if(n->gen[m].off<=off) lo=m; else hi=m-1; }
  while(size){ u64 end= lo+1<n->ngen? n->gen[lo+1].off : n->patch.size;
    if(end<=off) return -1;
    u64 part=end-off; if(part>size) part=size;
    if(layer_read(n,&n->patch,dst,part,off,(int64_t)n->gen[lo].gen)) return -1;
    dst+=part; off+=part; size-=part; lo++; }
  return 0; }

/* bytes of the patched (virtual) section */
static int virtual_read(Nsp* n,u8* dst,u64 size,u64 off){
  if(off>n->vsize || size>n->vsize-off) return -1;
  u32 lo=0,hi=n->nrel-1;
  while(lo<hi){ u32 m=(lo+hi+1)/2; if(n->rel[m].virt<=off) lo=m; else hi=m-1; }
  while(size){ const Reloc* r=&n->rel[lo]; u64 end= lo+1<n->nrel? n->rel[lo+1].virt : n->vsize;
    if(end<=off) return -1;
    u64 part=end-off; if(part>size) part=size; u64 at=r->phys+(off-r->virt);
    if(r->patch? patch_read(n,dst,part,at) : layer_read(n,&n->base,dst,part,at,-1)) return -1;
    dst+=part; off+=part; size-=part; lo++; }
  return 0; }

static u32 rd32(const u8* p){ return (u32)p[0]|(u32)p[1]<<8|(u32)p[2]<<16|(u32)p[3]<<24; }
static u64 rd64(const u8* p){ return (u64)rd32(p)|(u64)rd32(p+4)<<32; }

/* a BKTR table of the patch section: 0x4000 header, then 0x4000 buckets of `entry` byte entries. Returns the
   table (the caller walks it with the same layout) or NULL */
static u8* table_read(Nsp* n,u64 at,u64 size,u32 entry,u32* count,u64* total){
  if(size<0x4000 || size>(256u<<20) || at>n->patch.size || size>n->patch.size-at) return NULL;
  u8* t=malloc((size_t)size); if(!t) return NULL;
  if(layer_read(n,&n->patch,t,size,at,-1)){ free(t); return NULL; }
  u32 buckets=rd32(t+4); *total=rd64(t+8); *count=0;
  if((u64)buckets*0x4000>size-0x4000){ free(t); return NULL; }
  for(u32 b=0;b<buckets;b++){ u32 c=rd32(t+0x4000+(u64)b*0x4000+4);
    if(c>0x3ff0/entry){ free(t); return NULL; } *count+=c; }
  return t; }

static int tables_load(Nsp* n,u64 ind_at,u64 ind_size,u64 ex_at,u64 ex_size){
  u32 count; u64 total; u8* t=table_read(n,ind_at,ind_size,0x14,&count,&total);
  if(!t || !count){ free(t); return -1; }
  n->rel=malloc(sizeof(Reloc)*count); n->nrel=0; n->vsize=total; if(!n->rel){ free(t); return -1; }
  for(u32 b=0;b<rd32(t+4);b++){ const u8* bk=t+0x4000+(u64)b*0x4000;
    for(u32 i=0;i<rd32(bk+4);i++){ const u8* e=bk+0x10+i*0x14;
      n->rel[n->nrel++]=(Reloc){rd64(e),rd64(e+8),rd32(e+16)}; } }
  free(t);
  t=table_read(n,ex_at,ex_size,0x10,&count,&total);
  if(!t || !count){ free(t); return -1; }
  n->gen=malloc(sizeof(Gen)*count); n->ngen=0; if(!n->gen){ free(t); return -1; }
  for(u32 b=0;b<rd32(t+4);b++){ const u8* bk=t+0x4000+(u64)b*0x4000;
    for(u32 i=0;i<rd32(bk+4);i++){ const u8* e=bk+0x10+i*0x10; n->gen[n->ngen++]=(Gen){rd64(e),rd32(e+12)}; } }
  free(t);
  /* every range has to stay inside its section, or the keys are wrong / the update belongs to another game */
  if(n->rel[0].virt) return -1;
  for(u32 i=0;i<n->nrel;i++){ const Reloc* r=&n->rel[i]; u64 end= i+1<n->nrel? n->rel[i+1].virt : n->vsize;
    const Layer* l= r->patch? &n->patch : &n->base;
    if(r->patch>1 || end<r->virt || !l->present || r->phys>l->size || end-r->virt>l->size-r->phys) return -1; }
  for(u32 i=1;i<n->ngen;i++) if(n->gen[i].off<n->gen[i-1].off) return -1;
  return 0; }

static int layer_parse(const char* row,Layer* l,const char* keys,const char* package,u64 total,u64 tables[4]){
  unsigned long long file_off,size,nca_off,t[4]={0,0,0,0}; int crypto; char upper[32],key[160];
  int got=sscanf(row,"%llx %llx %llx %d %16s %159s %llx %llx %llx %llx",&file_off,&size,&nca_off,&crypto,upper,key,&t[0],&t[1],&t[2],&t[3]);
  if(got<6 || (tables && got!=10) || strlen(upper)!=16 || hex(upper,l->upper,8)) return -1;
  if(!size || file_off>total || size>total-file_off || (crypto!=1 && crypto!=3 && crypto!=4)) return -1;
  l->file_off=file_off; l->size=size; l->nca_off=nca_off; l->crypto=crypto; l->present=1;
  if(tables) for(int i=0;i<4;i++) tables[i]=t[i];
  if(crypto!=1 && unwrap(key,keys,package,&l->aes)) return -2;
  return 0; }

Nsp* nsp_open_marker(const char* marker){
  FILE* m=fopen(marker,"rb"); char magic[16],path[2048],row[4096],keys[2048]="",*end; Nsp* n=NULL; int ok=0;
  if(!m) return NULL;
  if(!line(m,magic,sizeof magic)||(strcmp(magic,"SWRNSP1")&&strcmp(magic,"SWRNSP2"))||!line(m,path,sizeof path)) goto done;
  u64 total=0;
  n=calloc(1,sizeof *n); if(!n) goto done;
  snprintf(n->path,sizeof n->path,"%s",path);
  n->file=open_utf8(path,&total);
  if(!n->file){ WARN("RomFS: the package %s is missing (moved or on a disconnected drive)\n",path); goto done; }
  if(!strcmp(magic,"SWRNSP1")){ char off[32],size[32];
    if(!line(m,off,sizeof off)||!line(m,size,sizeof size)) goto done;
    errno=0; u64 offset=strtoull(off,&end,16); if(errno||*end) goto done;
    errno=0; u64 length=strtoull(size,&end,16); if(errno||*end) goto done;
    if(!length||offset>total||length>total-offset) goto done;
    n->offset=offset; n->size=length; ok=1; goto done; }
  n->v2=1; u64 tables[4]={0,0,0,0}; int have_romfs=0;
  while(line(m,row,sizeof row)){
    if(!strncmp(row,"keys ",5)) snprintf(keys,sizeof keys,"%.2040s",row+5);
    else if(!strncmp(row,"romfs ",6)){ unsigned long long at,size; if(sscanf(row+6,"%llx %llx",&at,&size)!=2||!size) goto done;
      n->offset=at; n->size=size; have_romfs=1; }
    else if(!strncmp(row,"update ",7)){ snprintf(n->upath,sizeof n->upath,"%.2040s",row+7);
      n->ufile=open_utf8(n->upath,&n->utotal);
      if(!n->ufile){ WARN("RomFS: the update package %s is missing (moved or on a disconnected drive). Put it back, or "
                          "remove the update in the launcher (Game > Install Updates) and build the game again.\n",n->upath); goto done; } }
    else if(!strncmp(row,"base ",5)){ int r=layer_parse(row+5,&n->base,keys,path,total,NULL);
      if(r){ if(r==-1) WARN("RomFS: %s has an invalid base line\n",marker); goto done; } }
    else if(!strncmp(row,"patch ",6)){ int r=layer_parse(row+6,&n->patch,keys,n->ufile? n->upath : path,n->ufile? n->utotal : total,tables);
      if(r){ if(r==-1) WARN("RomFS: %s has an invalid patch line\n",marker); goto done; }
      n->patch.file=n->ufile; } }
  if(!have_romfs || (!n->base.present && !n->patch.present)) goto done;
  if(n->patch.present){
    if(tables_load(n,tables[0],tables[1],tables[2],tables[3])){
      WARN("RomFS: the update in %s does not fit its game (wrong keys, or the package changed since the build)\n",n->ufile? n->upath : path); goto done; } }
  else n->vsize=n->base.size;
  if(n->offset>n->vsize || n->size>n->vsize-n->offset) goto done;
  { u8 head[8]; if(nsp_pread(n,head,8,0) || rd64(head)!=0x50){
      WARN("RomFS: the data of %s did not decrypt (the installed prod.keys do not match this package)\n",path); goto done; } }
  ok=1;
done: fclose(m); if(!ok && n){ nsp_close(n); n=NULL; } return n; }

void nsp_close(Nsp* n){ if(!n) return; if(n->file) fclose(n->file); if(n->ufile) fclose(n->ufile); free(n->rel); free(n->gen);
  memset(&n->base.aes,0,sizeof(Aes)); memset(&n->patch.aes,0,sizeof(Aes)); free(n); }
u64 nsp_size(Nsp* n){ return n?n->size:0; }
const char* nsp_path(Nsp* n){ return n?n->path:""; }
const char* nsp_kind(Nsp* n){ return !n||!n->v2? "" : n->ufile? ", the update of its own package applied over the game, decrypted with prod.keys"
  : n->patch.present? ", the update applied over the game, decrypted with prod.keys" : ", decrypted with prod.keys"; }
const char* nsp_update_path(Nsp* n){ return n&&n->ufile? n->upath : ""; }
int nsp_pread(Nsp* n,void* dst,u64 size,u64 off){
  if(!n||off>n->size||size>n->size-off) return -1;
  if(!n->v2) return raw_read(n->file,dst,size,n->offset+off);
  if(n->patch.present) return virtual_read(n,dst,size,n->offset+off);
  return layer_read(n,&n->base,dst,size,n->offset+off,-1); }
