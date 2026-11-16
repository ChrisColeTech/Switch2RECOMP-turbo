/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the GNU Affero General Public License v3.0, see LICENSE.txt and NOTICE.txt.
   Forks and derived works have to keep the credit to SwitchRecomp by MZ Labs and the startup screen. */
/* standalone HID probe using the runtime's own Sony code (controller_sony_win32.h).
     (no option)  shared read-only: open the pad and decode 2 s of input reports
     --pulse      bounded output check: lights on, a short weak rumble, stop
     --rumble     owner's physical check: lights on, low motor, high motor, both, stop, lights off (about 5 s)
     --buttons N  shared read-only, N seconds (default 20): every physical press prints the Switch button it becomes
                  under button_layout=nintendo and =xbox (pad_layout.h, the runtime's own mapping)
   every output write prints its report bytes and the WriteFile result. */
#include <stdio.h>
#include <stdlib.h>
#define LOG(...) printf(__VA_ARGS__)
#include "../runtime/controller_sony_win32.h"
#include "../runtime/pad_layout.h"

static const char* const SONY_NAMES[16]={"Circle (right)","Cross (bottom)","Triangle (top)","Square (left)","L3","R3","L1","R1",
  "L2","R2","Options","Create","D-pad left","D-pad up","D-pad right","D-pad down"};
static int buttons(unsigned seconds){
  PadMap n,x; pad_map_build(&n,"nintendo","",NULL,0); pad_map_build(&x,"xbox","",NULL,0);
  printf("Press each button (%u s). Physical button -> Switch button with button_layout=nintendo | xbox\n",seconds); fflush(stdout);
  ULONGLONG start=GetTickCount64(); uint64_t last=0; int seen=0;
  while(GetTickCount64()-start<seconds*1000ull && g_sony.h){
    if(sony_poll(&g_sony)){ seen=1; uint64_t b=g_sony.state.buttons&0xffff,down=b&~last; last=b;
      for(int i=0;i<16;i++) if(down>>i&1){ uint64_t gn=pad_map_apply(&n,1ull<<i),gx=pad_map_apply(&x,1ull<<i); int a=0,c=0;
        while(gn>>a>1) a++; while(gx>>c>1) c++;
        printf("  %-15s -> nintendo: %-6s xbox: %s\n",SONY_NAMES[i],PAD_GUEST[a],PAD_GUEST[c]); fflush(stdout); } }
    Sleep(4);
  }
  printf("buttons check: valid_input=%d\n",seen); sony_close(&g_sony); return seen?0:1;
}

static int put(unsigned low,unsigned high,int lit,int reset,const char* what){
  int ok=sony_write(&g_sony,low,high,lit,reset);
  printf("  %-34s write=%s bytes=%u error=%lu  report:",what,ok?"OK":"FAILED",g_sony.last_len,(unsigned long)g_sony.last_error);
  for(unsigned i=0;i<g_sony.last_len;i++) printf("%s%02x",i%16?" ":"\n    ",g_sony.last_packet[i]);
  printf("\n"); fflush(stdout); return ok;
}

int main(int argc,char** argv){
  int pulse=argc==2 && !strcmp(argv[1],"--pulse"),rumble=argc==2 && !strcmp(argv[1],"--rumble"),seen=0,ok=1; uint64_t last=~0ull;
  int press=argc>=2 && !strcmp(argv[1],"--buttons");
  if(argc>1 && !pulse && !rumble && !press){ fprintf(stderr,"usage: controller_device [--pulse | --rumble | --buttons [seconds]]\n"); return 2; }
  if(press){ if(!sony_open(&g_sony,0,1)){ puts("No supported Sony HID interface could be opened."); return 1; }
    return buttons(argc>2?(unsigned)atoi(argv[2]):20); }
  int output=pulse||rumble;
  if(!sony_open(&g_sony,output,1)){ puts("No supported Sony HID interface could be opened (DualSense 054c:0ce6/0df2, DualShock 4 054c:05c4/09cc)."); return 1; }
  ULONGLONG start=GetTickCount64();
  while(GetTickCount64()-start<(rumble?500:2000) && g_sony.h){
    if(sony_poll(&g_sony)){
      seen=1;
      if(last!=g_sony.state.buttons){ last=g_sony.state.buttons;
        printf("device input: report=%02x buttons=%016llx sticks=%d,%d,%d,%d\n",g_sony.input[0],(unsigned long long)last,g_sony.state.lx,g_sony.state.ly,g_sony.state.rx,g_sony.state.ry); }
    }
    Sleep(4);
  }
  if(output && (!seen || !g_sony.h || !g_sony.writable)){
    printf("device check: valid_input=%d writable=%d: no output sent\n",seen,g_sony.h?g_sony.writable:0); ok=0;
  } else if(pulse){
    if(g_sony.model==SONY_DS5) ok=put(0,0,0,1,"lightbar animation release");
    if(ok) ok=put(0,0,1,0,"lights on");
    if(ok){ Sleep(500); ok=put(24,16,1,0,"rumble 24/16"); Sleep(150); }
    ok=put(0,0,1,0,"rumble stop") && ok;
    Sleep(500);
  } else if(rumble){
    puts("Watch and hold the controller: blue lightbar + centre player LED, then LOW motor (left grip), HIGH motor (right grip), both, then everything off.");
    if(g_sony.model==SONY_DS5) ok=put(0,0,0,1,"lightbar animation release");
    ok=put(0,0,1,0,"lights on (player 1, blue)") && ok; Sleep(1000);
    ok=put(160,0,1,0,"low-frequency motor 160/255") && ok; Sleep(1000);
    ok=put(0,160,1,0,"high-frequency motor 160/255") && ok; Sleep(1000);
    ok=put(200,200,1,0,"both motors 200/255") && ok; Sleep(1000);
    ok=put(0,0,1,0,"motors stop, lights stay") && ok; Sleep(700);
    ok=put(0,0,0,0,"lights off") && ok;
  }
  printf("device check: valid_input=%d output_requested=%d output_ok=%d; physical response requires human observation\n",seen,output,output?ok:0);
  sony_close(&g_sony); return seen && ok?0:1;
}
