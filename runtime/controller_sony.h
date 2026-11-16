/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the GNU Affero General Public License v3.0, see LICENSE.txt and NOTICE.txt.
   Forks and derived works have to keep the credit to SwitchRecomp by MZ Labs and the startup screen. */
/* protocol layouts adapted from Linux drivers/hid/hid-playstation.c:
   Copyright (c) 2020-2022 Sony Interactive Entertainment, GPL-2.0-or-later.
   https://github.com/torvalds/linux/blob/v6.12/drivers/hid/hid-playstation.c
   only classic rumble and player lights are enabled; audio and adaptive triggers are untouched. */
#ifndef SWR_CONTROLLER_SONY_H
#define SWR_CONTROLLER_SONY_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>

enum { SONY_DS4=4, SONY_DS5=5, SONY_USB=1, SONY_BT=2 };
typedef struct { uint64_t buttons; int32_t lx,ly,rx,ry; } SonyState;

static int sony_model(unsigned vid,unsigned pid){
  if(vid!=0x054c) return 0;
  if(pid==0x0ce6 || pid==0x0df2) return SONY_DS5;
  if(pid==0x05c4 || pid==0x09cc) return SONY_DS4;
  return 0;
}
static int sony_transport(int model,unsigned input,unsigned output){
  if(model!=SONY_DS4 && model!=SONY_DS5) return 0;
  if(input==78 && output==78) return SONY_BT;
  if(input==64 && (model==SONY_DS5?(output==48 || output==63):output==32)) return SONY_USB;
  return 0;
}
static uint32_t sony_crc(unsigned seed,const uint8_t* p,size_t n){
  uint32_t c=0xffffffffu;
  for(size_t i=0;i<=n;i++){ c^=i?p[i-1]:seed;
    for(int k=0;k<8;k++) c=(c>>1)^((0u-(c&1))&0xedb88320u); }
  return ~c;
}
static void sony_sign(uint8_t* p){
  uint32_t c=sony_crc(0xa2,p,74); for(int i=0;i<4;i++) p[74+i]=(uint8_t)(c>>(i*8));
}
/* reset=1 is a separate DualSense lightbar-animation release packet. */
static size_t sony_output(int model,int transport,unsigned seq,unsigned low,unsigned high,int lit,int reset,uint8_t* out,size_t cap){
  if((model!=SONY_DS4 && model!=SONY_DS5) || (transport!=SONY_USB && transport!=SONY_BT)) return 0;
  /* Windows DualSense descriptors expose the 47-byte payload + ID, also used by SDL. */
  size_t n=transport==SONY_BT?78:model==SONY_DS5?48:32; if(cap<n) return 0;
  memset(out,0,n); uint8_t* p=out+(transport==SONY_BT?3:1);
  if(low>255) low=255; if(high>255) high=255;
  if(model==SONY_DS5){
    out[0]=transport==SONY_BT?0x31:0x02;
    if(transport==SONY_BT){ out[1]=(seq&15)<<4; out[2]=0x10; }
    if(reset){ p[38]=2; p[41]=2; }
    else { p[0]=3; p[1]=0x14; p[2]=high; p[3]=low;
      p[43]=lit?4:0; p[44]=0; p[45]=lit?40:0; p[46]=lit?128:0; }
  } else {
    out[0]=transport==SONY_BT?0x11:0x05;
    if(transport==SONY_BT) out[1]=0xc4;
    p[0]=7; p[3]=high; p[4]=low; p[6]=lit?40:0; p[7]=lit?128:0;
  }
  if(transport==SONY_BT) sony_sign(out); return n;
}
static int32_t sony_axis(unsigned v,int invert){
  int32_t a=(int32_t)v-128; a=a<0?a*32767/128:a*32767/127;
  if(a>-7000 && a<7000) a=0; return invert?-a:a;
}
static int sony_input(int model,int transport,const uint8_t* in,size_t n,SonyState* s){
  if(!n || (model!=SONY_DS4 && model!=SONY_DS5)) return 0;
  const uint8_t* p; unsigned bo;
  if(transport==SONY_USB){ if(n!=64 || in[0]!=1) return 0; p=in+1; bo=model==SONY_DS5?7:4; }
  else if(transport==SONY_BT){
    if(in[0]==1){ if(n<10) return 0; p=in+1; bo=4; } /* simple Bluetooth report */
    else {
      if(n!=78 || in[0]!=(model==SONY_DS5?0x31:0x11)) return 0;
      uint32_t crc=(uint32_t)in[74]|(uint32_t)in[75]<<8|(uint32_t)in[76]<<16|(uint32_t)in[77]<<24;
      if(sony_crc(0xa1,in,74)!=crc) return 0;
      p=in+(model==SONY_DS5?2:3); bo=model==SONY_DS5?7:4;
    }
  } else return 0;
  uint64_t b=0; unsigned f=p[bo],x=p[bo+1],hat=f&15;
  /* keep the WinMM Sony preference: Cross confirms, Circle goes back. */
  if(f&0x20) b|=1ull<<0; if(f&0x40) b|=1ull<<1;
  if(f&0x80) b|=1ull<<2; if(f&0x10) b|=1ull<<3;
  if(x&0x40) b|=1ull<<4; if(x&0x80) b|=1ull<<5;
  if(x&1) b|=1ull<<6; if(x&2) b|=1ull<<7;
  if(x&4) b|=1ull<<8; if(x&8) b|=1ull<<9;
  if(x&0x20) b|=1ull<<10; if(x&0x10) b|=1ull<<11;
  if(hat==5 || hat==6 || hat==7) b|=1ull<<12;
  if(hat==7 || hat==0 || hat==1) b|=1ull<<13;
  if(hat==1 || hat==2 || hat==3) b|=1ull<<14;
  if(hat==3 || hat==4 || hat==5) b|=1ull<<15;
  s->buttons=b; s->lx=sony_axis(p[0],0); s->ly=sony_axis(p[1],1);
  s->rx=sony_axis(p[2],0); s->ry=sony_axis(p[3],1); return 1;
}
#endif
