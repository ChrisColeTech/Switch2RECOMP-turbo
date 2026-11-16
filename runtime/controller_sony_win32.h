/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the GNU Affero General Public License v3.0, see LICENSE.txt and NOTICE.txt.
   Forks and derived works have to keep the credit to SwitchRecomp by MZ Labs and the startup screen. */
/* included by the Win32 backend and the standalone device probe. no extra link libraries. */
#ifndef SWR_CONTROLLER_SONY_WIN32_H
#define SWR_CONTROLLER_SONY_WIN32_H
#include <windows.h>
#include <setupapi.h>
#include <hidsdi.h>
#include "controller_sony.h"

#define SONY_API_LIST(X) \
  X(hid,HidD_GetHidGuid) X(hid,HidD_GetAttributes) X(hid,HidD_GetPreparsedData) \
  X(hid,HidD_FreePreparsedData) X(hid,HidP_GetCaps) \
  X(setup,SetupDiGetClassDevsW) X(setup,SetupDiEnumDeviceInterfaces) \
  X(setup,SetupDiGetDeviceInterfaceDetailW) X(setup,SetupDiDestroyDeviceInfoList)
#define SONY_DECLARE(d,n) static __typeof__(&n) ps_##n;
SONY_API_LIST(SONY_DECLARE)
#undef SONY_DECLARE
static int sony_api(void){
  static int loaded; if(loaded) return loaded>0;
  HMODULE hid=LoadLibraryA("hid.dll"),setup=LoadLibraryA("setupapi.dll"); loaded=-1;
  if(!hid || !setup) return 0;
#define SONY_LOAD(d,n) ps_##n=(__typeof__(&n))GetProcAddress(d,#n); if(!ps_##n) return 0;
  SONY_API_LIST(SONY_LOAD)
#undef SONY_LOAD
  loaded=1; return 1;
}
#undef SONY_API_LIST

typedef struct {
  HANDLE h; OVERLAPPED read; uint8_t input[78]; SonyState state;
  int model,transport,writable,pending,valid,lit,seq,output_failed;
  unsigned input_len,output_len,pid,writes,errors; ULONGLONG last_input;
} SonyDevice;
static SonyDevice g_sony;

static int sony_write(SonyDevice* d,unsigned low,unsigned high,int lit,int reset){
  if(!d->h || !d->writable) return 0;
  uint8_t packet[78]={0}; DWORD done=0,err=0;
  size_t n=sony_output(d->model,d->transport,d->seq++,low,high,lit,reset,packet,sizeof packet);
  if(n==48 && d->output_len==63) n=63;
  if(!n || n!=d->output_len) return 0;
  OVERLAPPED ov={0}; ov.hEvent=CreateEventW(NULL,TRUE,FALSE,NULL); if(!ov.hEvent) return 0;
  BOOL ok=WriteFile(d->h,packet,(DWORD)n,&done,&ov);
  if(!ok){ err=GetLastError(); if(err==ERROR_IO_PENDING){
      DWORD wait=WaitForSingleObject(ov.hEvent,30);
      if(wait!=WAIT_OBJECT_0){ CancelIoEx(d->h,&ov); GetOverlappedResult(d->h,&ov,&done,TRUE); err=ERROR_TIMEOUT; }
      else { ok=GetOverlappedResult(d->h,&ov,&done,FALSE); err=ok?0:GetLastError(); }
    } }
  CloseHandle(ov.hEvent); ok=ok && done==n;
  if(!ok){ d->output_failed=1; if(d->errors++<8) LOG("controllers: Sony output FAILED id=%02x bytes=%lu/%u error=%lu\n",packet[0],(unsigned long)done,(unsigned)n,(unsigned long)err); }
  else { if(d->writes++<8 || getenv("SWR_CONTROLLER_TRACE")) LOG("controllers: Sony output sent id=%02x bytes=%u low=%u high=%u player1=%d reset=%d\n",packet[0],(unsigned)n,low,high,lit,reset); d->lit=lit; }
  return ok;
}
static void sony_close(SonyDevice* d){
  if(!d->h) return;
  /* one final bounded zero, including after a failed nonzero write. */
  if(d->writable) sony_write(d,0,0,0,0);
  if(d->pending){ DWORD n; CancelIoEx(d->h,&d->read); GetOverlappedResult(d->h,&d->read,&n,TRUE); }
  CloseHandle(d->read.hEvent); CloseHandle(d->h); memset(d,0,sizeof *d);
}
static int sony_open(SonyDevice* d,int output,int diagnostic){
  if(!sony_api()) return 0;
  GUID guid; ps_HidD_GetHidGuid(&guid);
  HDEVINFO list=ps_SetupDiGetClassDevsW(&guid,NULL,NULL,DIGCF_PRESENT|DIGCF_DEVICEINTERFACE);
  if(list==INVALID_HANDLE_VALUE) return 0;
  /* USB first: one controller can have a remembered Bluetooth interface too. */
  for(int bus=SONY_USB;bus<=SONY_BT && !d->h;bus++) for(DWORD i=0;;i++){
    SP_DEVICE_INTERFACE_DATA iface={0}; iface.cbSize=sizeof iface;
    if(!ps_SetupDiEnumDeviceInterfaces(list,NULL,&guid,i,&iface)) break;
    DWORD needed=0; ps_SetupDiGetDeviceInterfaceDetailW(list,&iface,NULL,0,&needed,NULL);
    if(!needed) continue;
    PSP_DEVICE_INTERFACE_DETAIL_DATA_W detail=malloc(needed); if(!detail) break;
    detail->cbSize=sizeof *detail;
    if(!ps_SetupDiGetDeviceInterfaceDetailW(list,&iface,detail,needed,NULL,NULL)){ free(detail); continue; }
    HANDLE h=CreateFileW(detail->DevicePath,0,FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,OPEN_EXISTING,0,NULL);
    HIDD_ATTRIBUTES a={0}; a.Size=sizeof a; HIDP_CAPS caps={0}; PHIDP_PREPARSED_DATA pp=NULL;
    int model=0,transport=0;
    if(h!=INVALID_HANDLE_VALUE){
      if(ps_HidD_GetAttributes(h,&a)) model=sony_model(a.VendorID,a.ProductID);
      if(model && ps_HidD_GetPreparsedData(h,&pp)){ if(ps_HidP_GetCaps(pp,&caps)==HIDP_STATUS_SUCCESS && caps.UsagePage==1 && (caps.Usage==4 || caps.Usage==5))
          transport=sony_transport(model,caps.InputReportByteLength,caps.OutputReportByteLength);
        ps_HidD_FreePreparsedData(pp); }
      CloseHandle(h);
    }
    if(model && diagnostic && bus==SONY_USB) LOG("controllers: HID candidate %04x:%04x usage=%x:%x input=%u output=%u transport=%s\n",a.VendorID,a.ProductID,caps.UsagePage,caps.Usage,caps.InputReportByteLength,caps.OutputReportByteLength,transport==SONY_USB?"USB":transport==SONY_BT?"Bluetooth":"unsupported");
    if(!transport || transport!=bus){ free(detail); continue; }
    /* deny concurrent writers while owning output. never request an exclusive input handle. */
    h=CreateFileW(detail->DevicePath,GENERIC_READ|(output?GENERIC_WRITE:0),output?FILE_SHARE_READ:FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,OPEN_EXISTING,FILE_FLAG_OVERLAPPED,NULL);
    int writable=output;
    if(h==INVALID_HANDLE_VALUE && output){
      if(diagnostic) LOG("controllers: Sony output ownership unavailable error=%lu; trying shared input\n",(unsigned long)GetLastError());
      h=CreateFileW(detail->DevicePath,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,OPEN_EXISTING,FILE_FLAG_OVERLAPPED,NULL); writable=0;
    }
    free(detail);
    if(h==INVALID_HANDLE_VALUE){ if(diagnostic) LOG("controllers: Sony input open failed error=%lu\n",(unsigned long)GetLastError()); continue; }
    memset(d,0,sizeof *d); d->h=h; d->model=model; d->transport=transport; d->pid=a.ProductID;
    d->writable=writable; d->input_len=caps.InputReportByteLength; d->output_len=caps.OutputReportByteLength;
    d->read.hEvent=CreateEventW(NULL,TRUE,FALSE,NULL); d->last_input=GetTickCount64();
    if(!d->read.hEvent){ CloseHandle(h); memset(d,0,sizeof *d); continue; }
    LOG("controllers: %s%s %04x:%04x %s backend=native-HID input=%u output=%u ownership=%s\n",model==SONY_DS5?"DualSense":"DualShock 4",a.ProductID==0x0df2?" Edge":"",a.VendorID,a.ProductID,transport==SONY_USB?"USB":"Bluetooth",d->input_len,d->output_len,writable?"input+LED+rumble":"shared input only");
    break;
  }
  ps_SetupDiDestroyDeviceInfoList(list); return d->h!=NULL;
}
/* drain queued reports without blocking; incomplete reports never change the held state. */
static int sony_poll(SonyDevice* d){
  if(!d->h) return 0;
  for(int i=0;i<64;i++){
    DWORD n=0; BOOL ok;
    if(d->pending){ ok=GetOverlappedResult(d->h,&d->read,&n,FALSE);
      if(!ok && GetLastError()==ERROR_IO_INCOMPLETE) break; d->pending=0;
    } else {
      ResetEvent(d->read.hEvent); ok=ReadFile(d->h,d->input,d->input_len,&n,&d->read);
      if(!ok && GetLastError()==ERROR_IO_PENDING){ d->pending=1; break; }
    }
    if(!ok){ LOG("controllers: Sony input disconnected/error=%lu\n",(unsigned long)GetLastError()); sony_close(d); return 0; }
    uint64_t old=d->state.buttons;
    if(sony_input(d->model,d->transport,d->input,n,&d->state)){
      if(old!=d->state.buttons && getenv("SWR_CONTROLLER_TRACE")) LOG("controllers: Sony input report=%02x bytes=%lu buttons=%016llx\n",d->input[0],(unsigned long)n,(unsigned long long)d->state.buttons);
      d->valid=1; d->last_input=GetTickCount64(); }
    else if(d->errors++<8) LOG("controllers: Sony input rejected id=%02x bytes=%lu (length/report/CRC)\n",n?d->input[0]:0,(unsigned long)n);
  }
  if(GetTickCount64()-d->last_input>1000){ LOG("controllers: Sony input timed out; releasing held buttons\n"); sony_close(d); return 0; }
  return d->valid;
}
#endif
