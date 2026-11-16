/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the GNU Affero General Public License v3.0, see LICENSE.txt and NOTICE.txt.
   Forks and derived works have to keep the credit to SwitchRecomp by MZ Labs and the startup screen. */
/* standalone HID probe: shared read-only by default; --pulse is a bounded output check. */
#include <stdio.h>
#include <stdlib.h>
#define LOG(...) printf(__VA_ARGS__)
#include "../runtime/controller_sony_win32.h"

int main(int argc,char** argv){
  int pulse=argc==2 && !strcmp(argv[1],"--pulse"),seen=0,ok=1; uint64_t last=~0ull;
  if(argc>1 && !pulse){ fprintf(stderr,"usage: controller_device [--pulse]\n"); return 2; }
  if(!sony_open(&g_sony,pulse,1)){ puts("No supported Sony HID interface could be opened."); return 1; }
  ULONGLONG start=GetTickCount64();
  while(GetTickCount64()-start<2000 && g_sony.h){
    if(sony_poll(&g_sony)){
      seen=1;
      if(last!=g_sony.state.buttons){ last=g_sony.state.buttons;
        printf("device input: report=%02x buttons=%016llx sticks=%d,%d,%d,%d\n",g_sony.input[0],(unsigned long long)last,g_sony.state.lx,g_sony.state.ly,g_sony.state.rx,g_sony.state.ry); }
    }
    Sleep(4);
  }
  if(pulse && seen && g_sony.h && g_sony.writable){
    if(g_sony.model==SONY_DS5) ok=sony_write(&g_sony,0,0,0,1);
    if(ok) ok=sony_write(&g_sony,0,0,1,0);
    if(ok){ Sleep(500); ok=sony_write(&g_sony,24,16,1,0); Sleep(150); }
    ok=sony_write(&g_sony,0,0,1,0) && ok;
    Sleep(500);
  } else if(pulse) ok=0;
  printf("device check: valid_input=%d output_requested=%d output_ok=%d; physical response requires human observation\n",seen,pulse,pulse?ok:0);
  sony_close(&g_sony); return seen && ok?0:1;
}
