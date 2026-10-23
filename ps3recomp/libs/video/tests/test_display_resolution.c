#include <assert.h>
#include "../rsx_display_resolution.h"
int main(void)
{
    unsigned w=0,h=0,cw,ch;
    assert(rsx_display_resolution("1024x768",&w,&h));
    rsx_display_fit(w,h,1280,720,&cw,&ch);
    assert(cw==1024 && ch==576);
    rsx_display_fit(1280,1024,1280,720,&cw,&ch);
    assert(cw==1280 && ch==720);
    rsx_display_fit(1920,1200,1280,720,&cw,&ch);
    assert(cw==1920 && ch==1080);
    rsx_display_fit(1280,800,1280,720,&cw,&ch);
    assert(cw==1280 && ch==720);
    rsx_display_fit(3440,1440,1280,720,&cw,&ch);
    assert(cw==2560 && ch==1440);
    rsx_display_fit(3840,2160,1280,720,&cw,&ch);
    assert(cw==3840 && ch==2160);
    assert(!rsx_display_resolution("1.5",&w,&h));
    assert(!rsx_display_resolution("1920x1080junk",&w,&h));
    assert(!rsx_display_resolution("0x0",&w,&h));
    assert(!rsx_display_resolution("99999x99999",&w,&h));
    return 0;
}
