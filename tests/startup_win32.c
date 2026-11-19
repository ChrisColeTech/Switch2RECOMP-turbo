/* Copyright 2026 ChrisColeTech
   SPDX-License-Identifier: MIT */
/* exercise the production painter on a memory DC, without a window or game. */
#include <windows.h>
#include <assert.h>
static HDC test_dc;
static RECT test_rect;
static HDC test_begin(HWND h,PAINTSTRUCT* ps){ (void)h; (void)ps; return test_dc; }
static BOOL test_end(HWND h,const PAINTSTRUCT* ps){ (void)h; (void)ps; return TRUE; }
static BOOL test_client(HWND h,RECT* r){ (void)h; *r=test_rect; return TRUE; }
#define BeginPaint test_begin
#define EndPaint test_end
#define GetClientRect test_client
#include "startup_production.h"

int main(void){
  test_dc=CreateCompatibleDC(NULL); assert(test_dc);
  for(int extra=0;extra<=160;extra+=160){
    int w=SPLASH_W+extra, h=SPLASH_H; test_rect=(RECT){0,0,w,h};
    BITMAPINFO bi={0}; bi.bmiHeader.biSize=sizeof bi.bmiHeader; bi.bmiHeader.biWidth=w; bi.bmiHeader.biHeight=-h;
    bi.bmiHeader.biPlanes=1; bi.bmiHeader.biBitCount=32; bi.bmiHeader.biCompression=BI_RGB;
    u32* pixels; HBITMAP bitmap=CreateDIBSection(test_dc,&bi,DIB_RGB_COLORS,(void**)&pixels,NULL,0); assert(bitmap);
    HGDIOBJ old=SelectObject(test_dc,bitmap);
    g_startup_px=malloc(SPLASH_W*SPLASH_H); assert(g_startup_px); splash_unpack(g_startup_px);
    startup_paint(NULL); GdiFlush();
    for(int y=0;y<h;y++) for(int x=0;x<w;x++){
      int sx=x-extra/2; u32 v=sx<0 || sx>=SPLASH_W?0:g_startup_px[y*SPLASH_W+sx];
      assert((pixels[y*w+x]&0xffffff)==v*0x010101);
    }
    startup_end(); assert(!g_startup_px);
    for(int i=0;i<w*h;i++) pixels[i]=0x00345678;
    for(int n=0;n<3;n++) startup_paint(NULL);
    GdiFlush(); for(int i=0;i<w*h;i++) assert(pixels[i]==0x00345678);
    SelectObject(test_dc,old); DeleteObject(bitmap);
  }
  DeleteDC(test_dc); puts("PASS original startup pixels, black letterboxing and no painting after renderer handoff"); return 0;
}
