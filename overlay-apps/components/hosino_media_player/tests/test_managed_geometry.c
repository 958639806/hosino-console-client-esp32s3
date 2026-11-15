#include "hosino_mjpeg_metadata.h"
#include <assert.h>
#include <stdio.h>
int main(void)
{
    uint16_t w=0,h=0,fps=0;
    assert(hosino_mjpeg_apply_metadata(false,true,320,180,20,&w,&h,&fps)&&w==0&&h==0&&fps==0);
    assert(hosino_mjpeg_apply_metadata(true,false,320,180,20,&w,&h,&fps)&&w==0&&h==0&&fps==0);
    assert(hosino_mjpeg_apply_metadata(true,true,320,180,20,&w,&h,&fps)&&w==320&&h==180&&fps==20);
    w=480;h=270;fps=15;
    assert(hosino_mjpeg_apply_metadata(true,true,480,270,15,&w,&h,&fps)&&w==480&&h==270&&fps==15);
    w=0;h=270;fps=0;
    assert(hosino_mjpeg_apply_metadata(true,true,480,270,15,&w,&h,&fps)&&w==480&&h==270&&fps==15);
    w=640;h=360;fps=30;
    assert(!hosino_mjpeg_apply_metadata(true,true,320,180,20,&w,&h,&fps)&&w==640&&h==360);
    w=0;h=0;fps=0;
    assert(!hosino_mjpeg_apply_metadata(true,true,480,480,15,&w,&h,&fps)&&w==0&&h==0);
    assert(!hosino_mjpeg_apply_metadata(true,true,480,270,30,&w,&h,&fps));
    assert(hosino_mjpeg_decoded_geometry(320,180,320,180,20));
    assert(hosino_mjpeg_decoded_geometry(320,184,320,180,20));
    assert(hosino_mjpeg_decoded_geometry(320,192,320,180,20));
    assert(hosino_mjpeg_decoded_geometry(480,272,480,270,15));
    assert(!hosino_mjpeg_decoded_geometry(0,0,320,180,20));
    assert(!hosino_mjpeg_decoded_geometry(321,180,320,180,20));
    assert(!hosino_mjpeg_decoded_geometry(320,181,320,180,20));
    assert(!hosino_mjpeg_decoded_geometry(640,360,320,180,20));
    assert(!hosino_mjpeg_decoded_geometry(480,480,480,270,15));
    puts("MANAGED_FLV_ZERO_ONLY_METADATA_AND_REAL_DECODE_GEOMETRY_GUARDS_PASS");
    return 0;
}
