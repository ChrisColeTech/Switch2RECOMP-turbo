/* Copyright 2026 ChrisColeTech
   SPDX-License-Identifier: MIT */
/* The committed libopus decodes: a tone encoded with the archive's own encoder comes back as that tone, through the two
   decoders the hwopus service uses (opus_decode, opus_multistream_decode), at 48 kHz in 20 ms packets as the games send
   them. No game data. Run by tests/test_opus.py. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "opus.h"
#include "opus_multistream.h"

#define RATE 48000
#define FRAME 960
#define FRAMES 50

static int fails;
static void check(int ok, const char* what){ printf("%s  %s\n", ok ? "ok  " : "FAIL", what); if(!ok) fails++; }

/* the decoded tone against the original: RMS of both and their correlation, after the codec's delay */
static void compare(const short* a, const short* b, int n, int step, double* ra, double* rb, double* corr){
  double best=-1, sa=0, sb=0;
  for(int i=0;i<n;i++){ sa+=(double)a[i*step]*a[i*step]; sb+=(double)b[i*step]*b[i*step]; }
  for(int d=0; d<800; d++){ double x=0, ea=0, eb=0;
    for(int i=0;i+d<n;i++){ x+=(double)a[i*step]*b[(i+d)*step]; ea+=(double)a[i*step]*a[i*step]; eb+=(double)b[(i+d)*step]*b[(i+d)*step]; }
    double c= ea>0 && eb>0 ? x/sqrt(ea*eb) : 0; if(c>best) best=c; }
  *ra=sqrt(sa/n)/32768; *rb=sqrt(sb/n)/32768; *corr=best;
}

int main(void){
  int err=0; static short in[FRAME*FRAMES*2], out[FRAME*FRAMES*2]; unsigned char pkt[FRAMES][1500]; int len[FRAMES];
  double ra, rb, c;
  printf("%s\n", opus_get_version_string());

  /* mono, as Ocarina of Time's movie sound: six decoders of one channel each */
  for(int i=0;i<FRAME*FRAMES;i++) in[i]=(short)(8000*sin(2*3.14159265358979*440*i/RATE));
  OpusEncoder* e=opus_encoder_create(RATE,1,OPUS_APPLICATION_AUDIO,&err); check(e && !err,"mono encoder");
  for(int f=0;f<FRAMES;f++){ len[f]=opus_encode(e,in+f*FRAME,FRAME,pkt[f],sizeof pkt[f]); if(len[f]<=0){ check(0,"encode"); return 1; } }
  OpusDecoder* d=opus_decoder_create(RATE,1,&err); check(d && !err,"mono decoder");
  int total=0; for(int f=0;f<FRAMES;f++){ int n=opus_decode(d,pkt[f],len[f],out+total,FRAME,0); if(n!=FRAME){ check(0,"a packet decodes to 960 samples"); return 1; } total+=n; }
  compare(in,out,FRAME*FRAMES-800,1,&ra,&rb,&c);
  printf("mono: RMS in %.4f, out %.4f, correlation %.4f\n",ra,rb,c);
  check(fabs(rb-ra)<0.02 && rb>0.1 && c>0.95,"the mono tone comes back (RMS within 0.02, correlation over 0.95)");
  check(opus_decode(d,(const unsigned char*)"\xff\xff\xff",3,out,FRAME,0)<0,"a junk packet is refused, not decoded");
  opus_decoder_destroy(d); opus_encoder_destroy(e);

  /* stereo through the multistream decoder (one coupled stream) */
  for(int i=0;i<FRAME*FRAMES;i++){ in[i*2]=(short)(8000*sin(2*3.14159265358979*440*i/RATE)); in[i*2+1]=(short)(4000*sin(2*3.14159265358979*660*i/RATE)); }
  unsigned char map[2]={0,1};
  OpusMSEncoder* me=opus_multistream_encoder_create(RATE,2,1,1,map,OPUS_APPLICATION_AUDIO,&err); check(me && !err,"multistream encoder");
  for(int f=0;f<FRAMES;f++){ len[f]=opus_multistream_encode(me,in+f*FRAME*2,FRAME,pkt[f],sizeof pkt[f]); if(len[f]<=0){ check(0,"multistream encode"); return 1; } }
  OpusMSDecoder* md=opus_multistream_decoder_create(RATE,2,1,1,map,&err); check(md && !err,"multistream decoder");
  total=0; for(int f=0;f<FRAMES;f++){ int n=opus_multistream_decode(md,pkt[f],len[f],out+total*2,FRAME,0); if(n!=FRAME){ check(0,"a multistream packet decodes to 960 samples"); return 1; } total+=n; }
  compare(in,out,FRAME*FRAMES-800,2,&ra,&rb,&c); printf("stereo, left: RMS in %.4f, out %.4f, correlation %.4f\n",ra,rb,c);
  check(fabs(rb-ra)<0.02 && c>0.95,"the left tone comes back");
  compare(in+1,out+1,FRAME*FRAMES-800,2,&ra,&rb,&c); printf("stereo, right: RMS in %.4f, out %.4f, correlation %.4f\n",ra,rb,c);
  check(fabs(rb-ra)<0.02 && c>0.95,"the right tone comes back");
  opus_multistream_decoder_destroy(md); opus_multistream_encoder_destroy(me);
  printf("%s\n", fails ? "FAILED" : "PASS");
  return fails ? 1 : 0;
}
