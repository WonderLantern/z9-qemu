/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Functional internal video RAW transport. This authored WLVR/zlib envelope
 * is a simulator-only lossless transport, NOT Nikon compression. Both ends
 * run in modeled devices; original firmware instructions are unchanged.
 * External Bayer -> packed14 FIFO -> real packet RAM -> checked unpack ->
 * functional ISP. No CPU/RTOS flags, media files or display pixels supplied.
 */
#include <zlib.h>
#include "nikon-he-runtime.h"

/* These are provenance records for actual RAM, not hardware FIFO entries.
 * CPU-owned R3D packets can outlive both receiver banks. Growing metadata
 * neither copies pixels nor acknowledges a firmware/software consumer. */
static unsigned video_raw_packet_slots(NikonZ9State *s)
{ return s->video_raw_receiver_banks_model ? 64+s->video_raw_packet_extra_count : 4; }

static WlRawPacketRecord *video_raw_packet_at(NikonZ9State *s,unsigned row)
{ return row<64 ? &s->video_raw_packets[row] : &s->video_raw_packet_extra[row-64]; }

static bool video_raw_record_posted(NikonZ9State *s,unsigned row)
{
    WlRawPacketRecord *p=video_raw_packet_at(s,row);
    if (!p->owned) { return false; }
    if (!s->video_raw_receiver_banks_model) { return true; }
    unsigned off=(0x120+p->descriptor_slot*8)/4;
    uint64_t bus=s->engine1530_config[off]|(uint64_t)s->engine1530_config[off+1]<<32,physical;
    return raw_pipeline_address(s,bus,64,&physical) && physical==p->address;
}


/* Explicit external CFA modes, independent of the receiver ROI. Select the
 * smallest covering declared frame (area, then width), never declaration order.
 * Defaults preserve origin-zero admission. Geometry does not establish physical
 * binning, optical margins, calibration or sensor mode identity. */
static char *get_video_raw_source_frame(Object *obj, Error **errp)
{
    NikonZ9State *s=NIKON_Z9_LAB(obj);
    GString *out=g_string_new(NULL);
    if (!s->video_raw_input_frames) { g_string_append(out,"0x0"); }
    for (unsigned i=0;i<s->video_raw_input_frames;i++) {
        g_string_append_printf(out,"%s%ux%u",i?";":"",
            s->video_raw_input_width[i],s->video_raw_input_height[i]);
    }
    return g_string_free(out,false);
}
static void set_video_raw_source_frame(Object *obj,const char *value,Error **errp)
{
    NikonZ9State *s=NIKON_Z9_LAB(obj);
    unsigned widths[4]={0},heights[4]={0};char tail;
    if (s->image_ram.ram || s->image_ram.rom_device) {
        error_setg(errp,"RAW source frame immutable after realization");return;
    }
    if (!value || !*value) {
        error_setg(errp,"bounded even RAW source WIDTHxHEIGHT modes required");return;
    }
    g_auto(GStrv) modes=g_strsplit(value,";",-1);
    unsigned count=g_strv_length(modes);
    if (!count || count>G_N_ELEMENTS(widths)) {
        error_setg(errp,"one to four explicit RAW source modes required");return;
    }
    for (unsigned i=0;i<count;i++) {
        unsigned w,h;
        if (sscanf(modes[i],"%ux%u%c",&w,&h,&tail)!=2 || !w || !h ||
            ((w|h)&1) || w>NIKON_RAW_MAX_DIMENSION || h>NIKON_RAW_MAX_DIMENSION ||
            (uint64_t)w*h>NIKON_RAW_MAX_SAMPLES) {
            error_setg(errp,"bounded even RAW source WIDTHxHEIGHT modes required");return;
        }
        for (unsigned j=0;j<i;j++) {
            if (widths[j]==w && heights[j]==h) {
                error_setg(errp,"duplicate RAW source mode");return;
            }
        }
        widths[i]=w;heights[i]=h;
    }
    memcpy(s->video_raw_input_width,widths,sizeof(widths));
    memcpy(s->video_raw_input_height,heights,sizeof(heights));
    s->video_raw_input_frames=count;
}

static bool video_raw_select_source(NikonZ9State *s,unsigned *width,unsigned *height)
{
    unsigned w=s->primary_d980_latched[1]>>16,h=s->primary_d980_latched[2]>>16;
    unsigned left=s->primary_d980_latched[1]&0xffff,top=s->primary_d980_latched[2]&0xffff;
    if (!s->video_raw_input_frames) {
        *width=w;*height=h;return !left && !top;
    }
    if ((left|top)&1) { return false; }
    bool found=false;
    uint64_t best=UINT64_MAX;
    for (unsigned i=0;i<s->video_raw_input_frames;i++) {
        unsigned fw=s->video_raw_input_width[i],fh=s->video_raw_input_height[i];
        if (w>fw || h>fh || left>fw-w || top>fh-h) { continue; }
        uint64_t area=(uint64_t)fw*fh;
        if (area<best || (area==best && fw<*width)) {
            *width=fw;*height=fh;best=area;found=true;
        }
    }
    return found;
}

static bool video_raw_descriptor(NikonZ9State *s)
{
    const uint32_t *c=s->primary_d980_latched;
    unsigned w=c[1]>>16,h=c[2]>>16,pitch=c[4];
    unsigned source_w=0,source_h=0;
    if (!video_raw_select_source(s,&source_w,&source_h)) { return false; }
    if (!s->video_raw_transport_model || !s->raw_pixels || c[0]!=1 ||
        !w || !h || ((w|h)&1) ||
        w>NIKON_RAW_MAX_DIMENSION || h>NIKON_RAW_MAX_DIMENSION ||
        (uint64_t)w*h>NIKON_RAW_MAX_SAMPLES ||
        pitch!=(((w*14+7)/8+31)&~31u) || (uint64_t)pitch*h>128*MiB ||
        c[3] || c[5]!=1 || c[6]!=1 || c[7] || c[8]!=1 || c[9] || c[10]!=3 || c[11]) { return false; }
    for (unsigned row=0;row<4;row++) {
        if (c[12+row*2]!=0xe0000000 || c[13+row*2]) { return false; }
    }
    return true;
}

static void video_raw_decoder_irq_update(NikonZ9State *s)
{
    if (s->video_raw_decoder_irq) {
        qemu_set_irq(s->video_raw_decoder_irq,
            (s->engine1530_config[0x1c/4] & s->engine1530_config[0x18/4])!=0);
    }
}

/* Four original receiver destinations are independent RAM leases. Producer
 * W1C advances its write cursor, but never consumes compressed image bytes.
 * No software-RAW/R3D consumer is invented: only the modeled decoder releases
 * a packet after a complete RAM read, header validation and checked decode.
 * This bounded receiver-before-decoder queue is opt-in. Once the decoder
 * runs, retain the established downstream backpressure instead of assuming
 * that a producer ACK permits speculative pipeline depth or STOP disposal. */
static unsigned video_raw_owned_packets(NikonZ9State *s)
{
    unsigned count=0;
    for (unsigned row=0;row<video_raw_packet_slots(s);row++) { count+=(*video_raw_packet_at(s,row)).owned; }
    return count;
}

static unsigned video_raw_oldest_packet(NikonZ9State *s)
{
    unsigned limit=video_raw_packet_slots(s),selected=limit;
    for (unsigned row=0;row<limit;row++) {
        if (video_raw_record_posted(s,row) &&
            (selected==limit || (*video_raw_packet_at(s,row)).epoch<(*video_raw_packet_at(s,selected)).epoch)) {
            selected=row;
        }
    }
    return selected;
}

static bool video_raw_has_decoder_packet(NikonZ9State *s)
{ return video_raw_oldest_packet(s)<video_raw_packet_slots(s); }

/* Decode only the complete actual receiver RAM. Host-source compression
 * caches are deliberately inaccessible here. Caller still validates the
 * descriptor, RAM lease, epoch, active request and pending IRQ ownership. */
static bool video_raw_he_pixels(NikonZ9State *s,uint64_t source,
                                unsigned capacity,unsigned w,unsigned h)
{
    if (capacity<256 || capacity>32*MiB || !dma_ram_range(s,source,capacity)) { return false; }
    uint8_t *packet=g_malloc(capacity);
    if (address_space_read(&address_space_memory,source,MEMTXATTRS_UNSPECIFIED,
                           packet,capacity)!=MEMTX_OK) { g_free(packet);return false; }
    if (s->video_raw_pixels && s->video_raw_decoded_packet &&
        s->video_raw_pixels->width==w && s->video_raw_pixels->height==h &&
        s->video_raw_decoded_packet_bytes==capacity &&
        !memcmp(packet,s->video_raw_decoded_packet,capacity)) {
        s->video_raw_decode_cache_hits++;g_free(packet);return true;
    }
    uint16_t *samples=g_malloc((uint64_t)w*h*2);
    if (!wl_he_decode14(packet,capacity,w,h,samples,(uint64_t)w*h)) {
        g_free(samples);g_free(packet);return false;
    }
    NikonRawPixels *pixels=nikon_raw_pixels_new((uint8_t *)samples,w,h,0,16383);g_free(samples);
    if (!pixels) { g_free(packet);return false; }
    nikon_raw_pixels_free(s->video_raw_pixels);s->video_raw_pixels=pixels;
    g_free(s->video_raw_decoded_packet);s->video_raw_decoded_packet=packet;
    s->video_raw_decoded_packet_bytes=capacity;s->video_raw_decompressions++;
    for (unsigned ch=0;ch<3;ch++) { g_free(s->raw_pipeline_cache[ch]);s->raw_pipeline_cache[ch]=NULL; }
    return true;
}

static bool video_raw_decode(NikonZ9State *s)
{
    if (!s->video_raw_transport_model || !s->video_raw_packet_owned || s->video_raw_frame_ready ||
        !s->engine1530_active || s->engine1530_config[0x404/4] ||
        s->engine1530_config[0x14/4]!=1 || s->engine15b0_config[0x38/4]!=1 ||
        (s->engine1530_config[0x1c/4]&4)) { return false; }
    unsigned row=s->video_raw_packet_ring_model ? video_raw_oldest_packet(s) : s->video_raw_packet_row;
    if (row>=video_raw_packet_slots(s)) { return false; }
    unsigned descriptor=s->video_raw_receiver_banks_model ? (*video_raw_packet_at(s,row)).descriptor_slot : row;
    unsigned off=(0x120+descriptor*8)/4;
    uint64_t expected_address=s->video_raw_packet_ring_model ?
        (*video_raw_packet_at(s,row)).address : s->video_raw_packet_address;
    uint64_t expected_epoch=s->video_raw_packet_ring_model ?
        (*video_raw_packet_at(s,row)).epoch : s->video_raw_epoch;
    uint32_t capacity=s->video_raw_packet_ring_model ?
        (*video_raw_packet_at(s,row)).capacity : s->engine152c_stream_capacity;
    unsigned expected_w=s->video_raw_packet_ring_model ? (*video_raw_packet_at(s,row)).width : s->video_raw_width;
    unsigned expected_h=s->video_raw_packet_ring_model ? (*video_raw_packet_at(s,row)).height : s->video_raw_height;
    uint64_t encoded=s->engine1530_config[off] | (uint64_t)s->engine1530_config[off+1]<<32,source;
    uint64_t bytes;
    if (!raw_pipeline_address(s,encoded,64,&source) || source!=expected_address) { goto invalid; }
    if (s->video_raw_he_codec_model) {
        if (!video_raw_he_pixels(s,source,capacity,expected_w,expected_h)) { goto invalid; }
        bytes=(uint64_t)((((expected_w*14+7)/8)+31)&~31u)*expected_h;
        goto completed;
    }
    uint8_t header[64];
    if (
        address_space_read(&address_space_memory,source,MEMTXATTRS_UNSPECIFIED,header,64)!=MEMTX_OK ||
        memcmp(header,"WLVR0001",8) || ldl_le_p(header+8)!=64 || ldl_le_p(header+12)!=1 ||
        ldq_le_p(header+16)!=expected_epoch) { goto invalid; }
    unsigned w=ldl_le_p(header+24),h=ldl_le_p(header+28),pitch=ldl_le_p(header+32);
    bytes=ldq_le_p(header+40);unsigned packed=ldl_le_p(header+48),crc=ldl_le_p(header+52);
    if (!w || !h || ((w|h)&1) || w>NIKON_RAW_MAX_DIMENSION || h>NIKON_RAW_MAX_DIMENSION ||
        (uint64_t)w*h>NIKON_RAW_MAX_SAMPLES || pitch!=(((w*14+7)/8+31)&~31u) ||
        bytes!=(uint64_t)pitch*h || bytes>128*MiB || ldl_le_p(header+36)!=14 ||
        capacity<64 || !packed || packed>capacity-64 ||
        !dma_ram_range(s,source+64,packed) || w!=expected_w || h!=expected_h) { goto invalid; }
    uint8_t *packet=g_malloc(packed);
    if (address_space_read(&address_space_memory,source+64,MEMTXATTRS_UNSPECIFIED,
                           packet,packed)!=MEMTX_OK) {
        g_free(packet);goto invalid;
    }
    /* Always read and compare the complete current RAM payload. Only an
     * exact copy of a previously decompressed and CRC-checked packet can
     * reuse its pixels; the encoder's host-source cache is never consulted.
     * Epoch, address, geometry, format and ownership checks above still run
     * on every frame. CRC equality alone is not content identity. */
    if (s->video_raw_pixels && s->video_raw_decoded_packet &&
        s->video_raw_pixels_crc==crc && s->video_raw_pixels->width==w &&
        s->video_raw_pixels->height==h && s->video_raw_decoded_packet_bytes==packed &&
        memcmp(packet,s->video_raw_decoded_packet,packed)==0) {
        s->video_raw_decode_cache_hits++;g_free(packet);
    } else {
        uint8_t *raw=g_malloc(bytes);uLongf actual=bytes;
        bool ok=uncompress(raw,&actual,packet,packed)==Z_OK && actual==bytes &&
            crc32(0,raw,bytes)==crc;
        if (!ok) { g_free(raw);g_free(packet);goto invalid; }
        uint8_t *samples=g_malloc((uint64_t)w*h*2);
        for (unsigned y=0;y<h;y++) {
            for (unsigned x=0;x<w;x++) {
                unsigned at=x*14/8,shift=(x*14)&7;
                uint32_t value=raw[(uint64_t)y*pitch+at] | ((uint32_t)raw[(uint64_t)y*pitch+at+1]<<8);
                if (shift>2) { value|=(uint32_t)raw[(uint64_t)y*pitch+at+2]<<16; }
                stw_le_p(samples+((uint64_t)y*w+x)*2,(value>>shift)&0x3fff);
            }
        }
        NikonRawPixels *pixels=nikon_raw_pixels_new(samples,w,h,0,16383);g_free(samples);
        g_free(raw);
        if (!pixels) { g_free(packet);goto invalid; }
        nikon_raw_pixels_free(s->video_raw_pixels);s->video_raw_pixels=pixels;
        s->video_raw_pixels_crc=crc;
        g_free(s->video_raw_decoded_packet);s->video_raw_decoded_packet=packet;
        s->video_raw_decoded_packet_bytes=packed;s->video_raw_decompressions++;
        for (unsigned ch=0;ch<3;ch++) { g_free(s->raw_pipeline_cache[ch]);s->raw_pipeline_cache[ch]=NULL; }
    }
completed:
    s->video_raw_decoded_epoch=expected_epoch;s->video_raw_decode_jobs++;
    s->video_raw_decoded_bytes+=bytes;s->video_raw_frame_ready=true;
    if (s->video_raw_packet_ring_model) { (*video_raw_packet_at(s,row)).owned=false; }
    s->video_raw_packet_owned=s->video_raw_packet_ring_model && video_raw_owned_packets(s);
    /* Original1530 ISR/W1C owns this completed internal decoder output. */
    s->engine1530_config[0x1c/4]|=4;video_raw_decoder_irq_update(s);
    return true;
invalid:
    s->video_raw_decode_rejects++;return false;
}

/* Functional even-phase ROI on the declared sensor frame. Sampling uses
 * full-frame coordinates before cropping; resizing directly to the ROI
 * would change every pixel's field of view. Physical ISP math is unverified. */
static uint8_t *video_raw_pack_window(const NikonRawPixels *p,
                                    unsigned source_w, unsigned source_h,
                                    unsigned left, unsigned top,
                                    unsigned width, unsigned height,
                                    unsigned pitch)
{
    uint8_t *raw = g_malloc0((size_t)pitch * height);
    for (unsigned y = 0; y < height; y++) {
        unsigned fy = y + top;
        unsigned sy = (uint64_t)(fy / 2) * (p->height / 2) /
                      (source_h / 2) * 2 + (fy & 1);
        for (unsigned x = 0; x < width; x += 4) {
            unsigned count = MIN(4, width - x);
            uint64_t packed = 0;
            for (unsigned j = 0; j < count; j++) {
                unsigned fx = x + j + left;
                unsigned sx = (uint64_t)(fx / 2) * (p->width / 2) /
                              (source_w / 2) * 2 + (fx & 1);
                uint16_t v = MIN(16383, ((uint64_t)p->samples[
                    (size_t)sy * p->width + sx] * 16383 + p->white / 2) /
                    p->white);
                packed |= (uint64_t)v << (14 * j);
            }
            for (unsigned byte = 0; byte < (count * 14 + 7) / 8; byte++) {
                raw[(size_t)y * pitch + x * 14 / 8 + byte] = packed >> (byte * 8);
            }
        }
    }
    return raw;
}

/* The native receiver posts four address pairs in either bank. Statusbit4
 * reaches the original callback which requests a new supplier quartet and
 * switches108. Signal it only after four whole RAM writes. A fresh bank needs
 * all eight newly written address words; W1C alone never replenishes it. */
static bool video_raw_receiver_bank_ready(NikonZ9State *s)
{
    if (!s->video_raw_receiver_banks_model || !s->video_raw_receiver_wait_bank) { return true; }
    unsigned bank=s->video_raw_receiver_bank^1,mask=0xffu<<(bank*8);
    const uint32_t *c=s->engine152c_config[0];
    if ((c[0x108/4]&1)!=bank || (s->video_raw_receiver_fresh&mask)!=mask) { return false; }
    uint64_t outputs[4];uint32_t capacity=s->engine152c_stream_capacity;
    for (unsigned row=0;row<4;row++) {
        unsigned off=(0x120+(bank*4+row)*8)/4;
        uint64_t encoded=c[off]|(uint64_t)c[off+1]<<32;
        if ((encoded&31) || !raw_pipeline_address(s,encoded,capacity,&outputs[row])) { return false; }
        for (unsigned j=0;j<row;j++) {
            if (outputs[row]<outputs[j]+capacity && outputs[j]<outputs[row]+capacity) { return false; }
        }
    }
    memcpy(s->engine152c_stream_outputs,outputs,sizeof(outputs));
    s->video_raw_receiver_fresh&=~mask;s->video_raw_receiver_bank=bank;
    s->video_raw_receiver_wait_bank=false;s->video_raw_receiver_latches++;
    return true;
}

static unsigned video_raw_packet_destination(NikonZ9State *s,uint64_t address,unsigned capacity)
{
    unsigned limit=video_raw_packet_slots(s);
    for (unsigned slot=0;slot<limit;slot++) {
        if (!(*video_raw_packet_at(s,slot)).owned ||
            (address<(*video_raw_packet_at(s,slot)).address+(*video_raw_packet_at(s,slot)).capacity &&
             (*video_raw_packet_at(s,slot)).address<address+capacity)) { return slot; }
    }
    /* Every descriptor and DMA range is still checked before this point.
     * Only metadata grows: all old RAM/epoch records remain unchanged. */
    unsigned count=s->video_raw_packet_extra_count;
    if (count>G_MAXUINT-128) { return G_MAXUINT; }
    WlRawPacketRecord *extra=g_try_realloc_n(s->video_raw_packet_extra,count+64,sizeof(*extra));
    if (!extra) { return G_MAXUINT; }
    memset(extra+count,0,64*sizeof(*extra));
    s->video_raw_packet_extra=extra;s->video_raw_packet_extra_count=count+64;
    return limit;
}

static void video_raw_packet_overwrite(NikonZ9State *s,uint64_t address,unsigned capacity)
{
    /* A fully posted next descriptor bank authorizes a new producer write.
     * Retire old metadata only when its actual RAM is overwritten. This is
     * explicit software repost, not a decoder completion or consumer ACK.
     * Disjoint older RAM remains readable and independently owned. */
    for (unsigned slot=0;slot<video_raw_packet_slots(s);slot++) {
        if ((*video_raw_packet_at(s,slot)).owned &&
            address<(*video_raw_packet_at(s,slot)).address+(*video_raw_packet_at(s,slot)).capacity &&
            (*video_raw_packet_at(s,slot)).address<address+capacity) {
            (*video_raw_packet_at(s,slot)).owned=false;s->video_raw_reposted_packets++;
        }
    }
}

static bool video_raw_receive(NikonZ9State *s)
{
    if (!video_raw_descriptor(s) || !s->event_timing_active || s->primary_reset_stage ||
        (s->video_forced_stop_model && (s->engine150c_stop_requested&0x4000)) ||
        !(s->engine150c_active&0x4000) || (s->engine_150c_init[0x18/4]&0x4000) ||
        !s->engine152c_stream_requested || s->engine152c_stream_pending ||
        s->primary_d980_pending || s->video_raw_frame_ready ||
        (s->video_raw_packet_owned && (!s->video_raw_packet_ring_model ||
         (s->engine1530_active && (!s->video_raw_receiver_banks_model || video_raw_has_decoder_packet(s))))) ||
        (!s->engine1530_active && !s->video_raw_queued_input_model) ||
        s->engine1530_config[0x404/4]) { return false; }
    if (!video_raw_receiver_bank_ready(s)) { return false; }
    unsigned window_w=s->primary_d980_latched[1]>>16;
    unsigned window_h=s->primary_d980_latched[2]>>16;
    unsigned source_w=0,source_h=0;
    if (!video_raw_select_source(s,&source_w,&source_h)) { return false; }
    unsigned origin_x=s->primary_d980_latched[1]&0xffff;
    unsigned origin_y=s->primary_d980_latched[2]&0xffff;
    unsigned w=window_w,h=window_h,left=origin_x,top=origin_y;
    unsigned pitch=s->primary_d980_latched[4],row=s->engine152c_stream_row;
    if (s->video_raw_queued_input_model) {
        w=s->engine152c_stream_width;h=s->engine152c_stream_height;
        left=s->video_raw_receiver_left;top=s->video_raw_receiver_top;
        if (!w || !h || ((w|h|left|top)&1) || s->video_raw_receiver_mode ||
            w>window_w || h>window_h || left>window_w-w || top>window_h-h) {
            s->video_raw_encode_rejects++;return false;
        }
        left+=origin_x;top+=origin_y;
        pitch=((w*14+7)/8+31)&~31u;
    }
    /* Do not overwrite any unconsumed packet, including an aliased span
     * from an older receiver request. A producer IRQ ACK is not a release.
     * In the paired sensor route, every RAW input must also be delivered to
     * the independent ISP source before the producer can accept a successor. */
    if (s->video_raw_packet_ring_model) {
        if (s->engine152c_stream_requested && video_secondary_sensor_route(s) &&
            (s->video_secondary_sensor_ready ||
             (s->video_raw_packet_owned &&
              s->video_secondary_parallel_epoch<s->video_raw_epoch))) { return false; }
        uint64_t destination=s->engine152c_stream_outputs[row];
        uint32_t span=s->engine152c_stream_capacity;
        for (unsigned slot=0;slot<4 && !s->video_raw_receiver_banks_model;slot++) {
            if ((*video_raw_packet_at(s,slot)).owned &&
                (slot==row || (destination<(*video_raw_packet_at(s,slot)).address+(*video_raw_packet_at(s,slot)).capacity &&
                 (*video_raw_packet_at(s,slot)).address<destination+span))) {
                s->video_raw_ring_full_waits++;return false;
            }
        }
    }
    uint32_t capacity=s->engine152c_stream_capacity;
    uint64_t output=s->engine152c_stream_outputs[row];
    if (w!=s->engine152c_stream_width || h!=s->engine152c_stream_height ||
        capacity<64 || !dma_ram_range(s,output,capacity)) { s->video_raw_encode_rejects++;return false; }
    unsigned packet_slot=s->video_raw_receiver_banks_model ? video_raw_packet_destination(s,output,capacity) : row;
    if (packet_slot>=video_raw_packet_slots(s)) { s->video_raw_ring_full_waits++;return false; }
    size_t bytes=(size_t)pitch*h;
    if (!s->video_raw_compressed ||
        (s->raw_temporal_input_model && s->video_raw_cache_input_frame!=s->raw_temporal_input_frames) ||
        w!=s->video_raw_width || h!=s->video_raw_height || pitch!=s->video_raw_pitch ||
        source_w!=s->video_raw_source_width || source_h!=s->video_raw_source_height ||
        left!=s->video_raw_crop_x || top!=s->video_raw_crop_y ||
        (s->video_raw_he_codec_model && s->video_raw_compressed_bytes!=capacity)) {
        uint8_t *raw=video_raw_pack_window(s->raw_pixels,source_w,source_h,left,top,w,h,pitch);
        uLongf amount=s->video_raw_he_codec_model ? capacity : compressBound(bytes);
        uint8_t *compressed=g_malloc(amount);
        int result=s->video_raw_he_codec_model ?
            (wl_he_encode14(raw,bytes,w,h,pitch,compressed,capacity) ? Z_OK : Z_DATA_ERROR) :
            compress2(compressed,&amount,raw,bytes,3);
        uint32_t checksum=crc32(0,raw,bytes);g_free(raw);
        if (result!=Z_OK || amount>capacity-(s->video_raw_he_codec_model ? 0 : 64)) { g_free(compressed);s->video_raw_encode_rejects++;return false; }
        g_free(s->video_raw_compressed);s->video_raw_compressed=compressed;
        s->video_raw_compressed_bytes=amount;s->video_raw_crc=checksum;
        s->video_raw_cache_input_frame=s->raw_temporal_input_frames;
        s->video_raw_width=w;s->video_raw_height=h;s->video_raw_pitch=pitch;
        s->video_raw_source_width=source_w;s->video_raw_source_height=source_h;
        s->video_raw_crop_x=left;s->video_raw_crop_y=top;
    }
    if (s->video_raw_compressed_bytes>capacity-(s->video_raw_he_codec_model ? 0 : 64)) { s->video_raw_encode_rejects++;return false; }
    uint8_t *packet=g_malloc0(capacity);
    if (s->video_raw_he_codec_model) {
        memcpy(packet,s->video_raw_compressed,capacity);
    } else {
        memcpy(packet,"WLVR0001",8);stl_le_p(packet+8,64);stl_le_p(packet+12,1);
        stq_le_p(packet+16,s->video_raw_epoch+1);stl_le_p(packet+24,w);stl_le_p(packet+28,h);
        stl_le_p(packet+32,pitch);stl_le_p(packet+36,14);stq_le_p(packet+40,bytes);
        stl_le_p(packet+48,s->video_raw_compressed_bytes);stl_le_p(packet+52,s->video_raw_crc);
        memcpy(packet+64,s->video_raw_compressed,s->video_raw_compressed_bytes);
    }
    if (s->video_raw_receiver_banks_model) { video_raw_packet_overwrite(s,output,capacity); }
    bool ok=address_space_write(&address_space_memory,output,MEMTXATTRS_UNSPECIFIED,packet,capacity)==MEMTX_OK;
    g_free(packet);
    if (!ok) { s->video_raw_encode_rejects++;return false; }
    s->video_raw_epoch++;s->video_raw_packet_owned=true;s->video_raw_packet_row=row;
    s->video_raw_packet_address=output;s->video_raw_encode_jobs++;s->video_raw_internal_bytes+=bytes;
    if (s->video_raw_packet_ring_model) {
        (*video_raw_packet_at(s,packet_slot)).owned=true;
        (*video_raw_packet_at(s,packet_slot)).address=output;(*video_raw_packet_at(s,packet_slot)).epoch=s->video_raw_epoch;
        (*video_raw_packet_at(s,packet_slot)).capacity=capacity;
        (*video_raw_packet_at(s,packet_slot)).width=w;(*video_raw_packet_at(s,packet_slot)).height=h;
        (*video_raw_packet_at(s,packet_slot)).descriptor_slot=s->video_raw_receiver_bank*4+row;
    }
    s->video_raw_packet_bytes+=capacity;s->engine152c_stream_jobs++;
    s->engine152c_stream_bytes+=capacity;s->engine152c_stream_pending|=4;
    if (s->video_raw_receiver_banks_model && !s->engine1530_active && row==3) {
        s->video_raw_receiver_wait_bank=true;s->video_raw_receiver_refills++;
        s->engine152c_stream_pending|=0x10;
    }
    /* The opt-in receiver makes owned input available before a downstream
     * decoder starts. Bit0 reaches the original client callback only after
     * this whole RAM write; bit2 remains packet completion. Masks/W1C stay
     * native-owned. The packet cannot be reused before actual consumption.
     * This boundary placement is functional, not measured silicon timing. */
    if (s->video_raw_queued_input_model) {
        if (s->video_raw_receiver_banks_model && !s->engine1530_active && row==3) {
            /* The original callback wakes its supplier for bit0 before it
             * stores bit4's bank-ready byte. Combining both causes exposes
             * a cross-CPU wake-before-ready window. Retain this actual packet
             * notification until the preceding bank cause is acknowledged;
             * the existing device clock then exposes it separately. */
            s->video_raw_receiver_notify_pending=true;
        } else {
            s->engine152c_stream_pending|=1;s->video_raw_input_events++;
        }
    }
    /* The D980 FIFO was internal, not a RAM DMA. Its old RAM-byte/frame
     * counters remain separate from this transport's actual packet writes. */
    /* This internal FIFO is consumed by152c, whose packet event owns the
     * write. It is not a finite D980 RAM transaction. Native389 masks out
     * group1 bit26; attaching that finite event stalls all later frames.
     * Receiver/decoder/output ownership already prevents every overwrite. */
    engine152c_stream_irq_update(s);
    return true;
}

static void video_raw_tick(void *opaque)
{
    NikonZ9State *s=opaque;
    timer_mod(s->video_raw_timer,qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)+33333333);
    if (s->video_raw_receiver_notify_pending) {
        if (!s->engine152c_stream_requested) {
            s->video_raw_receiver_notify_pending=false;
        } else if (!(s->engine152c_stream_pending&0x10)) {
            s->video_raw_receiver_notify_pending=false;
            s->engine152c_stream_pending|=1;s->video_raw_input_events++;
            engine152c_stream_irq_update(s);
        }
    }
    if (s->video_raw_packet_owned) { video_raw_decode(s); }
    video_backend_drain_tick(s);
    /* Input producer runs on the original source clock via D980 tick. */
}

static bool get_video_raw_he_codec_model(Object *obj, Error **errp)
{ return NIKON_Z9_LAB(obj)->video_raw_he_codec_model; }
static void set_video_raw_he_codec_model(Object *obj,bool value,Error **errp)
{
    NikonZ9State *s=NIKON_Z9_LAB(obj);
    if (s->image_ram.ram || s->image_ram.rom_device) {
        error_setg(errp,"functional HE codec immutable after realization");return;
    }
    s->video_raw_he_codec_model=value;
}
static bool get_video_raw_transport_model(Object *obj, Error **errp)
{ return NIKON_Z9_LAB(obj)->video_raw_transport_model; }
static bool get_video_raw_queued_input_model(Object *obj, Error **errp)
{ return NIKON_Z9_LAB(obj)->video_raw_queued_input_model; }
static void set_video_raw_queued_input_model(Object *obj, bool value, Error **errp)
{
    NikonZ9State *s=NIKON_Z9_LAB(obj);
    if (s->image_ram.ram || s->image_ram.rom_device) {
        error_setg(errp,"queued RAW input immutable after realization");return;
    }
    s->video_raw_queued_input_model=value;
}
static void set_video_raw_transport_model(Object *obj, bool value, Error **errp)
{
    NikonZ9State *s=NIKON_Z9_LAB(obj);
    if (s->image_ram.ram || s->image_ram.rom_device) {
        error_setg(errp,"video RAW transport immutable after realization");return;
    }
    s->video_raw_transport_model=value;
}
static char *get_video_raw_transport_state(Object *obj, Error **errp)
{
    NikonZ9State *s=NIKON_Z9_LAB(obj);
    char *state = g_strdup_printf("{\"enabled\":%s,\"epoch\":%" PRIu64 ",\"decoded_epoch\":%" PRIu64
        ",\"consumed_epoch\":%" PRIu64 ",\"encoder_jobs\":%" PRIu64 ",\"decoder_jobs\":%" PRIu64
        ",\"internal_raw_bytes\":%" PRIu64 ",\"packet_ram_bytes\":%" PRIu64 ",\"decoded_bytes\":%" PRIu64
        ",\"encoder_rejects\":%" PRIu64 ",\"decoder_rejects\":%" PRIu64 ",\"frame_ready\":%s,"
        "\"packet_owned\":%s,\"packet_address\":%" PRIu64 ",\"packet_row\":%u,\"compressed_bytes\":%u,"
        "\"width\":%u,\"height\":%u,\"pitch\":%u,\"ring_latched\":%s,\"ring_latches\":%" PRIu64 ","
        "\"nikon_codec_verified\":false,\"physical_sensor_verified\":false,"
        "\"scope\":\"functional RAW codec; actual packet RAM decode, Bayer ISP and native output ownership; original Nikon arithmetic unverified\"}",
        s->video_raw_transport_model ? "true" : "false",s->video_raw_epoch,s->video_raw_decoded_epoch,
        s->video_raw_consumed_epoch,s->video_raw_encode_jobs,s->video_raw_decode_jobs,
        s->video_raw_internal_bytes,s->video_raw_packet_bytes,s->video_raw_decoded_bytes,
        s->video_raw_encode_rejects,s->video_raw_decode_rejects,s->video_raw_frame_ready ? "true" : "false",
        s->video_raw_packet_owned ? "true" : "false",s->video_raw_packet_address,s->video_raw_packet_row,
        s->video_raw_compressed_bytes,s->video_raw_width,s->video_raw_height,s->video_raw_pitch,
        s->video_raw_ring_latched ? "true" : "false",s->video_raw_ring_latches);
    char *scoped=g_strdup_printf("%.*s,\"functional_he_codec\":%s,\"transport_format\":\"%s\"}",
        (int)strlen(state)-1,state,s->video_raw_he_codec_model ? "true" : "false",
        s->video_raw_he_codec_model ? "HE/JPEG-XS experimental predicted profile" : "WLVR/zlib");
    g_free(state);state=scoped;
    /* Read-only, same-QMP-call admission snapshot. In particular, cached
     * last-frame geometry is not the currently submitted D980 geometry.
     * These are device predicates, not a hardware topology assertion. */
    GString *out = g_string_new(state);
    g_free(state);
    g_string_truncate(out, out->len - 1);
    g_string_append_printf(out,
        ",\"admission\":{\"descriptor_valid\":%s,\"clock_active\":%s,"
        "\"reset_stage\":%u,\"source_active\":%u,\"source_hold\":%u,"
        "\"forced_stop_enabled\":%s,\"source_stop_requested\":%u,"
        "\"d980_pending\":%u,\"receiver_requested\":%s,"
        "\"receiver_pending\":%u,\"receiver_width\":%u,"
        "\"receiver_height\":%u,\"receiver_capacity\":%u,"
        "\"receiver_row\":%u,\"decoder_active\":%s,"
        "\"decoder_hold\":%u,\"decoder_gate\":%u,"
        "\"decoder_supplier_gate\":%u,\"decoder_pending\":%u,"
        "\"decoder_mask\":%u,\"submitted_width\":%u,"
        "\"submitted_height\":%u,\"submitted_pitch\":%u,"
        "\"queued_input_enabled\":%s,\"input_events\":%" PRIu64 ","
        "\"encoded_source_width\":%u,\"encoded_source_height\":%u,"
        "\"encoded_crop_x\":%u,\"encoded_crop_y\":%u,"
        "\"decoder_cache_hits\":%" PRIu64 ",\"decoder_decompressions\":%" PRIu64 "}}",
        video_raw_descriptor(s) ? "true" : "false",
        s->event_timing_active ? "true" : "false",
        s->primary_reset_stage, s->engine150c_active,
        s->engine_150c_init[0x18/4],
        s->video_forced_stop_model ? "true" : "false",
        s->engine150c_stop_requested, s->primary_d980_pending,
        s->engine152c_stream_requested ? "true" : "false",
        s->engine152c_stream_pending, s->engine152c_stream_width,
        s->engine152c_stream_height, s->engine152c_stream_capacity,
        s->engine152c_stream_row,
        s->engine1530_active ? "true" : "false",
        s->engine1530_config[0x404/4], s->engine1530_config[0x14/4],
        s->engine15b0_config[0x38/4], s->engine1530_config[0x1c/4],
        s->engine1530_config[0x18/4],
        s->primary_d980_latched[1] >> 16,
        s->primary_d980_latched[2] >> 16, s->primary_d980_latched[4],
        s->video_raw_queued_input_model ? "true" : "false",s->video_raw_input_events,
        s->video_raw_source_width,s->video_raw_source_height,
        s->video_raw_crop_x,s->video_raw_crop_y,
        s->video_raw_decode_cache_hits,s->video_raw_decompressions);
    return g_string_free(out, false);
}

static bool get_video_raw_packet_ring_model(Object *obj, Error **errp)
{ return NIKON_Z9_LAB(obj)->video_raw_packet_ring_model; }
static void set_video_raw_packet_ring_model(Object *obj, bool value, Error **errp)
{
    NikonZ9State *s=NIKON_Z9_LAB(obj);
    if (s->image_ram.ram || s->image_ram.rom_device) {
        error_setg(errp,"RAW packet ring immutable after realization");return;
    }
    s->video_raw_packet_ring_model=value;
}
static char *get_video_raw_packet_ring_state(Object *obj, Error **errp)
{
    NikonZ9State *s=NIKON_Z9_LAB(obj);
    GString *out=g_string_new(NULL);
    g_string_append_printf(out,"{\"enabled\":%s,\"owned\":%u,\"full_waits\":%"PRIu64",\"slots\":[",
        s->video_raw_packet_ring_model?"true":"false",video_raw_owned_packets(s),s->video_raw_ring_full_waits);
    for (unsigned row=0;row<video_raw_packet_slots(s);row++) {
        g_string_append_printf(out,"%s{\"row\":%u,\"owned\":%s,\"epoch\":%"PRIu64
            ",\"address\":%"PRIu64",\"capacity\":%u,\"width\":%u,\"height\":%u}",row?",":"",row,
            (*video_raw_packet_at(s,row)).owned?"true":"false",(*video_raw_packet_at(s,row)).epoch,
            (*video_raw_packet_at(s,row)).address,(*video_raw_packet_at(s,row)).capacity,
            (*video_raw_packet_at(s,row)).width,(*video_raw_packet_at(s,row)).height);
    }
    g_string_append_printf(out,"],\"banks_enabled\":%s,\"bank\":%u,\"wait_bank\":%s,"
        "\"fresh_words\":%u,\"refills\":%"PRIu64",\"bank_latches\":%"PRIu64",\"reposted_packets\":%"PRIu64,
        s->video_raw_receiver_banks_model?"true":"false",s->video_raw_receiver_bank,
        s->video_raw_receiver_wait_bank?"true":"false",s->video_raw_receiver_fresh,
        s->video_raw_receiver_refills,s->video_raw_receiver_latches,s->video_raw_reposted_packets);
    g_string_append_printf(out,",\"input_notification_queued\":%s",
        s->video_raw_receiver_notify_pending?"true":"false");
    g_string_append(out,",\"scope\":\"actual RAM provenance grows without a hardware FIFO limit; producer ACK cannot release; only explicitly posted decoder records are decoded; CPU RAM remains intact; original codec unimplemented\"}");
    return g_string_free(out,false);
}

static bool get_video_raw_receiver_banks_model(Object *obj, Error **errp)
{ return NIKON_Z9_LAB(obj)->video_raw_receiver_banks_model; }
static void set_video_raw_receiver_banks_model(Object *obj, bool value, Error **errp)
{
    NikonZ9State *s=NIKON_Z9_LAB(obj);
    if (s->image_ram.ram || s->image_ram.rom_device) {
        error_setg(errp,"RAW receiver banks immutable after realization");return;
    }
    s->video_raw_receiver_banks_model=value;
}
