/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the GNU Affero General Public License v3.0, see LICENSE.txt and NOTICE.txt.
   Forks and derived works have to keep the credit to SwitchRecomp by MZ Labs and the startup screen. */
/* execute the production service dispatch and shared-memory writer with host-only kernel stubs. */
#include <assert.h>
#include "../runtime/svc_hid.c"
static float sent_low,sent_high; static int sent,stopped;
u8* g_mem;
void backend_vibration_set(float l,float h){ sent_low=l; sent_high=h; sent++; }
void backend_vibration_stop(void){ sent_low=sent_high=0; stopped++; }
void pm_init(PMutex* m){ InitializeSRWLock(&m->l); }
void pm_lock(PMutex* m){ AcquireSRWLockExclusive(&m->l); }
void pm_unlock(PMutex* m){ ReleaseSRWLockExclusive(&m->l); }
void logf_(int level,const char* fmt,...){ (void)level; (void)fmt; }
int stub_ok(SvcObj* o,IpcCtx* c){ (void)o; (void)c; return 0; }
u32 h_new(KObj* o){ (void)o; return 1; }
void kobj_ref(KObj* o){ (void)o; }
KEvent* kevent_new(const char* n,int a){ static KEvent e; (void)n; (void)a; return &e; }
void kevent_signal(KEvent* e){ (void)e; }
KShmem* kshmem_new(u64 size,const char* name){ static KShmem s; (void)name; s.size=size; s.host=calloc(1,size); return &s; }
u8* kshmem_ptr(KShmem* s){ return s->host; }
void out_copy_h(IpcCtx* c,u32 h){ (void)c; (void)h; }
void out_obj(IpcCtx* c,SvcObj* o){ (void)c; (void)o; }
SvcObj* svcobj_new(const char* n,int (*d)(SvcObj*,IpcCtx*),size_t size){ static SvcObj o; (void)n; (void)d; (void)size; return &o; }
void svc_register(const char* n,SvcCtor ctor){ (void)n; (void)ctor; }
int buf_xs(IpcCtx* c,int i,u64* a,u64* s){ if(i>=c->nx) return 0; *a=c->x[i].addr; *s=c->x[i].size; return 1; }
static u64 read64(const u8* p){ u64 n; memcpy(&n,p,8); return n; }
static void command(unsigned cmd,const void* raw,unsigned n){ IpcCtx c={0}; c.cmd=cmd; c.raw=raw; c.raw_size=n; assert(hid_disp(NULL,&c)==0); }
static void activate(u32 h){ IpcCtx c={0}; c.raw=(u8*)&h; c.raw_size=4; hid_vibration_list(NULL,&c); }
int main(void){
  svc_hid_init(); g_mem=calloc(1,1024);
  assert(vib_side(0x00000003)==0 && vib_side(0x00010003)==1);
  assert(vib_side(0x00002004)==0 && vib_side(0x00012004)==1);
  assert(vib_side(5)==0 && vib_side(0x10005)==1);
  assert(vib_side(6)==0 && vib_side(0x10007)==1);
  for(unsigned h=0;h<0x40000;h++){
    unsigned style=h&255,id=(h>>8)&255,dev=h>>16;
    int valid=dev<2 && ((id==0 && (style==3 || style==5 || (style==6 && dev==0) || (style==7 && dev==1))) || (id==0x20 && style==4));
    assert((vib_side(h)>=0)==valid);
  }
  assert(vib_side(0x01000003)<0);
  activate(3); activate(0x10003);
  struct { u32 h; float v[4]; u32 pad; u64 aruid; } req={3,{.25f,160.f,.5f,320.f},0,0};
  command(201,&req,sizeof req); assert(sent==1 && sent_low==.25f && sent_high==.5f);
  command(201,&req,19); assert(sent==1);
  req.h=0x300; command(201,&req,sizeof req); assert(sent==1);
  req.h=3; req.v[0]=NAN; req.v[1]=INFINITY; req.v[2]=2;
  command(201,&req,sizeof req); assert(sent_low==0 && sent_high==1 && g_vib[0].lowf==160);
  u32 handles[2]={3,0x10003}; float values[8]={.2f,160,.3f,320,.7f,160,.8f,320};
  memcpy(g_mem+16,handles,sizeof handles); memcpy(g_mem+64,values,sizeof values);
  IpcCtx c={0}; c.cmd=206; c.nx=2; c.x[0]=(IpcBuf){16,8,0}; c.x[1]=(IpcBuf){64,32,0};
  int before=sent; hid_disp(NULL,&c); assert(sent==before+1 && sent_low==.7f && sent_high==.8f);
  c.x[1].size=15; before=sent; hid_disp(NULL,&c); assert(sent==before);
  c.x[1].size=16; values[0]=.9f; memcpy(g_mem+64,values,16); hid_disp(NULL,&c); assert(sent_low==.9f && sent_high==.8f);
  /* Switch 2 SendVibrationValuesInMode (214): raw u64 mode + aruid, same X buffers as 206; MKW/DKB send "2 0 1234 0". */
  { u32 raw214[4]={2,0,0x1234,0}; values[0]=.15f; values[2]=.35f; values[4]=.05f; values[6]=.25f; memcpy(g_mem+64,values,sizeof values);
    IpcCtx m={0}; m.cmd=214; m.raw=(u8*)raw214; m.raw_size=16; m.nx=2; m.x[0]=(IpcBuf){16,8,0}; m.x[1]=(IpcBuf){64,32,0};
    before=sent; assert(hid_disp(NULL,&m)==0 && sent==before+1 && sent_low==.15f && sent_high==.35f);
    m.nx=1; before=sent; hid_disp(NULL,&m); assert(sent==before); }
  /* 213: 0x18 raw bytes then aruid; value at 0x08, handle at 0x04 (mode first) or at 0x00 */
  { struct { u32 a,b; float v[4]; u64 aruid; } r213={2,0x10003,{.45f,160,.55f,320},0x1234};
    command(213,&r213,sizeof r213); assert(sent_low==.45f && g_vib[1].high==.55f);
    r213.a=3; r213.b=2; r213.v[0]=.65f; command(213,&r213,sizeof r213); assert(g_vib[0].low==.65f);
    r213.a=0x300; r213.b=0x300; before=sent; command(213,&r213,sizeof r213); assert(sent==before);
    before=sent; command(213,&r213,0x17); assert(sent==before); }
  { u32 h=0x10003; IpcCtx m={0}; m.cmd=220; m.raw=(u8*)&h; m.raw_size=4; hid_disp(NULL,&m); assert(m.out_size==1 && m.out[0]==1);
    h=0x300; IpcCtx bad={0}; bad.cmd=220; bad.raw=(u8*)&h; bad.raw_size=4; hid_disp(NULL,&bad); assert(bad.out_size==1 && bad.out[0]==0); }
  u8 permit=0; command(204,&permit,1); assert(stopped==1 && sent_low==0 && sent_high==0);
  before=sent; command(201,&req,sizeof req); assert(sent==before);
  permit=1; command(204,&permit,1); req.v[0]=.4f; req.v[2]=.6f;
  command(201,&req,sizeof req); assert(sent_low==.4f && sent_high==.6f); /* activation survives permission toggle */
  IpcCtx actual={0}; actual.cmd=202; actual.raw=(u8*)&req.h; actual.raw_size=4; hid_disp(NULL,&actual);
  assert(actual.out_size==16 && !memcmp(actual.out,req.v,4));
  /* g_pad -> all active rings, including wrap, release and stick direction bits. */
  g_pad.buttons=(1ull<<4)|(1ull<<6)|(1ull<<7); g_pad.lx=-32767; g_pad.ly=32767; g_pad.rx=0; g_pad.ry=-32767;
  for(int i=0;i<40;i++) hid_update();
  u8* base=kshmem_ptr(g_hid_shm)+HO_HidSharedMemory_npad;
  const unsigned rings[]={HO_HidNpadInternalState_full_key_lifo,HO_HidNpadInternalState_joy_dual_lifo,HO_HidNpadInternalState_handheld_lifo};
  for(int r=0;r<3;r++){
    u8* ring=base+rings[r]; u64 tail=read64(ring+0x10); assert(tail<17 && read64(ring+0x18)==16);
    u8* e=ring+0x20+tail*HS_HidNpadCommonStateAtomicStorage;
    assert(read64(e)==g_samp*2 && read64(e+8)==g_samp);
    assert(read64(e+8+HO_HidNpadCommonState_buttons)==(g_pad.buttons|(1ull<<16)|(1ull<<17)|(1ull<<23)));
    assert(!memcmp(e+8+HO_HidNpadCommonState_analog_stick_l,&g_pad.lx,16));
  }
  memset(&g_pad,0,sizeof g_pad); hid_update();
  u8* ring=base+HO_HidNpadInternalState_full_key_lifo;
  assert(read64(ring+0x20+read64(ring+0x10)*HS_HidNpadCommonStateAtomicStorage+8+HO_HidNpadCommonState_buttons)==0);
  puts("PASS production HID handle layout, inline/batch dispatch incl. Switch 2 commands 213/214/220, bounds, amplitude clamp, permission, activation and shared-memory rings (synthetic)");
  free(g_mem); free(g_hid_shm->host); return 0;
}
