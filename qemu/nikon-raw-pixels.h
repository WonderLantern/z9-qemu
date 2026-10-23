/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Functional RGGB cell demosaic and resampling, shared by lab ISP models.
 * Based on the project's Z7 pixel path, with explicit input dimensions/range.
 * No guest-memory, camera-specific register, interrupt or display access. */
#include <math.h>

/* Bounded 16-bit containers cover both Z9 and ZR full still frames.
 * These bounds describe host input storage, not a sensor wire format. */
#define NIKON_RAW_MAX_DIMENSION 16384U
#define NIKON_RAW_MAX_SAMPLES (UINT64_C(64) * MiB)

typedef struct NikonRawPixels {
    unsigned width, height, white;
    uint16_t *samples;
    uint8_t *tone;
    uint16_t *tone10;
} NikonRawPixels;

static NikonRawPixels *nikon_raw_pixels_new(const uint8_t *samples,
    unsigned width, unsigned height, unsigned black, unsigned white)
{
    if (!width || !height || ((width | height) & 1) || width > NIKON_RAW_MAX_DIMENSION ||
        height > NIKON_RAW_MAX_DIMENSION ||
        (uint64_t)width * height > NIKON_RAW_MAX_SAMPLES || black >= white || white > 65535) {
        return NULL;
    }
    NikonRawPixels *p = g_new0(NikonRawPixels, 1);
    p->width = width; p->height = height; p->white = white;
    p->samples = g_new(uint16_t, (size_t)width * height);
    p->tone = g_malloc(65536);
    p->tone10 = g_new(uint16_t, 65536);
    for (unsigned i = 0; i < 65536; i++) {
        double v = CLAMP(((double)i - black) / (white - black), 0, 1);
        v = v <= 0.0031308 ? 12.92 * v : 1.055 * pow(v, 1.0 / 2.4) - 0.055;
        p->tone[i] = CLAMP(lrint(v * 255), 0, 255);
        p->tone10[i] = CLAMP(lrint(v * 1023), 0, 1023);
    }
    for (size_t i = 0; i < (size_t)width * height; i++) {
        p->samples[i] = lduw_le_p(samples + i * 2);
    }
    return p;
}

static void nikon_raw_pixels_rgb(const NikonRawPixels *p, unsigned x, unsigned y,
                                unsigned w, unsigned h, uint8_t rgb[3])
{
    unsigned sx = MIN(p->width / 2 - 1, (uint64_t)x * (p->width / 2) / w) * 2;
    unsigned sy = MIN(p->height / 2 - 1, (uint64_t)y * (p->height / 2) / h) * 2;
    const uint16_t *q = p->samples + (size_t)sy * p->width + sx;
    rgb[0] = p->tone[q[0]];
    rgb[1] = p->tone[((unsigned)q[1] + q[p->width] + 1) / 2];
    rgb[2] = p->tone[q[p->width + 1]];
}

/* Full-range BT.601 functional output. uv_half=true is NV12; false is NV16.
 * Padding is initialized, source samples remain immutable. */
static uint8_t *nikon_raw_pixels_yuv(const NikonRawPixels *p, unsigned w,
                                   unsigned h, unsigned stride, bool uv_half)
{
    if (!p || !w || !h || ((w | h | stride) & 1) || w > 4096 || h > 4096 ||
        stride < w || stride > 8192 || (uint64_t)stride * h > 16 * MiB) {
        return NULL;
    }
    size_t ybytes = (size_t)stride * h;
    uint8_t *data = g_malloc(ybytes + ybytes / (uv_half ? 2 : 1));
    uint8_t *uv = data + ybytes;
    memset(data, 0, ybytes);
    memset(uv, 128, ybytes / (uv_half ? 2 : 1));
    unsigned dymax = uv_half ? 2 : 1;
    for (unsigned y = 0; y < h; y += dymax) {
        for (unsigned x = 0; x < w; x += 2) {
            unsigned sums[3] = {0};
            for (unsigned dy = 0; dy < dymax; dy++) {
                for (unsigned dx = 0; dx < 2; dx++) {
                    uint8_t rgb[3];
                    nikon_raw_pixels_rgb(p, x + dx, y + dy, w, h, rgb);
                    data[(size_t)(y + dy) * stride + x + dx] =
                        (77 * rgb[0] + 150 * rgb[1] + 29 * rgb[2] + 128) >> 8;
                    for (unsigned c = 0; c < 3; c++) { sums[c] += rgb[c]; }
                }
            }
            unsigned count = 2 * dymax;
            int red = (sums[0] + count / 2) / count;
            int green = (sums[1] + count / 2) / count;
            int blue = (sums[2] + count / 2) / count;
            uv[(size_t)(y / dymax) * stride + x] =
                CLAMP(((-43 * red - 85 * green + 128 * blue + 128) >> 8) + 128, 0, 255);
            uv[(size_t)(y / dymax) * stride + x + 1] =
                CLAMP(((128 * red - 107 * green - 21 * blue + 128) >> 8) + 128, 0, 255);
        }
    }
    return data;
}

/* Full-range functional BT.601 NV12 samples in host uint16_t containers.
 * Tone mapping and chroma reduction retain ten component bits throughout;
 * this is authored functional processing, not the original Nikon ISP. */
static uint16_t *nikon_raw_pixels_yuv10(const NikonRawPixels *p, unsigned w,
                                       unsigned h)
{
    if (!p || !p->tone10 || !w || !h || ((w | h) & 1) ||
        w > 4096 || h > 4096 || (uint64_t)w * h > 16 * MiB) {
        return NULL;
    }
    size_t ycount = (size_t)w * h;
    uint16_t *data = g_new0(uint16_t, ycount + ycount / 2);
    uint16_t *uv = data + ycount;
    for (unsigned y = 0; y < h; y += 2) {
        for (unsigned x = 0; x < w; x += 2) {
            unsigned sums[3] = {0};
            for (unsigned dy = 0; dy < 2; dy++) {
                for (unsigned dx = 0; dx < 2; dx++) {
                    unsigned sx = (uint64_t)(x + dx) * (p->width / 2) / w * 2;
                    unsigned sy = (uint64_t)(y + dy) * (p->height / 2) / h * 2;
                    const uint16_t *q = p->samples + (size_t)sy * p->width + sx;
                    unsigned rgb[3] = {p->tone10[q[0]],
                        p->tone10[((unsigned)q[1] + q[p->width] + 1) / 2],
                        p->tone10[q[p->width + 1]]};
                    data[(size_t)(y + dy) * w + x + dx] =
                        (77 * rgb[0] + 150 * rgb[1] + 29 * rgb[2] + 128) >> 8;
                    for (unsigned c = 0; c < 3; c++) { sums[c] += rgb[c]; }
                }
            }
            int red = (sums[0] + 2) / 4;
            int green = (sums[1] + 2) / 4;
            int blue = (sums[2] + 2) / 4;
            uv[(size_t)(y / 2) * w + x] =
                CLAMP(((-43 * red - 85 * green + 128 * blue + 128) >> 8) + 512, 0, 1023);
            uv[(size_t)(y / 2) * w + x + 1] =
                CLAMP(((128 * red - 107 * green - 21 * blue + 128) >> 8) + 512, 0, 1023);
        }
    }
    return data;
}

static void nikon_raw_pixels_free(NikonRawPixels *p)
{
    if (!p) { return; }
    g_free(p->samples); g_free(p->tone); g_free(p->tone10); g_free(p);
}
