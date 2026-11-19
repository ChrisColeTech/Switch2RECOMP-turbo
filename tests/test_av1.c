/* Copyright 2026 ChrisColeTech
   SPDX-License-Identifier: MIT */
/* The AV1 path of the Switch 2 video decoder service, without a game: runtime/av1video.inc and the committed dav1d
   decode two synthetic streams (tests/av1/vectors.h, made by tests/av1/make_vectors.py from ffmpeg's test pattern)
   and every picture has to come out once, in order, with its unit's time, as the NV12 libaom's decoder gives.
   Run by tests/test_av1.py. */
#include <stdio.h>
#include <stdlib.h>
#include "../runtime/av1video.inc"
#include "av1/vectors.h"

typedef struct { const char* name; const unsigned char* data; const unsigned* sizes; const unsigned long long* hashes; int units,pictures,w,h; } Stream;
typedef struct { int n,bad; const Stream* s; int base; } Got;   /* pictures so far; base: the time of the stream's first unit */
static int g_fail;
#define CHECK(c,...) do{ if(!(c)){ g_fail++; printf("FAIL %s:%d: ",__FILE__,__LINE__); printf(__VA_ARGS__); printf("\n"); } }while(0)

static unsigned long long fnv(unsigned long long h,const uint8_t* p,size_t n){ while(n--) h=(h^*p++)*0x100000001b3ull; return h; }
/* the sink: picture number n of the stream must have the time base + 1000*n and the expected NV12 */
static int sink(void* ctx,const Dav1dPicture* p){
  Got* g=ctx; const Stream* s=g->s; int i=g->n++;
  if(i>=s->pictures || p->p.w!=s->w || p->p.h!=s->h || p->m.timestamp!=g->base+1000ll*i){ g->bad++; return 0; }
  uint32_t w=(uint32_t)s->w,h=(uint32_t)s->h,uvp=(w+1)&~1u; uint8_t* y=malloc((size_t)w*h+(size_t)uvp*((h+1)/2)); uint8_t* uv=y+(size_t)w*h;
  av1_nv12(p,y,uv,uvp);
  if(fnv(0xcbf29ce484222325ull,y,(size_t)w*h+(size_t)uvp*((h+1)/2))!=s->hashes[i]) g->bad++;
  free(y); return 1; }
static const unsigned char* unit(const Stream* s,int i){ const unsigned char* p=s->data; for(int k=0;k<i;k++) p+=s->sizes[k]; return p; }
static void quiet(void* c,const char* f,va_list ap){ (void)c; (void)f; (void)ap; }

/* the whole stream through an open decoder, drained after the unit drain_at (-1: only at the end) */
static void play(Av1Dec* a,const Stream* s,int base,int drain_at,const char* what){
  Got g={0,0,s,base};
  for(int i=0;i<s->units;i++){ CHECK(av1_feed(a,unit(s,i),s->sizes[i],base+1000ll*i,sink,&g)>=0,"%s %s: unit %d",s->name,what,i); if(i==drain_at) av1_drain(a,sink,&g); }
  av1_drain(a,sink,&g);
  CHECK(g.n==s->pictures && !g.bad,"%s %s (drain after unit %d): %d of %d pictures, %d wrong",s->name,what,drain_at,g.n,s->pictures,g.bad);
  CHECK(!a->fed,"%s %s: the drain did not reach its end",s->name,what); }

int main(void){
  const Stream streams[2]={ {"a420 (8-bit 4:2:0)",a420_data,a420_sizes,a420_hashes,(int)(sizeof a420_sizes/sizeof *a420_sizes),(int)(sizeof a420_hashes/sizeof *a420_hashes),a420_w,a420_h},
                            {"b444 (10-bit 4:4:4)",b444_data,b444_sizes,b444_hashes,(int)(sizeof b444_sizes/sizeof *b444_sizes),(int)(sizeof b444_hashes/sizeof *b444_hashes),b444_w,b444_h} };
  printf("dav1d %s\n",dav1d_version());
  for(int k=0;k<2;k++){ const Stream* s=&streams[k]; Av1Dec a;
    /* one frame at a time: every picture is out when its unit has gone in */
    CHECK(av1_open(&a,1,1,quiet),"open 1/1");
    { Got g={0,0,s,5}; for(int i=0;i<s->units;i++) av1_feed(&a,unit(s,i),s->sizes[i],5+1000ll*i,sink,&g);
      CHECK(g.n==s->pictures && !g.bad,"%s, one frame at a time: %d of %d pictures before any drain, %d wrong",s->name,g.n,s->pictures,g.bad); av1_drain(&a,sink,&g); CHECK(g.n==s->pictures,"%s: a drain gave a picture again",s->name); }
    av1_close(&a);
    /* frame threads, as the runtime opens it: pictures come later than their units, the drain gives the rest */
    CHECK(av1_open(&a,4,3,quiet) && a.frames==3,"open 4/3"); play(&a,s,7,-1,"4 threads, 3 frames");
    /* the stream again on the same decoder, with a drain after each unit in turn (a movie that waits for its pictures) */
    for(int d=0;d<s->units;d++) play(&a,s,100000*(d+1),d,"again");
    /* 300 rounds with drains at random places */
    unsigned r=12345; for(int round=0;round<300;round++){ r=r*1103515245u+12345u; play(&a,s,round,(int)((r>>16)%(unsigned)(s->units+3))-3,"round"); }
    /* a unit that is no AV1: refused, and the stream after it (it starts with a key frame) is whole */
    { static const unsigned char junk[16]={0xff,0xff,0xff,0xff,1,2,3,4,5,6,7,8,9,10,11,12}; unsigned before=a.bad_units; av1_feed(&a,junk,sizeof junk,1,NULL,NULL); av1_drain(&a,NULL,NULL);
      CHECK(a.bad_units==before+1,"%s: junk was not refused (%u)",s->name,a.bad_units-before); play(&a,s,9,-1,"after junk"); }
    av1_close(&a); CHECK(!a.c,"close"); }
  printf(g_fail? "%d FAILED\n" : "OK: every picture once, in order, with its time, as libaom decodes it (2 streams; drains mid-stream; junk refused)\n",g_fail);
  return g_fail? 1 : 0; }
