/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the GNU Affero General Public License v3.0, see LICENSE.txt and NOTICE.txt.
   Forks and derived works have to keep the credit to SwitchRecomp by MZ Labs and the startup screen. */
/* compile the actual WinMM mapper and vibration lifecycle; fake only the XInput device boundary. */
#include <assert.h>
#include "controller_backend_production.h"
static u64 clock_ns=1000000000; static XVIB output; static DWORD owner; static unsigned writes;
u64 plat_ns(void){ return clock_ns; }
void logf_(int lvl,const char* fmt,...){ (void)lvl; (void)fmt; }
static DWORD WINAPI send(DWORD user,XVIB* v){ owner=user; output=*v; writes++; return 0; }
/* every pad path reports by position; one physical press per face position (right, bottom, top, left) */
enum { RIGHT, BOTTOM, TOP, LEFT };
static u64 press_xinput(int pos){ static const WORD w[]={0x2000,0x1000,0x8000,0x4000}; return xinput_buttons(w[pos],0,0); } /* B A Y X */
static u64 press_winmm(int pos,int sony){ static const int bit[]={2,1,3,0}; return joy_buttons(1u<<bit[pos],sony); }       /* circle cross triangle square */
static u64 press_sony(int pos,int model,int transport){                                                                   /* native HID report */
  static const uint8_t f[]={0x40,0x20,0x80,0x10}; uint8_t r[64]={1,0x80,0x80,0x80,0x80}; r[1+(model==SONY_DS5?7:4)]=8|f[pos];
  SonyState s; assert(sony_input(model,transport,r,64,&s)); return s.buttons; }
static u64 press_sdl(int pos){ static const int b[]={1,0,3,2}; u64 m=0;                                                     /* SDL B A Y X */
  for(unsigned i=0;i<sizeof PAD_SDL/sizeof PAD_SDL[0];i++) if(PAD_SDL[i].sdl==b[pos]) m|=1ull<<PAD_SDL[i].bit; return m; }
static void layout_matrix(void){
  /* guest bit (A 0, B 1, X 2, Y 3) for each physical position */
  static const int sw[]={0,1,2,3},xbox[]={1,0,3,2};
  for(int layout=0;layout<2;layout++){ PadMap m; char err[160];
    assert(pad_map_build(&m,layout?"xbox":"switch","",err,sizeof err)==0 && !err[0]);
    for(int pos=0;pos<4;pos++){ u64 want=1ull<<(layout?xbox:sw)[pos];
      assert(pad_map_apply(&m,press_xinput(pos))==want);
      assert(pad_map_apply(&m,press_winmm(pos,0))==want && pad_map_apply(&m,press_winmm(pos,1))==want);
      assert(pad_map_apply(&m,press_sony(pos,SONY_DS5,SONY_USB))==want && pad_map_apply(&m,press_sony(pos,SONY_DS4,SONY_USB))==want);
      assert(pad_map_apply(&m,press_sdl(pos))==want); }
    for(int bit=4;bit<16;bit++) assert(pad_map_apply(&m,1ull<<bit)==1ull<<bit);  /* only the face buttons move */
    assert(pad_map_apply(&m,0xff0000ull)==0xff0000ull);                          /* stick-as-button bits pass through */
  }
  PadMap m; char err[160], d[400];
  /* aliases and the empty default */
  assert(pad_layout_id("nintendo")==PAD_SWITCH && pad_layout_id("position")==PAD_SWITCH && pad_layout_id("")==PAD_SWITCH && pad_layout_id("label")==PAD_XBOX && pad_layout_id("labels")==PAD_XBOX && pad_layout_id("wii")<0);
  /* button_map: case-insensitive physical:guest pairs on top of the layout; Sony names alias the compass ones */
  assert(pad_map_build(&m,"switch","South:A, circle:b  triangle:none",err,sizeof err)==0);
  assert(pad_map_apply(&m,press_sony(BOTTOM,SONY_DS5,SONY_USB))==1 && pad_map_apply(&m,press_sony(RIGHT,SONY_DS5,SONY_USB))==2);
  assert(pad_map_apply(&m,press_sony(TOP,SONY_DS5,SONY_USB))==0 && pad_map_apply(&m,press_xinput(LEFT))==8);
  assert(pad_map_build(&m,"xbox","zl:zr zr:zl minus:plus",err,sizeof err)==0);
  assert(pad_map_apply(&m,1ull<<8)==1ull<<9 && pad_map_apply(&m,1ull<<9)==1ull<<8 && pad_map_apply(&m,1ull<<11)==1ull<<10 && pad_map_apply(&m,1ull<<10)==1ull<<10);
  pad_map_describe(&m,d,sizeof d); assert(!strcmp(d,"xbox (A=bottom B=right X=left Y=top) button_map: zl:zr zr:zl minus:plus"));
  /* bad entries are reported and skipped, the rest still applies; an unknown layout falls back to switch */
  assert(pad_map_build(&m,"sega","east:q bogus west:b",err,sizeof err)==3 && strstr(err,"sega"));
  assert(m.layout==PAD_SWITCH && pad_map_apply(&m,press_xinput(LEFT))==2 && pad_map_apply(&m,press_xinput(RIGHT))==1);
  assert(pad_map_build(&m,"switch","east:q",err,sizeof err)==1 && strstr(err,"east:q"));
  pad_map_build(&m,"switch","",err,sizeof err); pad_map_describe(&m,d,sizeof d); assert(!strcmp(d,"switch (A=right B=bottom X=top Y=left)"));
}
int main(void){
  for(int sony=0;sony<2;sony++){
    const int raw[]={4,5,10,11,6,7,3,0},hid[]={6,7,4,5,8,9,2,3};
    for(int i=0;i<8;i++) assert(joy_buttons(1u<<raw[i],sony)==(1ull<<hid[i]));
  }
  /* by position for every pad, Sony included: circle (2) = A position, cross (1) = B position */
  assert(joy_buttons(4,1)==1 && joy_buttons(2,1)==2);
  assert(joy_buttons(4,0)==1 && joy_buttons(2,0)==2);
  layout_matrix();
  assert(joy_buttons(1u<<9,1)==1ull<<10 && joy_buttons(1u<<8,1)==1ull<<11);
  assert(joy_buttons(0,1)==0);
  /* startup: stopping before XInput discovery must not touch an uninitialized lock. */
  backend_vibration_stop(); backend_vibration_set(.5f,.5f); hid_vibration_flush(); assert(writes==0);
  xset=send; g_xuser=2; g_vib_accept=1;
  backend_vibration_set(2,-1); hid_vibration_flush(); assert(writes==1 && owner==2 && output.wLeftMotorSpeed==65535 && output.wRightMotorSpeed==0);
  backend_vibration_set(.25f,.75f); hid_vibration_flush(); assert(writes==1); /* nonzero coalesces */
  clock_ns+=16000000; hid_vibration_flush(); assert(writes==2 && output.wLeftMotorSpeed==16384 && output.wRightMotorSpeed==49151);
  backend_vibration_stop(); hid_vibration_flush(); assert(writes==3 && !output.wLeftMotorSpeed && !output.wRightMotorSpeed); /* stop bypasses throttle */
  clock_ns+=16000000; backend_vibration_set(NAN,INFINITY); hid_vibration_flush(); assert(writes==3);
  backend_vibration_set(.5f,.5f); hid_vibration_flush(); assert(writes==4);
  controllers_shutdown(); assert(writes==5 && owner==2 && !output.wLeftMotorSpeed && !output.wRightMotorSpeed);
  backend_vibration_set(1,1); hid_vibration_flush(); assert(writes==5 && !g_vib_left && !g_vib_right);
  puts("PASS button layouts switch/xbox x XInput, WinMM generic + Sony, Sony HID DS5 + DS4, SDL table; button_map overrides, aliases, errors, startup description");
  puts("PASS production WinMM mapping, startup synchronization, amplitude bounds, output owner, rate limit, immediate stop and shutdown (mock device)"); return 0;
}
