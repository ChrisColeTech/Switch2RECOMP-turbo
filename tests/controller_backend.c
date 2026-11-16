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
int main(void){
  for(int sony=0;sony<2;sony++){
    const int raw[]={4,5,10,11,6,7,3,0},hid[]={6,7,4,5,8,9,2,3};
    for(int i=0;i<8;i++) assert(joy_buttons(1u<<raw[i],sony)==(1ull<<hid[i]));
  }
  assert(joy_buttons(2,1)==1 && joy_buttons(4,1)==2);
  assert(joy_buttons(4,0)==1 && joy_buttons(2,0)==2);
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
  puts("PASS production WinMM mapping, startup synchronization, amplitude bounds, output owner, rate limit, immediate stop and shutdown (mock device)"); return 0;
}
