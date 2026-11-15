/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the GNU Affero General Public License v3.0, see LICENSE.txt and NOTICE.txt.
   Forks and derived works have to keep the credit to SwitchRecomp by MZ Labs and the startup screen. */
/* Switch 2 graphics services. A Switch 2 game reaches the GPU through ISelfController 2000 (GetGraphicsSupervisor),
   not through nvdrv: the supervisor (TIPC) bootstraps, creates native memory and images, and opens sessions for the
   GPU (address spaces, devices, a worker that maps GPU memory), syncpoints, display ("vi2") and an nvdrv session of
   its own (1402). Underneath it all is our nvdrv: its nvmap handles, GPU page table and channels.
   Requests are TIPC: W(c,0) is the header word (16 + command), W(c,1) the size word, the arguments follow. Results go
   back as the first payload word (the dispatch return value) */
#include "rt.h"
#ifdef SWR_HAVE_VP9
#include <vpx/vpx_decoder.h>
#include <vpx/vp8dx.h>
#endif

extern SvcObj* nvdrv_obj_new(const char* name);
extern u32 nv_native_handle(u64 addr, u64 size);
extern void nv_gpu_map_raw(u64 gva, u64 cpu, u64 size), nv_gpu_unmap_raw(u64 gva, u64 size);
extern int nv_native_bind(u32 h, u64 addr), nv_native_alive(u32 h), nv_native_map(u64 gva, u32 h, u64 off, u64 size);
extern void nv_native_free(u32 h), nv_native_unmap(u64 gva, u64 size);
extern int nv_open_dev(const char* name), nv_fd_is_as(u32 fd);
extern u32 syncpt_read(u32 id), syncpt_alloc(void); extern void syncpt_incr(u32 id); extern int syncpt_wait(u32 id, u32 thresh, i64 timeout);
extern u64 nvmap_addr(u32 h); extern u32 nvmap_size(u32 h);
extern void vi_show_addr(u64 a, u32 w, u32 h, u32 pitch, u32 layout, u32 bhl, u32 fmt);
extern int ipc_warn_once(const char* name, u32 cmd);

/* kernel results (module 1) */
#define K_INVSIZE   0xCA01
#define K_INVADDR   0xCC01
#define K_OUTOFRES  0xCE01
#define K_INVMEM    0xD401
#define K_NOTIMPL   0xF001
#define K_INVSTATE  0xFA01
#define K_NOTFOUND  0xF201
#define K_INVCOMB   0xE801
#define K_BUSY      0xFC01

static u32 W(IpcCtx* c, u32 i){ u32 v; memcpy(&v,c->msg+4*i,4); return v; }
static u64 W64(IpcCtx* c, u32 i){ return (u64)W(c,i)|((u64)W(c,i+1)<<32); }
static u32 ev_h(KEvent* e){ return h_new(&e->o); }
/* Switch 2 services fail soft: what this file doesn't know is logged and answered "ok" with zeros, so the game goes
   on and one log shows several missing pieces instead of stopping at the first one. SWR_NS2_STRICT=1 answers
   NotImplemented like the system */
static int ns2_strict(void){ static int v=-1; if(v<0) v=getenv("SWR_NS2_STRICT")!=NULL; return v; }
static int soft_worker_disp(SvcObj*,IpcCtx*); static int soft_sync_disp(SvcObj*,IpcCtx*); static int soft_vi2_disp(SvcObj*,IpcCtx*);
static int soft_gpusess_disp(SvcObj*,IpcCtx*); static int soft_gsv_disp(SvcObj*,IpcCtx*);
/* a command this file doesn't know */
static int ns2_unknown(SvcObj* o, IpcCtx* c){
  /* command 0 with a 64 bit word is a version handshake: the sdk sends its version and aborts unless the same value
     comes back (nn::syncpt::sdk::Initialize does it on the syncpoint session) */
  if(c->cmd==0 && (W(c,1)&0x3FF)>=2){ u64 v=W64(c,2);
    if(ipc_warn_once(o->name,0)) LOG("switch 2: %s handshake %llx\n",o->name,(unsigned long long)v);
    out64(c,v); return 0; }
  if(ipc_warn_once(o->name,c->cmd)){ u32 n=W(c,1)&0x3FF; if(n>12) n=12; char b[160]; int k=0; for(u32 i=0;i<n;i++) k+=snprintf(b+k,sizeof b-k," %x",W(c,2+i));
    WARN("  [switch 2] %s cmd %u not implemented (words%s)%s\n",o->name,c->cmd,k? b : " none",ns2_strict()? "" : ", answered ok with zeros"); }
  if(ns2_strict()) return K_NOTIMPL;
  for(int i=0;i<4;i++) out64(c,0);   /* the game reads its outputs as zeros instead of aborting on NotImplemented */
  return 0; }
/* a check this file doesn't know how to answer (NotImplemented) becomes success too, logged once per command */
static int ns2_soft(SvcObj* o, IpcCtx* c, int rc){
  if(rc!=K_NOTIMPL || ns2_strict()) return rc;
  if(ipc_warn_once("ns2-soft",((u32)(uintptr_t)o->name<<8)^c->cmd)) WARN("  [switch 2] %s cmd %u: case not handled, answered ok\n",o->name,c->cmd);
  return 0; }
/* a session handed back as a move handle; on failure the slot still goes out (the SDK reads it before the result) */
static int move_obj(IpcCtx* c, SvcObj* o, int rc){ out_move_h(c, rc? 0 : session_new(o)); if(rc && o) svcobj_unref(o); return rc; }

/* ============================================================ shared state (one process) */
static PMutex g_ns2; static int g_ns2_init;
static void ns2_lock(void){ if(!g_ns2_init){ pm_init(&g_ns2); g_ns2_init=1; } pm_lock(&g_ns2); }
static void ns2_unlock(void){ pm_unlock(&g_ns2); }

typedef struct { u32 id, mem; u32 pref[16]; u64 size; int window_pool; u64 off; int decoder; } NsImage;   /* off: where in mem (image pools); decoder: decoder image pool (gsv 911) */
static NsImage g_img[1024]; static u32 g_nimg, g_next_img=1;
static u32 g_native_mem[1024]; static u32 g_nnative;
static int g_phase;              /* 0 created, 1 initialized, 2 process bound, 3 ready */
static int g_gpu_ctx, g_gpu_ready, g_frame_ctx, g_img_ctx, g_img_formats, g_disp_ctx, g_sync_ctx;

static int native_mem_has(u32 id){ for(u32 i=0;i<g_nnative;i++) if(g_native_mem[i]==id) return 1; return 0; }
static NsImage* image_find(u32 id){ for(u32 i=0;i<g_nimg;i++) if(g_img[i].id==id) return &g_img[i]; return NULL; }

/* the size of a native image: four-byte color, row pitch in GOBs, block height 2^pref[4] pixels */
/* a layout image_size doesn't know: a size that's surely big enough (16 bytes a pixel unless the pitch says less, 128
   line blocks, every layer and twice that for mips), so the game gets memory instead of an abort */
static int image_size(const u32* p, u32 count, u64* size, u64* align);
static int image_size_any(const u32* p, u32 count, u64* size, u64* align){
  if(image_size(p,count,size,align)) return 1;
  { static int said; if(said<sw2_cap(16)){ said++; WARN("switch 2: image layout not handled (count %u):",count); for(int i=0;i<16;i++) WARN(" %x",p[i]); WARN("%s\n",ns2_strict()? "" : ", size estimated"); } }
  if(ns2_strict()) return 0;
  u64 w=(p[2]&0xFFFF)+1, h=(p[2]>>16)+1, pitch= p[5]>=(w*4+63)/64 && p[5]<=4096? (u64)p[5]*64 : w*16;
  u64 layers=(count? count : 1), one=pitch*((h+127)&~127ull);
  *size=((one*layers*(p[7]>1? 2 : 1))+0xFFFF)&~0xFFFFull; if(align) *align=0x10000; return 1; }

/* p[6] isn't always 1: Mario Kart World's window asks for its 3840x2160 textures with 4 there. It doesn't make the
   image bigger: the window places its textures one image size apart, and with 4x the size its memory pool came to
   382 MB, more than the game's 117 MB GraphicsSystem heap (HeapDef), so the pool got no memory and the window aborted.
   SWR_NS2_IMG_LAYERS=1 multiplies by it again */
static int image_size(const u32* p, u32 count, u64* size, u64* align){
  if(count!=1 || p[0]!=0 || (p[1]!=1 && p[1]!=2) || p[3]!=0 || !p[6] || p[6]>16 || p[7]!=1 || p[4]>8) return 0;
  for(int i=8;i<16;i++) if(p[i]) return 0;
  u64 w=(p[2]&0xFFFF)+1, h=(p[2]>>16)+1, gobs=p[5]; if(gobs<(w*4+63)/64 || gobs>4096) return 0;
  u64 bh=1ull<<p[4]; u64 one=gobs*64*((h+bh-1)&~(bh-1)); one=(one+0xFFF)&~0xFFFull;
  *size= getenv("SWR_NS2_IMG_LAYERS")? one*p[6] : one; if(align){ *align=64*bh; if(*align<0x1000) *align=0x1000; } return 1; }

/* ============================================================ syncpoint cache: 192 counters the GPU's syncpoints
   are copied into, in two transfer memories (requests in rw, values and acks in ro), refreshed every millisecond */
#define SC_N 192
static u64 g_sc_rw, g_sc_ro; static KEvent *g_sc_req, *g_sc_done; static u64 g_sc_prev[SC_N];
static void sc_refresh(void){
  if(!g_sc_rw) return;
  u64 req=gr64(g_sc_rw), ack=gr64(g_sc_ro+0x640), groups=(req^ack)&7; if(!groups) return;
  for(u32 g=0;g<3;g++){ if(!(groups>>g&1)) continue;
    u64 want=gr64(g_sc_rw+8+g*8), slots=want^gr64(g_sc_ro+0x648+g*8);
    for(u32 b=0;b<64;b++) if(slots>>b&1){ u32 id=g*64+b; u32 cur=syncpt_read(id); u64 prev=g_sc_prev[id];
      u64 v= prev==~0ull? cur : ((prev&0xFFFFFFFF00000000ull)+((u32)cur<(u32)prev? 1ull<<32 : 0))|cur; g_sc_prev[id]=v; gw64(g_sc_ro+0x40+id*8,v); }
    __atomic_thread_fence(__ATOMIC_RELEASE); gw64(g_sc_ro+0x648+g*8,want); }
  __atomic_thread_fence(__ATOMIC_RELEASE); gw64(g_sc_ro+0x640,req);
  kevent_clear(g_sc_req); kevent_signal(g_sc_done); }
/* syncpoint event slots (ISyncpointSupervisor 22/201/202): 201 asks for the slot's event once a syncpoint reaches a
   value, 202 cancels and always signals (nn::syncpt::sdk::CancelSyncPointEvent waits on it either way) */
static struct { KEvent* ev; u32 id, val; int armed; } g_sps[64];
static KEvent* sps_ev(u32 s){ s&=63; if(!g_sps[s].ev){ g_sps[s].ev=kevent_new("syncpoint slot",0); kobj_ref(&g_sps[s].ev->o); } return g_sps[s].ev; }
static void sps_poll(void){ for(int i=0;i<64;i++) if(g_sps[i].armed && (i32)(syncpt_read(g_sps[i].id)-g_sps[i].val)>=0){ g_sps[i].armed=0; kevent_signal(sps_ev(i)); } }
/* vi2 31/32 events: the display's vsync, every 16.7 ms (the main thread waits on 32 before its first frame) */
static KEvent* g_vsync[2];
static void sc_thread(void* a){ (void)a; u64 next=plat_ns()+16666667;
  for(;;){ plat_sleep_ns(1000000); ns2_lock(); sc_refresh(); sps_poll();
    if(plat_ns()>=next){ next+=16666667; if(plat_ns()>next) next=plat_ns()+16666667; for(int i=0;i<2;i++) if(g_vsync[i]) kevent_signal(g_vsync[i]); }
    ns2_unlock(); } }

static int sync_disp(SvcObj* o, IpcCtx* c){   /* ISyncpointSupervisor */
  switch(c->cmd){
    case 1: out32(c,63); return 0;                                      /* GetMaximumEventCount */
    case 21: return W(c,2)<=63? 0 : K_INVCOMB;                          /* SetEventCount */
    case 103: { u32 id=~W(c,2); out64(c,syncpt_read(id)); out32(c,0); return 0; }   /* QueryCounter */
    case 22: ns2_lock(); out_move_h(c,ev_h(sps_ev(W(c,2)))); ns2_unlock(); return 0;   /* the slot's event */
    case 201: { u32 s=W(c,2)&63; ns2_lock(); KEvent* e=sps_ev(s); kevent_clear(e); g_sps[s].id=~W(c,3); g_sps[s].val=W(c,4); g_sps[s].armed=1; sps_poll(); ns2_unlock(); return 0; }
    case 202: { u32 s=W(c,2)&63; ns2_lock(); g_sps[s].armed=0; kevent_signal(sps_ev(s)); ns2_unlock(); return 0; }
  }
  return ns2_unknown(o,c); }

/* ============================================================ GPU session, worker */
typedef struct { int as[8], as_reg[8]; u64 dev_name[256]; u8 dev_used[256]; KEvent* ev[64]; SvcObj* worker; } GpuDevs;
static GpuDevs g_dev;
typedef struct { u32 as; u64 gva, size; u32 mem; } GpuMap;
static GpuMap g_maps[32768]; static u32 g_nmaps;
typedef struct { u64 gva, size, cpu; } Sparse; static Sparse g_sparse[64]; static u32 g_nsparse;
/* memory behind sparse regions, at most 2 GB in all (SWR_SPARSE_MB): Windows commits all of it, and Mario Kart World's
   ~8 GB of regions on top of its 7.6 GB heap ran PCs out of commit, the next allocations failed and the runtime crashed.
   The renderer keeps render targets in host textures, a region left unbacked only loses CPU reads of them */
static u64 g_sparse_mb; static u64 sparse_budget_mb(void){ static u64 v; if(!v){ const char* e=getenv("SWR_SPARSE_MB"); v= e? strtoull(e,NULL,10) : 2048; if(!v) v=1; } return v; } static u32 g_wmem[16384]; static u32 g_nwmem;

static int as_owned(u32 id){ for(int i=0;i<8;i++) if(g_dev.as[i]==(int)id && g_dev.as_reg[i]) return 1; return 0; }
static void ev_sig(u32 i){ if(i<64 && g_dev.ev[i]) kevent_signal(g_dev.ev[i]); }
/* memory kinds: equal requested/fallback ones need no compression; compressible ones fall back */
static int kind_select(u8 req, u8 fb, u8* sel){
  if(req==fb && (req==0 || req==1 || req==3 || req==6)){ *sel=req; return 1; }
  if((req==8 && fb==6) || (req==0xB && fb==1) || (req==0xC && fb==3)){ *sel=fb; return 1; }
  return 0; }
/* guest memory the GPU may use carries attribute 0x20 (svc 0x80) */
static int gpu_accessible(u64 a, u64 end){
  for(u64 p=a;p<=end;){ MemBlock b; mem_query(p,&b); if(!b.size || !(b.attr&0x20)) return 0; u64 n=b.addr+b.size; if(n<=p) return 0; p=n; }
  return 1; }

static int worker_disp(SvcObj* o, IpcCtx* c){   /* IGpuWorker */
  switch(c->cmd){
    case 101: { /* CreateGpuAccessibleMemory {begin, end, flags u8, device u8} */
      u64 b=W64(c,2), e=W64(c,4); u32 fl=W(c,6)&0xFF, dv=W(c,7)&0xFF; u32 id=0; int rc;
      if(fl) rc=K_NOTIMPL; else if(!g_dev.dev_used[dv]) rc=K_NOTFOUND; else if(e<b || e==~0ull) rc=K_INVSIZE;
      else if((b|(e-b+1))&0xFFF) rc=(b&0xFFF)? K_INVADDR : K_INVSIZE; else if(g_nwmem>=16384) rc=K_OUTOFRES;
      else if(!gpu_accessible(b,e)) rc=K_INVMEM;
      else { id=nv_native_handle(b,e-b+1); rc= id? 0 : K_OUTOFRES; if(id) g_wmem[g_nwmem++]=id; }
      if(rc){ static int said; if(said<sw2_cap(8)){ said++; LOG("switch 2: GPU-accessible memory %llx-%llx refused (%x, %u registered)\n",(unsigned long long)b,(unsigned long long)e,rc,g_nwmem); } }
      out32(c,id); return rc; }
    case 102: { /* DestroyGpuAccessibleMemory {id, notification} */
      u32 id=W(c,2), n=W(c,3); for(u32 i=0;i<g_nwmem;i++) if(g_wmem[i]==id){ nv_native_free(id); g_wmem[i]=g_wmem[--g_nwmem]; ev_sig(n); return 0; }
      return K_NOTFOUND; }
    case 211: { /* NormalMapGpuMemory4K {as, begin, end, memory, offset, ..., kind, fallback, ..., device, ..., notification} */
      u32 as=W(c,2), mem=W(c,7), dv=W(c,15)&0xFF, note=W(c,17); u64 b=W64(c,3), e=W64(c,5), off=W64(c,8); u8 sel=0; int rc;
      if(!as_owned(as) || !nv_native_alive(mem) || !g_dev.dev_used[dv]) rc=K_NOTFOUND;
      else if(!kind_select((u8)W(c,12),(u8)W(c,13),&sel)) rc=K_NOTIMPL;
      else if(e<b || e==~0ull) rc=K_INVSIZE; else if(g_nmaps>=32768) rc=K_OUTOFRES;
      else { rc= nv_native_map(b,mem,off,e-b+1)? K_INVMEM : 0; if(!rc) g_maps[g_nmaps++]=(GpuMap){as,b,e-b+1,mem}; }
      ev_sig(note);
      { static int said; if(said<sw2_cap(6)){ said++; LOG("switch 2: GPU map %llx-%llx -> memory %u + %llx, kind %x (%x)\n",(unsigned long long)b,(unsigned long long)e,mem,(unsigned long long)off,sel,rc); } }
      out32(c,sel); return rc; }
    case 212: { /* NormalModifyGpuMemory4K: the kind only (our page table has none) */
      u64 b=W64(c,3), e=W64(c,5); u8 sel=0; int rc=K_NOTFOUND;
      if(kind_select((u8)W(c,7),(u8)W(c,8),&sel)) for(u32 i=0;i<g_nmaps;i++) if(g_maps[i].as==W(c,2) && b>=g_maps[i].gva && e<g_maps[i].gva+g_maps[i].size){ rc=0; break; }
      ev_sig(W(c,10)); out32(c,sel); return rc; }
    /* 214 CreateSparseRegion {as, first, last, ...}, done through the event in word 11; 215 destroys it. The graphics
       server backs these itself on the console (render targets live there, Mario Kart World makes several). Here
       each gets memory of its own up to 2 GB a region; a bigger one stays unbacked (logged), maps into it still work */
    case 214: { u64 b=W64(c,3), e=W64(c,5), sz= e>=b? e-b+1 : 0; int rc=0; u64 cpu=0;
      if(!sz || (b&0xFFF) || (sz&0xFFF)) rc=K_INVSIZE;
      else if(g_nsparse<64 && sz<=(2ull<<30) && g_sparse_mb+(sz>>20)<=sparse_budget_mb()){ cpu=mem_find_free(0xC00000000ull,0x1300000000ull,sz,0x10000);
        if(cpu && !mem_map(cpu,sz,MS_NORMAL,PERM_RW)){ nv_gpu_map_raw(b,cpu,sz); g_sparse[g_nsparse++]=(Sparse){b,sz,cpu}; g_sparse_mb+=sz>>20; } else cpu=0; }
      { static int said; if(said<sw2_cap(8)){ said++; LOG("switch 2: sparse GPU region %llx-%llx (%llu MB) %s\n",(unsigned long long)b,(unsigned long long)e,(unsigned long long)(sz>>20),cpu? "backed" : "left unbacked"); } }
      ev_sig(W(c,11)); return rc; }
    case 215: { u64 b=W64(c,3);
      for(u32 i=0;i<g_nsparse;i++) if(g_sparse[i].gva==b){ nv_gpu_unmap_raw(b,g_sparse[i].size); mem_unmap(g_sparse[i].cpu,g_sparse[i].size); g_sparse_mb-=g_sparse[i].size>>20; g_sparse[i]=g_sparse[--g_nsparse]; break; }
      ev_sig(W(c,11)); return 0; }
    case 213: { /* NormalUnmapGpuMemory4K */
      u64 b=W64(c,3), e=W64(c,5), n=0; int rc=K_NOTFOUND;
      for(u32 i=0;i<g_nmaps;i++) if(g_maps[i].as==W(c,2) && g_maps[i].gva==b && e-b+1==g_maps[i].size){ nv_native_unmap(b,g_maps[i].size); n=g_maps[i].size; g_maps[i]=g_maps[--g_nmaps]; rc=0; break; }
      ev_sig(W(c,8)); out64(c,n); return rc; }
  }
  return ns2_unknown(o,c); }

static int gpusess_disp(SvcObj* o, IpcCtx* c){   /* IGpuSession */
  ns2_lock(); int rc=0;
  switch(c->cmd){
    case 11: out32(c,0); out32(c,63); break;                            /* GetNotificationRange */
    case 12: { u32 i=W(c,2); if(i>=64){ rc=K_INVCOMB; out_move_h(c,0); break; }   /* GetNotificationEvent */
      if(!g_dev.ev[i]){ g_dev.ev[i]=kevent_new("gpu notification",0); kobj_ref(&g_dev.ev[i]->o); }
      out_move_h(c,ev_h(g_dev.ev[i])); break; }
    case 21: { u32 i=W(c,2); if(i>=64 || !g_dev.ev[i]){ rc=K_INVSTATE; break; }   /* RequestWorker */
      if(!g_dev.worker) g_dev.worker=svcobj_new("IGpuWorker",soft_worker_disp,0); kevent_signal(g_dev.ev[i]); break; }
    case 22: if(!g_dev.worker){ rc=K_INVSTATE; out_move_h(c,0); break; }   /* AcquireWorker */
      g_dev.worker->refs++; out_move_h(c,session_new(g_dev.worker)); break;
    case 33: { rc=K_OUTOFRES; for(int i=0;i<8;i++) if(!g_dev.as[i]){ int fd=nv_open_dev("/dev/nvhost-as-gpu");   /* CreateAddressSpace */
        if(fd>0){ g_dev.as[i]=fd; out32(c,(u32)fd); rc=0; } break; }
      if(rc) out32(c,0); break; }
    case 41: { u64 name=W64(c,2), flags=W64(c,4); u32 id=0; rc= flags? K_NOTIMPL : K_OUTOFRES;   /* CreateGraphicsDevice */
      if(!flags) for(u32 i=1;i<256;i++) if(!g_dev.dev_used[i]){ g_dev.dev_used[i]=1; g_dev.dev_name[i]=name; id=i; rc=0; break; }
      out32(c,id); break; }
    case 42: { u32 i=W(c,2)&0xFF; rc= g_dev.dev_used[i]? 0 : K_NOTFOUND; g_dev.dev_used[i]=0; break; }   /* DestroyGraphicsDevice */
    case 202: case 203: { u32 id=W(c,2); int ok=0; for(int i=0;i<8;i++) if(g_dev.as[i]==(int)id) ok=1;   /* Get{Gsv,User}Partition */
      rc= ok? 0 : K_NOTFOUND;
      if(c->cmd==202){ out64(c,~0ull); out64(c,0); }
      else { /* the user partition: from the 4K region's start to the end of the 40 bit space (the 4K region of ioctl_as 0x08) */
        out64(c,ok? 1 : 0); out64(c,0x04000000); out64(c,0xFFFFFFFFFFull);   /* the whole 40 bit space: NVN reserves 0x8000000..512 GB of it and takes more past that */ for(int k=0;k<6;k++) out64(c,0); }
      break; }
    default: ns2_unlock(); return ns2_unknown(o,c);
  }
  ns2_unlock(); return rc; }

/* ============================================================ vi2: display sessions, presenting native images */
typedef struct { u64 token; u32 slots; int has_display, bound_default; u64 available; u32 retire_note, avail_note; u64 encoding; u32 queued; } Vi2Client;
typedef struct { Vi2Client cl[16]; u32 client_slots; int display_refs; u32 display_note; u32 notes[32]; KEvent* ev; int endpoint; u64 endpoint_token; } Vi2;
#define NAME_DEFAULT 0x00746C7561666564ull   /* "default" */
static Vi2Client* vi2_find(Vi2* v, u64 t){ if(!t) return NULL; for(int i=0;i<16;i++) if(v->cl[i].token==t) return &v->cl[i]; return NULL; }
static void vi2_note(Vi2* v, u32 n){ v->notes[(n/32)&31]|=1u<<(n%32); if(v->ev) kevent_signal(v->ev); }

static int vi2_present(Vi2* v, Vi2Client* s, IpcCtx* c){
  u32 slot=W(c,4)&0xFF; if(!s->has_display || slot>=15) return K_INVSTATE;
  NsImage* im=image_find(W(c,5)); if(!im || !im->window_pool) return K_NOTFOUND;
  u32 fence=~W(c,22), fval=W(c,24);
  /* word 7: the crop, (bottom << 16) | right, inclusive. Mario Kart World renders 1920x1080 into its 3840x2160 window
     images and shows only that corner (it filled a quarter of the screen without this) */
  { extern volatile i32 g_vi_crop[4]; u32 cr=W(c,7); if(cr){ g_vi_crop[0]=0; g_vi_crop[1]=0; g_vi_crop[2]=(i32)(cr&0xFFFF)+1; g_vi_crop[3]=(i32)(cr>>16)+1; } else memset((void*)g_vi_crop,0,sizeof g_vi_crop); }
  /* the image is complete once the GPU reached the fence. Mario Kart World passes no fence (its NativeSyncHandle is -1,
     which reads as syncpoint 0 here): that wait never ended and cost 200 ms each frame (5 FPS). The present goes to the
     GPU thread behind the submits that draw it anyway */
  { static int nl; if(nl<sw2_cap(3)){ nl++; LOG("switch 2: present words 20-25: %08x %08x %08x %08x %08x %08x\n",W(c,20),W(c,21),W(c,22),W(c,23),W(c,24),W(c,25)); } }
  if(fence!=~0u && fence!=0) syncpt_wait(fence,fval,50000000);
  u32 w=(im->pref[2]&0xFFFF)+1, h=(im->pref[2]>>16)+1, pitch=im->pref[5]*64;
  u64 a=nvmap_addr(im->mem); if(!a) return K_NOTFOUND;
  { static u64 pk=~0ull; static int np; if(pk==~0ull){ const char* e=getenv("SWR_PEEK"); pk= e? strtoull(e,NULL,16) : 0; }   /* debug: guest words at a GPU address, each present */
    { static u32 fw; static int done; if(!done && np==30){ done=1; const char* e=getenv("SWR_FIND"); if(e){ fw=(u32)strtoul(e,NULL,16); int hits=0;   /* debug: where a u32 sits in guest memory */
        for(u64 a=0x10000; a<(1ull<<39) && hits<24;){ MemBlock b; mem_query(a,&b); if(!b.size) break; u64 n=b.addr+b.size; if(n<=a) break;
          if(b.state!=MS_FREE && b.perm){ const u32* w=(const u32*)GP(b.addr); for(u64 i=0;i<b.size/4 && hits<24;i++) if(w[i]==fw){ hits++; LOG("find %08x at %llx\n",fw,(unsigned long long)(b.addr+i*4)); } }
          a=n; } LOG("find %08x: %d hits\n",fw,hits); } } }
    { static int done; if(!done && np==25){ const char* e=getenv("SWR_DUMPMEM"); if(e){ done=1; u64 da=strtoull(e,NULL,16), dn=4u<<20; extern u64 gmmu_translate(u64); u64 c0=gmmu_translate(da);   /* debug: 4 MB of guest memory at a GPU address */
        if(c0){ FILE* f=fopen("captures/memdump.bin","wb"); if(f){ fwrite(GP(c0),1,dn,f); fclose(f); LOG("dumped 4 MB at %llx\n",(unsigned long long)da); } } } } }
    if(pk && np%20==0){ extern u64 gmmu_translate(u64); u64 c0=gmmu_translate(pk); if(c0){ int nz=0, first=-1; for(int i=0;i<0x1000;i++) if(gr32(c0+i*4)){ nz++; if(first<0) first=i*4; } LOG("peek %llx (present %d): %d non-zero words in 16 KB (first at +%x)\n",(unsigned long long)pk,np,nz,first);
      if(np==20){ for(int r=0;r<0x240/16;r++){ float f[4]; memcpy(f,GP(c0+r*16),16); LOG("  +%03x: %g %g %g %g\n",r*16,f[0],f[1],f[2],f[3]); } } } }
    if(pk && np++<0){ extern u64 gmmu_translate(u64); u64 c=gmmu_translate(pk); if(c) LOG("peek %llx: %08x %08x %08x %08x  +0x40: %08x %08x %08x %08x  +0x210: %08x %08x %08x %08x\n",(unsigned long long)pk,gr32(c),gr32(c+4),gr32(c+8),gr32(c+12),gr32(c+0x40),gr32(c+0x44),gr32(c+0x48),gr32(c+0x4c),gr32(c+0x210),gr32(c+0x214),gr32(c+0x218),gr32(c+0x21c)); } }
  { static int said; if(said<sw2_cap(3)){ said++; LOG("switch 2: present image %u (%ux%u, pitch %u, block height 2^%u) slot %u, fence %d/%u\n",im->id,w,h,pitch,im->pref[4],slot,(int)fence,fval); } }
  vi_show_addr(a,w,h,pitch,3,im->pref[4]>=3? im->pref[4]-3 : 0,532120);   /* RGBA8, block linear, block height in GOBs */
  s->available|=1ull<<slot; vi2_note(v,s->avail_note);   /* shown already: the slot is free again */
  (void)fval; return 0; }

/* the chat context's session (GameChat camera images): command 11 hands out its notification event like vi2's;
   no camera, so the event never fires. The rest answers ok */
static int chat_disp(SvcObj* o, IpcCtx* c){
  if(c->cmd==11){ static KEvent* ev; if(!ev){ ev=kevent_new("chat notification",0); kobj_ref(&ev->o); } out_move_h(c,ev_h(ev)); return 0; }
  return ns2_unknown(o,c); }

/* video decoder protocol and output ordering adapted from Eglantine Emulator Project's nx2_gsv.h (GPL-3.0-or-later).
   libvpx is synchronous here: the service owns no shared GSV lock, so a decode only stalls the submitting guest thread. */
#define VDEC_IMAGES 32
#define VDEC_FRAMES 32
#define VDEC_OUTPUTS 64
#define VDEC_QUEUE_BYTES (128u<<20)
typedef struct { u32 id,target,release_id,release_val; int state,has_release; } VDecImage; /* 1 queued, 2 held by client */
typedef struct { u8 *y,*uv; u32 w,h,uv_pitch; u64 pts,serial; int end; } VDecFrame;
typedef struct { u32 image,w,h; u64 pts,serial; int end; } VDecOutput;
typedef struct {
  PMutex lock; KEvent* ready; u32 width,height,rate,syncpt,sync_base,handed; u64 submitted,pictures,dropped,queue_bytes,next_serial;
  int configured,started,ended,ctx_ok; char codec[32]; VDecImage images[VDEC_IMAGES]; u32 nimages,free[VDEC_IMAGES],nfree;
  VDecFrame frames[VDEC_FRAMES]; u32 nframes; VDecOutput outputs[VDEC_OUTPUTS]; u32 noutputs;
#ifdef SWR_HAVE_VP9
  vpx_codec_ctx_t ctx;
#endif
} VDec;
static PMutex g_vpx_lock; static int g_vpx_lock_init;
static u64 vdec_frame_bytes(VDec* v){ u64 w=v->width,h=v->height; return w*h+((w+1)&~1ull)*((h+1)/2); }
static int vdec_room(VDec* v,u32 reserve){ u64 one=vdec_frame_bytes(v); return one && reserve && v->nframes+reserve<=VDEC_FRAMES && one<=VDEC_QUEUE_BYTES/reserve && v->queue_bytes<=VDEC_QUEUE_BYTES-one*reserve; }
static KEvent* vdec_ready_event(VDec* v){ if(!v->ready){ v->ready=kevent_new("video decoder ready",0); kobj_ref(&v->ready->o); } return v->ready; }
static void vdec_ready_update(VDec* v){ if(!v->ready) return; if(!v->ended && vdec_room(v,2)) kevent_signal(v->ready); else kevent_clear(v->ready); }
static VDecImage* vdec_image(VDec* v,u32 id){ for(u32 i=0;i<v->nimages;i++) if(v->images[i].id==id) return &v->images[i]; return NULL; }
static void vdec_complete(VDec* v,u32 target){ if(!v->syncpt) return; u32 cur=syncpt_read(v->syncpt); while((i32)(target-cur)>0){ syncpt_incr(v->syncpt); cur++; } }
static void vdec_complete_all(VDec* v){ if(v->syncpt) vdec_complete(v,v->sync_base+v->handed); }
static void vdec_cancel_free(VDec* v){ for(u32 i=0;i<v->nfree;i++){ VDecImage* vi=vdec_image(v,v->free[i]); if(vi) vi->state=2; } v->nfree=0; }
static void vdec_free_frame(VDec* v,VDecFrame* f){ v->queue_bytes-=f->y? (u64)f->w*f->h+(u64)f->uv_pitch*((f->h+1)/2) : 0; free(f->y); memset(f,0,sizeof *f); }
static void vdec_clear_frames(VDec* v){ while(v->nframes) vdec_free_frame(v,&v->frames[--v->nframes]); }
/* A submission may already be scheduled from an older ready signal. Keep it successful and bounded by discarding the
   oldest picture it overtook. Frames in output images aren't in this queue, and an EOS marker is never discarded. */
static int vdec_make_room(VDec* v,u64 bytes,u64 pts){
  if(bytes>VDEC_QUEUE_BYTES) return 0;
  while(v->nframes>=VDEC_FRAMES || v->queue_bytes>VDEC_QUEUE_BYTES-bytes){ u32 i;
    for(i=0;i<v->nframes;i++) if(!v->frames[i].end && v->frames[i].pts<=pts) break;
    if(i==v->nframes) return 0; u64 old=v->frames[i].pts; vdec_free_frame(v,&v->frames[i]);
    memmove(v->frames+i,v->frames+i+1,(--v->nframes-i)*sizeof *v->frames); memset(&v->frames[v->nframes],0,sizeof *v->frames); v->dropped++;
    if(v->dropped<=8 || !(v->dropped%60)) LOG("switch 2 video: dropped queued frame %llu (pts %llu us) for newer pts %llu us, %u queued (%.1f MB)\n",(unsigned long long)v->dropped,(unsigned long long)old,(unsigned long long)pts,v->nframes,v->queue_bytes/1048576.0); }
  return 1; }
#ifdef SWR_HAVE_VP9
static int vdec_codec_open(VDec* v){
  if(v->ctx_ok) return 1;
  vpx_codec_dec_cfg_t cfg; memset(&cfg,0,sizeof cfg); cfg.threads=1; cfg.w=v->width; cfg.h=v->height;
  pm_lock(&g_vpx_lock); int e=vpx_codec_dec_init(&v->ctx,vpx_codec_vp9_dx(),&cfg,0); pm_unlock(&g_vpx_lock);
  if(e){ WARN("switch 2 video: libvpx decoder init failed: %s\n",vpx_codec_err_to_string((vpx_codec_err_t)e)); return 0; }
  v->ctx_ok=1; return 1; }
static void vdec_codec_close(VDec* v){ if(v->ctx_ok){ pm_lock(&g_vpx_lock); vpx_codec_destroy(&v->ctx); pm_unlock(&g_vpx_lock); v->ctx_ok=0; } }
#else
static int vdec_codec_open(VDec* v){ (void)v; return 0; }
static void vdec_codec_close(VDec* v){ (void)v; }
#endif
static void vdec_reset(VDec* v){ vdec_codec_close(v); vdec_clear_frames(v); v->ended=0; if(v->configured) vdec_codec_open(v); }
#ifdef SWR_HAVE_VP9
/* 1 captured, 0 unsupported output, -1 bounded queue full, -2 allocation failed */
static int vdec_capture(VDec* v,const vpx_image_t* im,u64 fallback_pts,u64* serial){
  if(!im || !im->d_w || !im->d_h || im->d_w>8192 || im->d_h>8192 || im->x_chroma_shift!=1 || im->y_chroma_shift!=1) return 0;
  if(!im->planes[0] || !im->planes[1] || ((im->fmt&~VPX_IMG_FMT_HIGHBITDEPTH)!=VPX_IMG_FMT_NV12 && !im->planes[2])) return 0;
  u32 w=im->d_w,h=im->d_h,uvp=(w+1)&~1u,ch=(h+1)/2; u64 yn=(u64)w*h,uvn=(u64)uvp*ch,total=yn+uvn;
  u64 outpts=im->user_priv? (u64)(uintptr_t)im->user_priv-1 : fallback_pts; if(!vdec_make_room(v,total,outpts)) return -1;
  u8* data=malloc((size_t)total); if(!data) return -2; VDecFrame* f=&v->frames[v->nframes]; memset(f,0,sizeof *f); f->y=data; f->uv=data+yn; f->w=w; f->h=h; f->uv_pitch=uvp;
  f->pts=outpts; f->serial=++v->next_serial; *serial=f->serial;
  int high=(im->fmt&VPX_IMG_FMT_HIGHBITDEPTH)!=0, shift=high && im->bit_depth>8? (int)im->bit_depth-8 : 0;
  for(u32 y=0;y<h;y++){ const u8* s=im->planes[0]+(i64)y*im->stride[0]; u8* d=f->y+(u64)y*w;
    if(high) for(u32 x=0;x<w;x++){ u16 q; memcpy(&q,s+x*2,2); d[x]=(u8)(q>>shift); } else memcpy(d,s,w); }
  if((im->fmt&~VPX_IMG_FMT_HIGHBITDEPTH)==VPX_IMG_FMT_NV12){ for(u32 y=0;y<ch;y++){ u32 n=im->stride[1]<0? (u32)-im->stride[1] : (u32)im->stride[1]; if(n>uvp) n=uvp; memcpy(f->uv+(u64)y*uvp,im->planes[1]+(i64)y*im->stride[1],n); } }
  else { int flip=(im->fmt&VPX_IMG_FMT_UV_FLIP)!=0; const u8* up=im->planes[flip?2:1]; const u8* vp=im->planes[flip?1:2]; int us=im->stride[flip?2:1],vs=im->stride[flip?1:2]; u32 cw=(w+1)/2;
    for(u32 y=0;y<ch;y++){ u8* d=f->uv+(u64)y*uvp; const u8* u=up+(i64)y*us; const u8* q=vp+(i64)y*vs;
      for(u32 x=0;x<cw;x++){ if(high){ u16 a,b; memcpy(&a,u+x*2,2); memcpy(&b,q+x*2,2); d[x*2]=(u8)(a>>shift); d[x*2+1]=(u8)(b>>shift); } else { d[x*2]=u[x]; d[x*2+1]=q[x]; } } } }
  v->queue_bytes+=total; v->nframes++; return 1; }
#endif
static int vdec_write(VDec* v,VDecImage* vi,VDecFrame* f,u32* ow,u32* oh){
  NsImage* im=image_find(vi->id); if(!im || !im->decoder) return 0; u32 lw=(im->pref[2]&0xFFFF)+1,lh=(im->pref[2]>>16)+1; u64 pitch=(lw+63)&~63ull,chroma=pitch*((lh+127)&~127ull),size=chroma+pitch*(((lh+1)/2+127)&~127ull);
  u64 base=nvmap_addr(im->mem),msz=nvmap_size(im->mem); if(!base || im->off>msz || size>msz-im->off || size>im->size || !guest_span_writable(base+im->off,size,1)) return 0;
  u8* d=GP(base+im->off); memset(d,0,(size_t)size); *ow=f->y? (f->w<lw?f->w:lw) : (v->width<lw?v->width:lw); *oh=f->y? (f->h<lh?f->h:lh) : (v->height<lh?v->height:lh);
  if(!f->y) return 1;
  for(u32 y=0;y<*oh;y++) memcpy(d+(u64)y*pitch,f->y+(u64)y*f->w,*ow);
  u32 cw=(*ow+1)&~1u,ch=(*oh+1)/2; for(u32 y=0;y<ch;y++) memcpy(d+chroma+(u64)y*pitch,f->uv+(u64)y*f->uv_pitch,cw); return 1; }
static void vdec_pump(VDec* v){
  while(v->nframes && v->nfree && v->noutputs<VDEC_OUTPUTS){ u32 id=v->free[0]; memmove(v->free,v->free+1,(--v->nfree)*sizeof *v->free); VDecImage* vi=vdec_image(v,id); if(!vi || vi->state!=1) continue;
    if(vi->has_release && vi->release_id<192) syncpt_wait(vi->release_id,vi->release_val,100000000);
    vi->has_release=0;
    VDecFrame f=v->frames[0]; memmove(v->frames,v->frames+1,(--v->nframes)*sizeof *v->frames); u32 w=0,h=0; int wrote=vdec_write(v,vi,&f,&w,&h);
    if(!wrote){ vi->state=2; WARN("switch 2 video: image %u lost its writable decoder memory\n",id); vdec_free_frame(v,&f); continue; }
    vi->state=2; v->outputs[v->noutputs++]=(VDecOutput){id,w,h,f.pts,f.serial,f.end}; vdec_complete(v,vi->target); v->pictures++;
    if(v->pictures==1 || !(v->pictures%120) || f.end) LOG("switch 2 video: frame %llu (%ux%u, pts %llu us%s) in image %u, %u queued, %llu dropped\n",(unsigned long long)v->pictures,w,h,(unsigned long long)f.pts,f.end? ", end" : "",id,v->nframes,(unsigned long long)v->dropped);
    vdec_free_frame(v,&f); }
  vdec_ready_update(v); }
static void vdec_handout(VDec* v,IpcCtx* c,u32 id,u32 release_word,u32 release_val,int release){
  VDecImage* vi=vdec_image(v,id); if(!vi && v->nimages<VDEC_IMAGES){ vi=&v->images[v->nimages++]; memset(vi,0,sizeof *vi); vi->id=id; }
  if(!vi || vi->state==1){ out32(c,~0u); out32(c,0); out32(c,0); out32(c,0); return; }
  vi->state=1; vi->target=v->sync_base+(++v->handed); vi->has_release=release && release_word!=~0u; vi->release_id=~release_word; vi->release_val=release_val; v->free[v->nfree++]=id;
  out32(c,~v->syncpt); out32(c,0); out32(c,vi->target); out32(c,0); vdec_pump(v); }
static void vdec_destroy(SvcObj* o){ VDec* v=o->data; pm_lock(&v->lock); vdec_complete_all(v); vdec_codec_close(v); vdec_clear_frames(v); pm_unlock(&v->lock); if(v->ready) kobj_unref(&v->ready->o); }
static int vdec_svc_disp(SvcObj* o, IpcCtx* c){ VDec* v=o->data; int rc=0; pm_lock(&v->lock);
  switch(c->cmd){
    case 3: out_copy_h(c,ev_h(vdec_ready_event(v))); vdec_ready_update(v); break;
    case 6: v->started=1; if(v->configured && !v->ctx_ok) vdec_codec_open(v); vdec_ready_event(v); vdec_ready_update(v); break;
    case 8: case 207: vdec_complete_all(v); vdec_cancel_free(v); vdec_reset(v); v->noutputs=0; vdec_ready_event(v); vdec_ready_update(v); break;
    case 115: vdec_complete_all(v); vdec_cancel_free(v); vdec_codec_close(v); vdec_clear_frames(v); v->started=0; v->ended=1; break;
    case 200: case 201: case 202: case 210: break;
    case 101: { u64 a=0,n=0; char name[32]={0}; if(buf_in(c,0,&a,&n) && n && guest_span_readable(a,n,1)){ u64 z=n<sizeof name-1?n:sizeof name-1; memcpy(name,GP(a),(size_t)z); }
      v->width=in32(c,0); v->height=in32(c,4); v->rate=in32(c,8); if(!v->width || v->width>8192 || !v->height || v->height>8192) rc=K_INVSIZE;
      else if(!strstr(name,"vp9")) rc=K_NOTIMPL; else { strncpy(v->codec,name,sizeof v->codec-1); v->configured=1; vdec_reset(v); if(!v->ctx_ok) rc=K_NOTIMPL; }
      LOG("switch 2 video: configure codec \"%s\" %ux%u rate %u (%x)\n",name,v->width,v->height,v->rate,rc); break; }
    case 311: { u32 id=in32(c,0); NsImage* im=image_find(id); u64 base=im?nvmap_addr(im->mem):0,msz=im?nvmap_size(im->mem):0;
      if(!im || !im->decoder || !base || im->off>msz || im->size>msz-im->off || !guest_span_writable(base+im->off,im->size,1) || vdec_image(v,id) || v->nimages>=VDEC_IMAGES){ rc=K_NOTFOUND; for(int i=0;i<4;i++) out32(c,0); }
      else { vdec_handout(v,c,id,0,0,0); LOG("switch 2 video: registered output image %u on syncpoint %u value %u\n",id,v->syncpt,v->handed); } break; }
    case 314: { u32 id=in32(c,0); VDecImage* vi=vdec_image(v,id); if(!vi || vi->state!=2){ rc=K_NOTFOUND; for(int i=0;i<4;i++) out32(c,0); } else vdec_handout(v,c,id,in32(c,8),in32(c,16),1); break; }
    case 212: { u32 id=in32(c,0); VDecImage* vi=vdec_image(v,id); if(!vi){ rc=K_NOTFOUND; break; } if(vi->state==1) vdec_complete(v,vi->target);
      for(u32 i=0;i<v->nfree;i++) if(v->free[i]==id){ memmove(v->free+i,v->free+i+1,(--v->nfree-i)*sizeof *v->free); break; }
      for(u32 i=0;i<v->noutputs;) if(v->outputs[i].image==id) memmove(v->outputs+i,v->outputs+i+1,(--v->noutputs-i)*sizeof *v->outputs); else i++;
      *vi=v->images[--v->nimages]; break; }
    case 213: if(!v->noutputs) rc=0x1c286f; else { VDecOutput q=v->outputs[0]; memmove(v->outputs,v->outputs+1,(--v->noutputs)*sizeof *v->outputs); u64 ns=q.pts*1000;
      out32(c,q.image); out32(c,q.w); out32(c,q.h); out32(c,q.w); out32(c,q.h); out32(c,0); out32(c,(u32)ns); out32(c,(u32)(ns>>32)); out32(c,q.end?1:0); for(int i=0;i<7;i++) out32(c,0); } break;
    case 109: case 400: { u64 a=0,n=0,pts=in64(c,0),last=0; int end=in8(c,8)!=0; if(!v->configured || !v->ctx_ok) rc=K_INVSTATE;
      else if(!buf_in(c,0,&a,&n) || n>64u<<20 || (n && !guest_span_readable(a,n,1))) rc=K_INVSIZE;
#ifdef SWR_HAVE_VP9
      else { pm_lock(&g_vpx_lock); int e=vpx_codec_decode(&v->ctx,n?GP(a):NULL,(unsigned)n,(void*)(uintptr_t)(pts+1),0); vpx_codec_iter_t it=NULL; const vpx_image_t* im;
        int cap=1; while(!e && (im=vpx_codec_get_frame(&v->ctx,&it))) if((cap=vdec_capture(v,im,pts,&last))!=1) break;
        pm_unlock(&g_vpx_lock); vdec_pump(v);
        if(!e && cap==1 && end){ pm_lock(&g_vpx_lock); e=vpx_codec_decode(&v->ctx,NULL,0,(void*)(uintptr_t)(pts+1),0); it=NULL; while(!e && (im=vpx_codec_get_frame(&v->ctx,&it))) if((cap=vdec_capture(v,im,pts,&last))!=1) break; pm_unlock(&g_vpx_lock); vdec_pump(v); }
        if(e){ WARN("switch 2 video: VP9 decode failed: %s\n",vpx_codec_err_to_string((vpx_codec_err_t)e)); rc=K_INVSIZE; }
        else if(cap!=1){ WARN("switch 2 video: decoded output could not be queued (%s, %u frames, %.1f MB)\n",cap<0? (cap==-1? "capacity" : "allocation") : "format",v->nframes,v->queue_bytes/1048576.0); rc= cap==-2? K_OUTOFRES : K_INVSIZE; }
        else if(end){ if(last){ for(u32 i=0;i<v->nframes;i++) if(v->frames[i].serial==last) v->frames[i].end=1; for(u32 i=0;i<v->noutputs;i++) if(v->outputs[i].serial==last) v->outputs[i].end=1; }
          else if(vdec_make_room(v,0,pts)){ VDecFrame* f=&v->frames[v->nframes++]; memset(f,0,sizeof *f); f->pts=pts; f->serial=++v->next_serial; f->end=1; }
          v->ended=1; vdec_pump(v); }
      }
#else
      else { (void)last; rc=K_NOTIMPL; }
#endif
      if(!rc){ u64 id=++v->submitted; out64(c,id); if(id<=24 || !(id%120)) LOG("switch 2 video: submit %llu, %llu bytes, pts %llu us%s, %llu output, %u queued (%.1f MB), %llu dropped\n",(unsigned long long)id,(unsigned long long)n,(unsigned long long)pts,end? ", end" : "",(unsigned long long)v->pictures,v->nframes,v->queue_bytes/1048576.0,(unsigned long long)v->dropped); }
      vdec_ready_update(v);
      break; }
    default: pm_unlock(&v->lock); return ns2_unknown(o,c);
  }
  pm_unlock(&v->lock); return rc; }
static int vdec_disp(SvcObj* o, IpcCtx* c){
  if(c->cmd==1){ SvcObj* s=svcobj_new("IVideoDecoderService",vdec_svc_disp,sizeof(VDec)); VDec* v=s->data; pm_init(&v->lock); ns2_lock(); if(!g_vpx_lock_init){ pm_init(&g_vpx_lock); g_vpx_lock_init=1; } ns2_unlock(); v->syncpt=syncpt_alloc(); v->sync_base=syncpt_read(v->syncpt); s->destroy=vdec_destroy; return move_obj(c,s,0); }
  return ns2_unknown(o,c); }

static int vi2_disp(SvcObj* o, IpcCtx* c){   /* IVi2Session */
  Vi2* v=(Vi2*)o->data; ns2_lock(); int rc=0; u64 tok=W64(c,2); Vi2Client* s=vi2_find(v,tok);
  switch(c->cmd){
    case 1: case 3: case 5: out32(c,c->cmd==5? 7 : 0); break;           /* notification / session / request group limits */
    case 11: if(!v->ev){ v->ev=kevent_new("vi2 notification",0); kobj_ref(&v->ev->o); }   /* GetNotificationEvent */
      { int any=0; for(int i=0;i<32;i++) any|=v->notes[i]!=0; if(any) kevent_signal(v->ev); }
      out_move_h(c,ev_h(v->ev)); break;
    case 13: break;                                                     /* ReleaseNotificationEvent */
    case 31: case 32: { int i=c->cmd-31; if(!g_vsync[i]){ g_vsync[i]=kevent_new(i? "vi2 vsync 32" : "vi2 vsync 31",0); kobj_ref(&g_vsync[i]->o); }
      out_move_h(c,ev_h(g_vsync[i])); break; }                            /* vsync events, signalled at 60 Hz */
    case 21: { u64 a,sz; buf_out(c,0,&a,&sz); if(sz<sizeof v->notes){ rc=K_INVSIZE; break; }   /* GetNotifications */
      memcpy(GP(a),v->notes,sizeof v->notes); memset(v->notes,0,sizeof v->notes); if(v->ev) kevent_clear(v->ev); break; }
    case 51: rc= W(c,2)<8? 0 : K_INVCOMB; break;                        /* ConfigureRequestGroups */
    case 101: { u64 name=W64(c,2); u32 note=W(c,4);                       /* OpenDisplay */
      if(name!=NAME_DEFAULT){ rc=0; break; }   /* GameShare's player displays: accepted, nothing shown there */
      if(note!=1){ rc=K_NOTIMPL; break; } v->display_refs++; v->display_note=note; break; }
    case 102: if(W64(c,2)==NAME_DEFAULT && v->display_refs){ if(!--v->display_refs) vi2_note(v,v->display_note); } break;   /* CloseDisplay */
    case 111: if(!s && tok==NAME_DEFAULT){ v->endpoint=1; v->endpoint_token=tok; break; }   /* Mario Kart World opens it by display name ("default", "default") */
      if(!s || !s->has_display || v->endpoint){ rc= s? K_INVSTATE : K_NOTFOUND; break; } v->endpoint=1; v->endpoint_token=tok; break;   /* OpenEndpoint */
    case 112: if(!v->endpoint){ rc=K_NOTFOUND; break; } v->endpoint=0; break;   /* CloseEndpoint */
    case 104: case 105: break;   /* display "default", ~75 times a second in Mario Kart World: success, no outputs */
    case 201: { u32 a=W(c,4), b=W(c,5);                                 /* RegisterClientState {token, retire, available} */
      if(!tok || a>=32 || b>=32 || a==b){ rc=K_INVCOMB; break; } u32 sl=(1u<<a)|(1u<<b);
      if(s || (v->client_slots&sl)){ rc=K_INVSTATE; break; }
      Vi2Client* f=vi2_find(v,0); for(int i=0;i<16 && !f;i++) if(!v->cl[i].token) f=&v->cl[i];
      if(!f){ rc=K_OUTOFRES; break; }
      memset(f,0,sizeof *f); f->token=tok; f->slots=sl; f->retire_note=a; f->avail_note=b; f->available=0x7FFF; f->encoding=0x14; v->client_slots|=sl; break; }
    case 202: if(!s){ rc=K_NOTFOUND; break; } vi2_note(v,s->retire_note); v->client_slots&=~s->slots; memset(s,0,sizeof *s); break;   /* UnregisterClientState */
    case 212: if(!s){ rc=K_NOTFOUND; out64(c,0); break; } out64(c,s->available); break;   /* GetClientAvailability */
    case 228: { u32 slot=W(c,4)&0xFF; if(!s || slot>=15){ rc=K_NOTFOUND; } else s->available|=1ull<<slot;   /* CancelClientBuffer */
      out64(c,s? s->available : 0); for(int i=0;i<4;i++) out32(c,0); break; }
    case 229: { if(!s) rc=K_NOTFOUND; else { s->available&=~(1ull<<(W(c,4)&0xFF)); ns2_unlock(); rc=vi2_present(v,s,c); ns2_lock(); }   /* PresentClientBuffer */
      out64(c,s? s->available : 0); for(int i=0;i<4;i++) out32(c,0); break; }
    case 251: if(!s){ rc=K_NOTFOUND; break; } if(W64(c,4)==NAME_DEFAULT && !v->display_refs){ rc=K_INVSTATE; break; } s->has_display=1; break;   /* BindClientDisplay */
    case 252: if(!s || !s->has_display){ rc=K_NOTFOUND; break; } s->has_display=0; break;   /* UnbindClientDisplay */
    case 351: if(!s){ rc=K_NOTFOUND; break; } rc= W64(c,4)==0x14? 0 : K_NOTIMPL; break;   /* SetClientOutputEncoding */
    default: ns2_unlock(); return ns2_unknown(o,c);
  }
  ns2_unlock(); return rc; }

/* ============================================================ IGraphicsSupervisor */
static int gsv_disp(SvcObj* o, IpcCtx* c){
  int cmd=(int)c->cmd, rc=0;
  ns2_lock();
  switch(cmd){
    case 4: { u64 flags=W64(c,6); rc= g_phase? K_INVSTATE : 0; if(!rc) g_phase=1;   /* Initialize {process, aruid, flags, remote memory} */
      LOG("switch 2: graphics supervisor bootstrap (flags %llx)\n",(unsigned long long)flags); break; }
    case 11: rc= g_phase==1? 0 : K_INVSTATE; if(!rc) g_phase=2; break;  /* BindProcess */
    case 101: rc= g_phase==2? 0 : K_INVSTATE; if(!rc){ g_phase=3; LOG("switch 2: graphics supervisor ready\n"); } break;   /* CompleteBootstrap */
    case 111: { u64 sz=W64(c,3); u32 id=0;                             /* CreateNativeMemory {kind, size} */
      if(g_phase!=3) rc=K_INVSTATE; else if(!sz || sz>(1ull<<32) || (sz&0xFFF)) rc=K_INVSIZE; else if(g_nnative>=1024) rc=K_OUTOFRES;
      else { id=nv_native_handle(0,sz); if(!id) rc=K_OUTOFRES; else g_native_mem[g_nnative++]=id; }
      out32(c,id); break; }
    case 112: { u32 id=W(c,2); rc=K_NOTFOUND; for(u32 i=0;i<g_nnative;i++) if(g_native_mem[i]==id){ nv_native_free(id); g_native_mem[i]=g_native_mem[--g_nnative]; rc=0; break; } break; }   /* DestroyNativeMemory */
    case 114: { u32 id=W(c,2); u64 a=W64(c,3), sz=W64(c,5), fl=W64(c,7), off=W64(c,9);   /* BindNativeMemory */
      if(!native_mem_has(id)) rc=K_NOTFOUND; else if((fl!=0 && fl!=2) || off) rc=K_NOTIMPL; else if(!a || sz!=nvmap_size(id)) rc=K_INVSIZE;
      else if(a&0xFFF) rc=K_INVADDR; else if(fl==2 && !gpu_accessible(a,a+sz-1)) rc=K_INVMEM; else nv_native_bind(id,a);
      break; }
    case 115: rc= g_phase==3? 0 : K_INVSTATE; out64(c,0); break;       /* ConfigureNativeMemory: the nvdrv-compatible mode */
    case 201: rc= g_phase==3? 0 : K_INVSTATE; if(!rc) g_sync_ctx=1; break;   /* InitializeSyncpointContext */
    case 202: if(!g_sync_ctx){ rc=K_INVSTATE; break; } rc=move_obj(c,svcobj_new("ISyncpointSupervisor",soft_sync_disp,0),0); break;   /* GetSyncpointSession */
    case 204: { /* BindSyncpointMemory: two transfer memories, 0x1000 bytes written by the game, 0x2000 read by it */
      KTmem* rw= c->ncopy>0? (KTmem*)h_get_type(c->copy_h[0],KO_TMEM) : NULL; KTmem* ro= c->ncopy>1? (KTmem*)h_get_type(c->copy_h[1],KO_TMEM) : NULL;
      if(!g_sync_ctx || g_sc_rw) rc=K_INVSTATE; else if(!rw || !ro || rw==ro) rc=0xE401; else if(rw->size!=0x1000 || ro->size!=0x2000) rc=K_INVSIZE;
      if(rc){ out_move_h(c,0); out_move_h(c,0); break; }
      memset(GP(rw->addr),0,0x1000); memset(GP(ro->addr),0,0x2000);
      static const u32 hdr[8]={1,0,SC_N-1,0x40,0,0x640,8,0x648}; memcpy(GP(ro->addr),hdr,sizeof hdr);
      for(u32 i=0;i<SC_N;i++){ gw64(ro->addr+0x40+i*8,~0ull); g_sc_prev[i]=~0ull; }
      g_sc_rw=rw->addr; g_sc_ro=ro->addr; g_sc_req=kevent_new("syncpoint cache request",0); g_sc_done=kevent_new("syncpoint cache",0);
      kobj_ref(&g_sc_req->o); kobj_ref(&g_sc_done->o); plat_thread(sc_thread,NULL,256*1024);
      out_move_h(c,ev_h(g_sc_req)); out_move_h(c,ev_h(g_sc_done));
      LOG("switch 2: syncpoint cache at %llx / %llx\n",(unsigned long long)g_sc_rw,(unsigned long long)g_sc_ro); break; }
    case 301: rc= g_phase==3? 0 : K_INVSTATE; if(!rc) g_disp_ctx=1; break;   /* InitializeDisplayContext */
    case 305: if(!g_disp_ctx){ rc=K_INVSTATE; out_move_h(c,0); break; }       /* OpenDisplaySession */
      rc=move_obj(c,svcobj_new("IVi2Session",soft_vi2_disp,sizeof(Vi2)),0); break;
    case 314: case 315: { NsImage* im=image_find(W(c,2)); int create=cmd==315;   /* {Destroy,Create}NativeWindowAliasImagePool */
      if(!im) rc=K_NOTFOUND; else if(create && W(c,3)!=1) rc=K_NOTIMPL; else if(im->window_pool==create) rc=K_INVSTATE; else im->window_pool=create; break; }
    case 401: rc= g_phase==3? 0 : K_INVSTATE; if(!rc) g_gpu_ctx=1; break;   /* InitializeGpuContext */
    case 402: rc= g_gpu_ctx? 0 : K_INVSTATE; if(!rc) g_gpu_ready=1; break;  /* ActivateGpuContext */
    case 423: { u32 id=W(c,2), parent=W(c,7); u64 a=W64(c,3), sz=W64(c,5), off=W64(c,8);   /* BindNativeGpuMemory: a range of other native memory */
      u64 pa=nvmap_addr(parent);
      if(!pa || !native_mem_has(id) || off>nvmap_size(parent) || sz>nvmap_size(parent)-off || a!=pa+off) rc=K_NOTFOUND;
      else if(sz!=nvmap_size(id)) rc=K_INVSIZE; else nv_native_bind(id,a);
      break; }
    case 429: { u32 id=0; u64 sz=0;                                    /* CreateNativeImage {preference[16], count, memory} */
      u32 p[16]; for(int i=0;i<16;i++) p[i]=W(c,2+i);
      if(g_phase!=3 || !g_gpu_ready || !g_img_formats) rc=K_INVSTATE;
      else if(!image_size_any(p,W(c,18),&sz,NULL)) rc=K_NOTIMPL;
      else if(!native_mem_has(W(c,19)) || !nvmap_addr(W(c,19))) rc=K_NOTFOUND;
      else if(nvmap_size(W(c,19))<sz) rc=K_INVSIZE; else if(g_nimg>=1024) rc=K_OUTOFRES;
      else { NsImage* im=&g_img[g_nimg++]; memset(im,0,sizeof *im); im->id=id=g_next_img++; im->mem=W(c,19); memcpy(im->pref,p,sizeof p); im->size=sz; }
      { static int said; if(said<sw2_cap(4)){ said++; LOG("switch 2: native image %u: %ux%u, pitch %u GOBs, memory %u (%x)\n",id,(p[2]&0xFFFF)+1,(p[2]>>16)+1,p[5],W(c,19),rc); } }
      out32(c,id); break; }
    case 430: { NsImage* im=image_find(W(c,2)); if(!im) rc=K_NOTFOUND; else if(im->window_pool) rc=K_BUSY; else *im=g_img[--g_nimg]; break; }   /* DestroyNativeImage */
    case 434: { u32 id=W(c,2); rc= g_gpu_ready? K_NOTFOUND : K_INVSTATE;   /* RegisterGpuAddressSpace */
      if(g_gpu_ready) for(int i=0;i<8;i++) if(g_dev.as[i]==(int)id){ g_dev.as_reg[i]=1; rc=0; } break; }
    case 435: case 1510: { u32 p[16]; for(int i=0;i<16;i++) p[i]=W(c,2+i); u64 sz=0, al=0;   /* CalculateNativeImageSize / ImageRequirements */
      if(g_phase!=3 || !g_img_formats) rc=K_INVSTATE; else if(!image_size_any(p,W(c,18),&sz,&al)) rc=K_NOTIMPL;
      { static int said; if(said<sw2_cap(2)){ said++; LOG("switch 2: %s {",cmd==1510? "image requirements" : "native image size"); for(int i=0;i<16;i++) LOG("%s%x",i? " " : "",p[i]); LOG("} count %u -> size %llx align %llx (%x)\n",W(c,18),(unsigned long long)sz,(unsigned long long)al,rc); } }
      out64(c,sz); if(cmd==1510) out64(c,al); break; }
    case 497: if(g_phase!=3){ rc=K_INVSTATE; out_move_h(c,0); break; }       /* OpenGpuSession */
      rc=move_obj(c,svcobj_new("IGpuSession",soft_gpusess_disp,0),0); break;
    /* 1201-1203: the chat (GameChat camera) context, nn::chat::gsv::Initialize. 1202 hands out a session the sdk
       calls next (it sent requests to handle 0 without one); everything on it answers ok for now */
    /* 601/602: another context with its session (the game sends requests on 602's right away); the session answers
       ok to everything for now, its commands show up in the log */
    case 901: case 1001: break;                                        /* the video decoder's contexts */
    case 902: case 906: case 1002: rc=move_obj(c,svcobj_new("IVideoDecoderSession",vdec_disp,0),0); break;
    /* 991: nn::codec::CalculateDecoderImagePoolBuffer. NV12 planes have 64-byte rows and 128-row blocks. */
    case 991: { u32 dims=W(c,4), w=(dims&0xFFFF)+1, h=(dims>>16)+1, count=W(c,18);   /* pref[2] packs (w-1)|(h-1)<<16 */
      if(w<16 || w>8192 || h<16 || h>8192 || !count || count>VDEC_IMAGES){ rc=K_INVSIZE; out64(c,0); out64(c,0x8000); break; }
      u64 pitch=(w+63)&~63ull, one=pitch*((h+127)&~127ull)+pitch*(((h+1)/2+127)&~127ull),sz=one*count;
      { static int said; if(said<sw2_cap(2)){ said++; LOG("switch 2: decoder image size {%x %x %x %x}, count %u -> %ux%u, %llx bytes\n",W(c,2),W(c,3),W(c,4),W(c,5),count,w,h,(unsigned long long)sz); } }
      out64(c,sz); out64(c,0x8000); break; }
    /* 911: CreateDecoderImagePool {preference[16], count, memory}: count images sharing the pool memory, image i at
       offset layout.size*i, handle = first + i. The decoder writes NV12 into these (gsv 961 describes the layout). */
    case 911: { u32 p[16]; for(int i=0;i<16;i++) p[i]=W(c,2+i); u32 mem=W(c,19), count=W(c,18);
      u32 dims=p[2], w=(dims&0xFFFF)+1, h=(dims>>16)+1; if(w<16 || w>8192 || h<16 || h>8192){ w=640; h=360; }
      u64 pitch=(w+63)&~63ull, isz=pitch*((h+127)&~127ull)+pitch*(((h+1)/2+127)&~127ull);
      if(!mem || !native_mem_has(mem) || !nvmap_addr(mem)){ rc=K_NOTFOUND; out32(c,0); break; }
      if(!count || count>VDEC_IMAGES || isz>nvmap_size(mem) || count>nvmap_size(mem)/isz){ rc=K_INVSIZE; out32(c,0); break; }
      if(g_nimg+count>1024) rc=K_OUTOFRES;
      else { u32 first=g_next_img; for(u32 i=0;i<count;i++){ NsImage* im=&g_img[g_nimg++]; memset(im,0,sizeof *im); im->id=g_next_img++; im->mem=mem; memcpy(im->pref,p,sizeof p); im->size=isz; im->off=(u64)i*isz; im->decoder=1; }
        out32(c,first); }
      { static int said; if(said<sw2_cap(2)){ said++; LOG("switch 2: decoder image pool in memory %u: %u images of %ux%u (%llx bytes) (%x)\n",mem,count,w,h,(unsigned long long)isz,rc); } }
      break; }
    /* 961: GetCpuImageDescriptor {image}: the 128-byte descriptor behind nn::codec::ReadDecoderOutputImage.
       kind byte at +0 (8 RGBA / 9 BGRA / 10 NV12), plane offset u32 at +8, row pitch u64 at +0x10,
       NV12 chroma offset at +0x18 and chroma pitch at +0x20. */
    case 961: { NsImage* im=image_find(W(c,2)); if(!im){ rc=K_NOTFOUND; break; }
      u32 dims=im->pref[2], w=(dims&0xFFFF)+1, h=(dims>>16)+1; u64 pitch=(w+63)&~63ull;
      u64 chroma_off=pitch*((h+127)&~127ull);
      out32(c,10); out32(c,0); out64(c,0); out64(c,pitch); out64(c,chroma_off); out64(c,pitch);
      break; }
    case 601: break;
    case 602: rc=move_obj(c,svcobj_new("IGsvSession602",ns2_unknown,0),0); break;
    case 1201: break;
    /* 1211: the chat camera's image pool {preference[16], count, memory}: one native image in that memory, its id is
       the pool's (nn::chat::gsv::GetImagePoolImageHandle hands it to QueryNativeImage, 428) */
    /* the pool's images get consecutive ids (nn::gs::sdk::CalculateNativeImageHandle is the first one + index), each
       at its own offset in the memory: as many as fit (the memory came from 1291's size) */
    case 1211: { u32 p[16]; for(int i=0;i<16;i++) p[i]=W(c,2+i); u32 mem=0; u64 sz=0;
      for(int i=18;i<22;i++) if(native_mem_has(W(c,i))){ mem=W(c,i); break; }
      image_size_any(p,1,&sz,NULL); u64 msz= mem? nvmap_size(mem) : 0; if(!sz || sz>msz) sz=msz;
      u32 n= sz? (u32)(msz/sz) : 0; if(n>16) n=16;
      if(!mem || !n) rc=K_NOTFOUND; else if(g_nimg+n>1024) rc=K_OUTOFRES;
      else { u32 first=g_next_img; for(u32 i=0;i<n;i++){ NsImage* im=&g_img[g_nimg++]; memset(im,0,sizeof *im); im->id=g_next_img++; im->mem=mem; memcpy(im->pref,p,sizeof p); im->size=sz; im->off=(u64)i*sz; }
        out32(c,first); }
      { static int said; if(said<sw2_cap(2)){ said++; LOG("switch 2: chat image pool in memory %u: %u images of %llx bytes (%x)\n",mem,n,(unsigned long long)sz,rc); } }
      if(rc) out32(c,0); break; }
    /* 428: QueryNativeImage. The sdk reads the address at data+8, then a 128 byte descriptor: two kind bytes that have
       to be 1 (data+16, data+48), the image's offset and size (data+56) */
    /* 426: MapNativeImage {image, address space, permission, ...}: the same answer as 428 with the GPU address. GPU
       addresses are the CPU ones here, the image's memory gets mapped at its own address. 427 unmaps (ok) */
    case 426: case 428: { NsImage* im=image_find(W(c,2)); if(!im){ rc=K_NOTFOUND; break; }
      u64 a=nvmap_addr(im->mem); if(a) a+=im->off;
      if(cmd==426 && a){ for(u32 i=0;i<g_nmaps;i++) if(g_maps[i].gva==a){ a=0; break; }
        if(a && !nv_native_map(a,im->mem,im->off,(im->size+0xFFF)&~0xFFFull) && g_nmaps<4096) g_maps[g_nmaps++]=(GpuMap){W(c,3),a,(im->size+0xFFF)&~0xFFFull,im->mem};
        a=nvmap_addr(im->mem)+im->off; }
      /* descriptor (nvnTextureInitializeFromNativeImage reads it): byte 0 = 1, +8 the size word ((w-1) | (h-1) << 16),
         +32 = 1 with the image kind above it (the preference's p[1]: 1 for color formats, 2 for the YUV ones),
         +40 offset and size */
      u32 kind= im->pref[1]==1 || im->pref[1]==2? im->pref[1] : 1;
      out64(c,a); out64(c,a); out64(c,1); out64(c,im->pref[2]);   /* data+0: the GPU address (MapNativeImage keys its mappings by it) */ out64(c,0); out64(c,0); out64(c,1|((u64)kind<<32));
      out64(c,(u64)(u32)im->size<<32); for(int i=0;i<10;i++) out64(c,0); out32(c,0);
      { static int said; if(said<sw2_cap(4)){ said++; LOG("switch 2: native image %u %s (%llx, %llx bytes)\n",im->id,cmd==426? "mapped" : "queried",(unsigned long long)a,(unsigned long long)im->size); } }
      break; }
    case 427: break;
    /* 1291: the chat camera's video memory for an image preference ({0, 1, size, ...} like 1510's): a size that's
       surely big enough, so CreateNativeMemory (111) gets a real size instead of 0 */
    case 1291: { u32 p[16]; for(int i=0;i<16;i++) p[i]=W(c,2+i); u64 sz=0, al=0x10000;
      image_size_any(p,1,&sz,&al); if(!sz) sz=0x400000; sz=(sz*4+0xFFFF)&~0xFFFFull;
      { static int said; if(said<sw2_cap(2)){ said++; LOG("switch 2: chat video memory %ux%u -> %llx bytes\n",(p[2]&0xFFFF)+1,(p[2]>>16)+1,(unsigned long long)sz); } }
      out64(c,sz); out64(c,al); break; }
    case 1202: rc=move_obj(c,svcobj_new("IChatSession",chat_disp,0),0); break;
    case 1101: rc= g_phase==3? 0 : K_INVSTATE; if(!rc) g_frame_ctx=1; break;   /* InitializeFrameContext */
    case 1102: rc= g_frame_ctx? 0 : K_INVSTATE; break;                         /* ActivateFrameContext */
    /* GetNvidiaDriver: an nvdrv session. games that bootstrap with flag bit 0 (nvdrv mode) ask for it right after
       CompleteBootstrap, without setting up the image context first */
    case 1402: if(g_phase!=3){ rc=K_INVSTATE; break; }
      out_move_h(c,session_new(nvdrv_obj_new("nvdrv:gsv"))); LOG("switch 2: nvdrv session for the game's NVN\n"); break;
    case 1501: rc= g_phase==3? 0 : K_INVSTATE; if(!rc) g_img_ctx=1; break;   /* InitializeImageContext */
    case 1502: rc= g_img_ctx? 0 : K_INVSTATE; if(!rc) g_img_formats=1; break;
    default: ns2_unlock(); return ns2_unknown(o,c);
  }
  ns2_unlock();
  if(rc && ipc_warn_once("gsv-fail",(u32)cmd)) WARN("switch 2: graphics supervisor cmd %d -> %x\n",cmd,rc);
  return rc; }

/* ISelfController 2000: the session is made once and handed out as a copy handle */
#define NS2_SOFT(f) static int soft_##f(SvcObj* o, IpcCtx* c){ return ns2_soft(o,c,f(o,c)); }
NS2_SOFT(worker_disp) NS2_SOFT(sync_disp) NS2_SOFT(vi2_disp) NS2_SOFT(gpusess_disp) NS2_SOFT(gsv_disp)

void ns2_graphics_supervisor(IpcCtx* c){
  static u32 h; static SvcObj* o;
  if(!o){ o=svcobj_new("IGraphicsSupervisor",soft_gsv_disp,0); h=session_new(o); LOG("switch 2: graphics supervisor opened\n"); }
  out_copy_h(c,h_new(h_get(h))); }   /* a handle of its own each time, to the same session */

/* renderer=vulkan (the NVN layer): the memory of native image id, for nvnMemoryPoolBuilderSetNativeHandleNVX */
int ns2_native_image_mem(u32 id, u64* a, u64* n){ NsImage* im=image_find(id); if(!im){ *a=*n=0; return 0; }
  *a=nvmap_addr(im->mem)+im->off; *n=im->size; return *a!=im->off || im->off; }
