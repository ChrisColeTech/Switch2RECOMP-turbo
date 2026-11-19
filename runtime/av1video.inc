/* Copyright 2026 ChrisColeTech
   SPDX-License-Identifier: MIT */
/* av1video.inc - AV1 with dav1d (BSD-2-Clause, runtime/av1), for the Switch 2 video decoder service (svc_ns2.c, only
   compiled when SWR_HAVE_AV1 is defined) and for tests/test_av1.c. It needs nothing of the runtime.

   How dav1d is driven is ported from nxnative's dm/host/hle_movie_av1.c (MIT, Copyright (c) 2026 seedds and the
   nxnative contributors; docs/AV1_DECODER.md has the notice):
   - frame threads: a temporal unit of a hierarchical stream holds several frames (the hidden ones the next units only
     show), and decoded one after the other they make that unit many times longer than the others. With
     max_frame_delay above 1 they, and the units behind them, decode side by side; a picture then comes out later than
     its unit went in, so each unit's time rides with it (Dav1dData.m.timestamp, Dav1dPicture.m.timestamp);
   - one dav1d_get_picture for each dav1d_send_data: a second one in a row is dav1d's drain, which waits for the
     frames in flight, so decodes them one after the other;
   - a drain that has begun goes on to its end (EAGAIN) before the next unit: data sent between two drain calls can
     leave dav1d waiting for ever (its frame_thread.next on a frame still in flight);
   - the picture as 8-bit NV12: luma copied, or shifted down from 10 or 12 bits; chroma interleaved, subsampled from
     4:2:2 and 4:4:4 by taking the even rows and columns, grey for 4:0:0. */
#include <dav1d/dav1d.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>

typedef int (*Av1Sink)(void* ctx,const Dav1dPicture* p);   /* each picture, in presentation order; the picture is the decoder's */
typedef struct { Dav1dContext* c;
  int fed;                           /* units went in since the last finished drain: dav1d may hold pictures */
  int threads,frames;                /* as opened */
  unsigned bad_units,bad_frames; int last_error;   /* units dav1d refused, frames it could not decode, its last error */
} Av1Dec;

static int av1_open(Av1Dec* a,int threads,int frames,void (*logger)(void*,const char*,va_list)){
  Dav1dSettings st; dav1d_default_settings(&st);
  if(threads<1) threads=1; if(frames<1) frames=1; if(frames>threads) frames=threads;
  st.n_threads=threads; st.max_frame_delay=frames; st.all_layers=0;   /* the highest spatial layer only, as a player shows */
  if(logger){ st.logger.cookie=a; st.logger.callback=logger; }
  memset(a,0,sizeof *a); a->threads=threads; a->frames=frames;
  int e=dav1d_open(&a->c,&st); if(e){ a->c=NULL; a->last_error=e; return 0; }
  return 1; }
static void av1_close(Av1Dec* a){ if(a->c) dav1d_close(&a->c); a->c=NULL; a->fed=0; }

/* dav1d's planar picture into NV12: y is w*h bytes, uv ((h+1)/2) rows of uvp bytes (uvp >= ((w+1)&~1)) */
static void av1_nv12(const Dav1dPicture* p,uint8_t* y,uint8_t* uv,uint32_t uvp){
  uint32_t w=(uint32_t)p->p.w,h=(uint32_t)p->p.h,ch=(h+1)/2,cw=(w+1)/2; int sh=p->p.bpc-8, l=p->p.layout;
  int sx= l!=DAV1D_PIXEL_LAYOUT_I444, sy= l==DAV1D_PIXEL_LAYOUT_I420;
  for(uint32_t r=0;r<h;r++){ const uint8_t* s=(const uint8_t*)p->data[0]+(ptrdiff_t)r*p->stride[0]; uint8_t* d=y+(size_t)r*w;
    if(sh>0) for(uint32_t x=0;x<w;x++){ uint16_t q; memcpy(&q,s+x*2,2); d[x]=(uint8_t)(q>>sh); } else memcpy(d,s,w); }
  for(uint32_t r=0;r<ch;r++){ uint8_t* d=uv+(size_t)r*uvp;
    if(l==DAV1D_PIXEL_LAYOUT_I400){ memset(d,128,cw*2); continue; }
    uint32_t row= sy? r : 2*r; const uint8_t* u=(const uint8_t*)p->data[1]+(ptrdiff_t)row*p->stride[1]; const uint8_t* v=(const uint8_t*)p->data[2]+(ptrdiff_t)row*p->stride[1];
    for(uint32_t x=0;x<cw;x++){ uint32_t c= sx? x : 2*x;
      if(sh>0){ uint16_t a,b; memcpy(&a,u+c*2,2); memcpy(&b,v+c*2,2); d[x*2]=(uint8_t)(a>>sh); d[x*2+1]=(uint8_t)(b>>sh); } else { d[x*2]=u[c]; d[x*2+1]=v[c]; } } } }

/* one dav1d_get_picture: 1 a picture (given to the sink), 0 none now, below 0 dav1d's error */
static int av1_take(Av1Dec* a,Av1Sink sink,void* ctx){
  Dav1dPicture p; memset(&p,0,sizeof p); int g=dav1d_get_picture(a->c,&p);
  if(!g){ if(sink) sink(ctx,&p); dav1d_picture_unref(&p); return 1; }
  if(g==DAV1D_ERR(EAGAIN)) return 0;
  a->bad_frames++; a->last_error=g; return g; }
/* the pictures dav1d still holds, to the drain's end: how many came. A frame in error is passed over */
static int av1_drain(Av1Dec* a,Av1Sink sink,void* ctx){
  int n=0; if(!a->c || !a->fed) return 0;
  for(int tries=0;tries<64;tries++){ int g=av1_take(a,sink,ctx); if(!g){ a->fed=0; break; } if(g>0) n++; }
  return n; }
/* one temporal unit (its OBUs in the low-overhead format, each with its size) with its time: how many pictures came out,
   -1 no memory. A unit dav1d refuses is dropped (bad_units) and the stream goes on */
static int av1_feed(Av1Dec* a,const uint8_t* data,size_t n,int64_t pts,Av1Sink sink,void* ctx){
  int got=0; if(!a->c || !n) return 0;
  Dav1dData in; memset(&in,0,sizeof in); uint8_t* buf=dav1d_data_create(&in,n); if(!buf) return -1;
  memcpy(buf,data,n); in.m.timestamp=pts; a->fed=1;
  for(int tries=0; in.sz && tries<64; tries++){
    int r=dav1d_send_data(a->c,&in);   /* EAGAIN: a picture has to be taken first */
    if(r && r!=DAV1D_ERR(EAGAIN)){ a->bad_units++; a->last_error=r; break; }
    if(av1_take(a,sink,ctx)>0) got++; }
  if(in.sz) dav1d_data_unref(&in);
  return got; }
