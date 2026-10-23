/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Explicit external input packet, not recovered sensor/ISP arithmetic.
 * Matches temporal_bayer_input.py: LE magic/time_ns/CRC16-CCITT in nine
 *64x24 CFA-aligned patches. No DMA, IRQ, camera settings or CPU access. */
#ifndef NIKON_TEMPORAL_BAYER_H
#define NIKON_TEMPORAL_BAYER_H
#include <stdbool.h>
#include <stdint.h>

static bool nikon_temporal_bayer_stamp(uint16_t *samples, unsigned width,
    unsigned height, unsigned white, uint64_t virtual_ns)
{
    if (!samples || width < 192 || height < 72 || width > 16384 ||
        height > 16384 || (uint64_t)width*height > UINT64_C(64)*1024*1024 || ((width | height) & 1) || white < 8 || white > 65535) {
        return false;
    }
    uint8_t packet[12] = {0x5c, 0xa7};
    for (unsigned i = 0; i < 8; i++) {
        packet[2+i] = virtual_ns >> (i*8);
    }
    uint16_t crc = 0xffff;
    for (unsigned i = 0; i < 10; i++) {
        crc ^= (uint16_t)packet[i] << 8;
        for (unsigned bit = 0; bit < 8; bit++) {
            crc = (uint16_t)((crc << 1) ^ ((crc & 0x8000) ? 0x1021 : 0));
        }
    }
    packet[10] = crc;
    packet[11] = crc >> 8;
    unsigned xs[3] = {0, (width-64)/4*2, width-64};
    unsigned ys[3] = {0, (height-24)/4*2, height-24};
    uint16_t low = white/8, high = white-white/8;
    for (unsigned oy = 0; oy < 3; oy++) {
        for (unsigned ox = 0; ox < 3; ox++) {
            for (unsigned bit = 0; bit < 96; bit++) {
                uint16_t value = (packet[bit/8] >> (bit&7)) & 1 ? high : low;
                unsigned x = xs[ox]+(bit%16)*4, y = ys[oy]+(bit/16)*4;
                for (unsigned dy = 0; dy < 4; dy++) {
                    for (unsigned dx = 0; dx < 4; dx++) {
                        samples[(uint64_t)(y+dy)*width+x+dx] = value;
                    }
                }
            }
        }
    }
    return true;
}
#endif
