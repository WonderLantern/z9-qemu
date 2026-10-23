/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <glib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#define MiB (1024 * 1024)
static uint16_t lduw_le_p(const void *p) {
    const uint8_t *b = p;
    return b[0] | ((uint16_t)b[1] << 8);
}
#include "../qemu/nikon-raw-pixels.h"
static void check_ten_bit(void) {
    /* A complete grey ramp is independent of the Bayer interpolation: all
     * four CFA sites have the same value. Check direct sRGB quantization,
     * rather than comparing this implementation with its own tone table. */
    unsigned non_eight_bit = 0;
    for (unsigned value=0; value<=4095; value++) {
        uint8_t raw[8];
        for (unsigned i=0;i<4;i++) { raw[2*i]=value; raw[2*i+1]=value>>8; }
        NikonRawPixels *p=nikon_raw_pixels_new(raw,2,2,0,4095);
        uint16_t *out=nikon_raw_pixels_yuv10(p,8,6);
        assert(out);
        double linear=(double)value/4095;
        double encoded=linear<=0.0031308?12.92*linear:
            1.055*pow(linear,1.0/2.4)-0.055;
        unsigned wanted=lrint(encoded*1023);
        for (unsigned i=0;i<48;i++) { assert(out[i]==wanted); }
        for (unsigned i=48;i<72;i++) { assert(out[i]==512); }
        unsigned eight=lrint(encoded*255);
        non_eight_bit += wanted != (eight*1023+127)/255;
        assert(!nikon_raw_pixels_yuv10(p,7,6));
        assert(!nikon_raw_pixels_yuv10(p,8,0));
        assert(!nikon_raw_pixels_yuv10(p,4098,2));
        g_free(out);nikon_raw_pixels_free(p);
    }
    assert(non_eight_bit>1000);
    assert(!nikon_raw_pixels_yuv10(NULL,8,6));
}
int main(void) {
    check_ten_bit();
    const uint8_t red[8] = {255,15,0,0,0,0,0,0};
    const uint8_t white[8] = {255,15,255,15,255,15,255,15};
    const uint8_t black[8] = {0};
    const uint8_t *inputs[] = {red, white, black};
    const int expected[][3] = {{77,85,255},{255,128,128},{0,128,128}};
    for (unsigned i=0;i<3;i++) {
        NikonRawPixels *p=nikon_raw_pixels_new(inputs[i],2,2,0,4095);
        assert(p);
        for (unsigned half=0;half<2;half++) {
            uint8_t *out=nikon_raw_pixels_yuv(p,8,6,10,half);
            assert(out);
            for (unsigned y=0;y<6;y++) {
                for (unsigned x=0;x<8;x++) assert(abs(out[y*10+x]-expected[i][0])<=1);
                assert(out[y*10+8]==0 && out[y*10+9]==0);
            }
            for (unsigned y=0;y<(half?3:6);y++) {
                for (unsigned x=0;x<8;x+=2) {
                    assert(abs(out[60+y*10+x]-expected[i][1])<=1);
                    assert(abs(out[61+y*10+x]-expected[i][2])<=1);
                }
                assert(out[68+y*10]==128 && out[69+y*10]==128);
            }
            g_free(out);
        }
        assert(!nikon_raw_pixels_yuv(p,8,6,6,true));
        assert(!nikon_raw_pixels_yuv(p,7,6,8,true));
        nikon_raw_pixels_free(p);
    }
    assert(!nikon_raw_pixels_new(red,1,2,0,4095));
    assert(!nikon_raw_pixels_new(red,2,2,4095,4095));
    return 0;
}
