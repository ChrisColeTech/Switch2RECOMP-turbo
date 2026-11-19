/* Copyright 2026 ChrisColeTech
   SPDX-License-Identifier: MIT */
/* controller button layout, shared by every pad path (XInput, Sony native HID, WinMM, Linux SDL2).
   each path reports its buttons BY POSITION in the Switch bit order: bit 0 = the right face button (where the Switch
   has A), 1 = bottom (B), 2 = top (X), 3 = left (Y), 4..15 = stick clicks, L, R, ZL, ZR, Plus, Minus, D-pad. this
   header turns that positional mask into the guest mask:
     button_layout=switch  identity: the button at the Switch A position is A (right = A, bottom = B, top = X, left = Y)
     button_layout=xbox      by Xbox label: bottom = A, right = B, left = X, top = Y (Cross = A, Square = X on Sony)
     button_map=south:a ...  optional per-button overrides applied on top of the layout (physical:guest pairs)
   the keyboard is not remapped. pure C, no globals: tests compile it directly. */
#ifndef SWR_PAD_LAYOUT_H
#define SWR_PAD_LAYOUT_H
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { PAD_SWITCH=0, PAD_XBOX=1 };
typedef struct { uint8_t to[16]; int layout; int overrides; } PadMap;   /* to[physical bit] = guest bit, 0xff = none */

/* physical names: compass for the face buttons (any pad), Sony names as aliases, Switch names for the rest */
static const struct { const char* name; uint8_t bit; } PAD_PHYS[]={
  {"east",0},{"south",1},{"north",2},{"west",3},{"circle",0},{"cross",1},{"triangle",2},{"square",3},
  {"lstick",4},{"rstick",5},{"l",6},{"r",7},{"zl",8},{"zr",9},{"plus",10},{"minus",11},
  {"left",12},{"up",13},{"right",14},{"down",15}};
static const char* const PAD_GUEST[16]={"a","b","x","y","lstick","rstick","l","r","zl","zr","plus","minus","left","up","right","down"};
static const char* const PAD_POS[4]={"right","bottom","top","left"};

/* Linux: SDL2 game controller buttons (SDL names them by Xbox position: A bottom, B right, X left, Y top) to the
   positional mask; SDL_GAMECONTROLLER_USE_BUTTON_LABELS=0 keeps Nintendo pads positional too */
static const struct { int sdl, bit; } PAD_SDL[]={{1,0},{0,1},{3,2},{2,3},{7,4},{8,5},{9,6},{10,7},{6,10},{4,11},{13,12},{11,13},{14,14},{12,15}};

static int pad_layout_id(const char* v){
  if(!v || !*v || !strcmp(v,"switch") || !strcmp(v,"nintendo") || !strcmp(v,"position") || !strcmp(v,"positions")) return PAD_SWITCH;
  if(!strcmp(v,"xbox") || !strcmp(v,"label") || !strcmp(v,"labels")) return PAD_XBOX;
  return -1;
}
static int pad_name(const char* s,size_t n,int guest){
  if(guest){ if(n==4 && !strncmp(s,"none",4)) return 0xff;
    for(int i=0;i<16;i++) if(strlen(PAD_GUEST[i])==n && !strncmp(PAD_GUEST[i],s,n)) return i;
    return -1; }
  for(unsigned i=0;i<sizeof PAD_PHYS/sizeof PAD_PHYS[0];i++) if(strlen(PAD_PHYS[i].name)==n && !strncmp(PAD_PHYS[i].name,s,n)) return PAD_PHYS[i].bit;
  return -1;
}
/* builds the table; returns the number of rejected parts (unknown layout or bad map entries), naming the first in err */
static int pad_map_build(PadMap* m,const char* layout,const char* map,char* err,size_t errn){
  int bad=0; if(err && errn) err[0]=0;
  int id=pad_layout_id(layout); if(id<0){ bad++; if(err) snprintf(err,errn,"unknown button_layout '%s' (switch or xbox)",layout); id=PAD_SWITCH; }
  m->layout=id; m->overrides=0;
  for(int i=0;i<16;i++) m->to[i]=(uint8_t)i;
  if(id==PAD_XBOX){ m->to[0]=1; m->to[1]=0; m->to[2]=3; m->to[3]=2; }
  for(const char* p=map?map:"";*p;){
    while(*p==' ' || *p==',' || *p==';' || *p=='\t') p++;
    if(!*p) break;
    const char* t=p; while(*p && *p!=' ' && *p!=',' && *p!=';' && *p!='\t') p++;
    char tok[48]; size_t n=(size_t)(p-t); if(n>=sizeof tok) n=sizeof tok-1;
    for(size_t i=0;i<n;i++){ char c=t[i]; tok[i]=c>='A' && c<='Z'?(char)(c+32):c; } tok[n]=0;
    char* c=strchr(tok,':'); int ph=-1,gu=-1;
    if(c){ ph=pad_name(tok,(size_t)(c-tok),0); gu=pad_name(c+1,strlen(c+1),1); }
    if(ph<0 || gu<0){ if(!bad++ && err) snprintf(err,errn,"bad button_map entry '%s' (physical:guest, e.g. south:a)",tok); continue; }
    m->to[ph]=(uint8_t)gu; m->overrides++;
  }
  return bad;
}
static uint64_t pad_map_apply(const PadMap* m,uint64_t pos){
  uint64_t out=pos&~0xffffull;
  for(int i=0;i<16;i++) if((pos>>i&1) && m->to[i]!=0xff) out|=1ull<<m->to[i];
  return out;
}
/* "switch (A=right B=bottom X=top Y=left)" plus any overrides as physical:guest */
static void pad_map_describe(const PadMap* m,char* out,size_t n){
  size_t k=(size_t)snprintf(out,n,"%s (",m->layout==PAD_XBOX?"xbox":"switch");
  static const uint8_t base_x[4]={1,0,3,2};
  for(int g=0;g<4 && k<n;g++){ int ph=m->layout==PAD_XBOX?base_x[g]:g; k+=(size_t)snprintf(out+k,n-k,"%s%c=%s",g?" ":"","ABXY"[g],PAD_POS[ph]); }
  if(k<n) k+=(size_t)snprintf(out+k,n-k,")");
  if(m->overrides && k<n){ PadMap base; pad_map_build(&base,m->layout==PAD_XBOX?"xbox":"switch","",NULL,0);
    k+=(size_t)snprintf(out+k,n-k," button_map:");
    for(int i=0;i<16 && k<n;i++) if(m->to[i]!=base.to[i]){
      const char* ph="?"; for(unsigned j=0;j<sizeof PAD_PHYS/sizeof PAD_PHYS[0];j++) if(PAD_PHYS[j].bit==i){ ph=PAD_PHYS[j].name; break; }
      k+=(size_t)snprintf(out+k,n-k," %s:%s",ph,m->to[i]==0xff?"none":PAD_GUEST[m->to[i]]); } }
}
#endif
