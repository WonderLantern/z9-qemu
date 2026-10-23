/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Functional streaming RAW -> IMPRSZ output exploration. Run234 provides
 * A0/B0..2/F0..2 and bank0 descriptors. 40c2590c registers INTID6a.
 * Resampling, NV12/NV16 selection, ring pacing and frame bit0 are explicit
 * model assumptions; this does not emulate physical EXPEED processing. */
static void raw_pipeline_irq_update(NikonZ9State *s)
{
    uint32_t pending = 0;
    if (!s->engine1542_irq || !(s->raw_pipeline_model || s->video_preview_model || s->engine1543_stop_ack_model)) { return; }
    for (unsigned i = 0; i < 15; i++) {
        unsigned off = i < 10 ? 0x18 : i < 13 ? 0x10 : i == 13 ? 0x14 : 0x50;
        pending |= s->engine1542_pending[i] & s->engine1542_config[i][off / 4];
    }
    qemu_set_irq(s->engine1542_irq, pending != 0);
}

static bool raw_pipeline_address(NikonZ9State *s, uint64_t encoded,
                                 uint64_t bytes, uint64_t *physical)
{
    /* Native B register versus C ring pairs differ by0x400000000:
     * E -> A and C ->8. Already physical8/A addresses are retained. */
    if (encoded >= UINT64_C(0xc00000000) && encoded < UINT64_C(0x1000000000)) {
        encoded -= UINT64_C(0x400000000);
    }
    *physical = encoded;
    return dma_ram_range(s, encoded, bytes);
}

/* Input timing is independent of IMPRSZ destination setup. Original ZR
 * 412aa7a0 maps primary subgroup1 bit29 to source wait400000; Z9 uses the
 * same cause at410b6ae8. No OS state is written by the device. This source
 * model produces no guest DMA, resize completion or saved RAW file. */
static void raw_source_boundary_receive(NikonZ9State *s, bool single_input)
{
    if (!s->raw_source_boundary_model || !s->raw_pixels ||
        (!single_input && (!s->event_timing_active || primary_d980_finite_input_owned(s))) ||
        s->primary_reset_stage) {
        return;
    }
    uint32_t active = s->engine150c_active &
        ~(s->engine150c_stop_requested | s->engine150c_stop_done |
          s->engine_150c_init[0x18 / 4]);
    active = raw_source_handshake_filter(s, active, single_input);
    uint32_t causes = ((active & 2) ? 0x20000000 : 0) |
                      ((active & 0x10000) ? 0x40000000 : 0);
    if (!causes) {
        return;
    }
    s->raw_source_boundaries++;
    s->raw_source_notifications += !!(causes & ~s->raw_source_pending);
    s->raw_source_pending |= causes;
    engine150c_stop_irq_update(s);
}

static void raw_source_boundary_tick(NikonZ9State *s)
{
    /* The selected full RAW FIFO owns this source boundary. An empty
     * periodic placeholder cannot preempt every actual queued DMA. */
    if (s->primary_d980_input_pair_model && (s->engine150c_active & 0x4000) &&
        !video_raw_descriptor(s)) { return; }
    raw_source_boundary_receive(s, false);
}

static bool get_raw_source_boundary_model(Object *obj, Error **errp)
{
    return NIKON_Z9_LAB(obj)->raw_source_boundary_model;
}

static void set_raw_source_boundary_model(Object *obj, bool value, Error **errp)
{
    NikonZ9State *s = NIKON_Z9_LAB(obj);
    if (s->image_ram.ram || s->image_ram.rom_device) {
        error_setg(errp, "raw-source-boundary-model is immutable after realization");
        return;
    }
    s->raw_source_boundary_model = value;
}

static char *get_raw_source_boundary_state(Object *obj, Error **errp)
{
    NikonZ9State *s = NIKON_Z9_LAB(obj);
    return g_strdup_printf("{\"enabled\":%s,\"pending\":%u,\"boundaries\":%" PRIu64
        ",\"notifications\":%" PRIu64 ",\"acks\":%" PRIu64 ",\"cancelled\":%" PRIu64
        ",\"pixel_dma_bytes\":0,\"physical_timing_verified\":false}",
        s->raw_source_boundary_model ? "true" : "false", s->raw_source_pending,
        s->raw_source_boundaries, s->raw_source_notifications, s->raw_source_acks,
        s->raw_source_cancelled);
}

/* Functional histogram bins, not the original ISP statistic algorithm.
 * The original reader exposes256 24-bit words for Y, or three such arrays.
 * This approximation bins the selected completed output's active pixels;
 * padding is excluded. Source index0..2 and Y-vs-RGB selection are declared
 * functional routing, not verified physical EXPEED register semantics.
 * No raw-source screenshot or LCD destination participates in this path. */
static unsigned raw_histogram_clip(int value)
{
    return value < 0 ? 0 : value > 255 ? 255 : value;
}

static void raw_histogram_complete(NikonZ9State *s, unsigned source)
{
    unsigned w=s->raw_pipeline_width[source], h=s->raw_pipeline_height[source];
    unsigned stride=s->raw_pipeline_stride[source];
    bool half=s->raw_pipeline_uv_half[source];
    const uint8_t *y=s->raw_pipeline_cache[source], *uv=y+(size_t)stride*h;
    bool luma=!!(s->engine1542_config[14][8/4]&1);
    memset(s->raw_histogram,0,sizeof(s->raw_histogram));
    for (unsigned row=0;row<h;row++) {
        for (unsigned col=0;col<w;col++) {
            unsigned v=y[(size_t)row*stride+col];
            if (luma) {
                uint32_t *bin=&s->raw_histogram[0][v];
                if (*bin<0xffffff) { (*bin)++; }
            } else {
                size_t index=(size_t)(row/(half?2:1))*stride+(col&~1u);
                int u=uv[index]-128, cr=uv[index+1]-128;
                unsigned rgb[3]={raw_histogram_clip((int)v+(359*cr)/256),
                    raw_histogram_clip((int)v-(88*u+183*cr)/256),
                    raw_histogram_clip((int)v+(454*u)/256)};
                for (unsigned ch=0;ch<3;ch++) {
                    uint32_t *bin=&s->raw_histogram[ch][rgb[ch]];
                    if (*bin<0xffffff) { (*bin)++; }
                }
            }
        }
    }
    s->raw_histogram_source=source;s->raw_histogram_luma=luma;
    s->raw_histogram_samples=(uint64_t)w*h;s->raw_histogram_frames++;
}

#include "nikon-expeed-main-output.h"
#include "nikon-z9-source-ram.h"


/* Read-only bounded gate history, retained across source STOP. */
static void raw_admission_observe(NikonZ9State *s, bool primary, bool secondary,
    bool sensor_secondary, const NikonRawPixels *pixels)
{
    bool video=primary||secondary, internal=video&&s->video_raw_transport_model;
    unsigned isp=primary?0:1, source=primary?0:2, input=video?1:0;
    unsigned selected=s->engine1542_config[internal?11:10][2]&1;
    unsigned initial=internal?selected:0;
    uint32_t mask=0;
#define RAW_GATE(bit, condition) do { if (condition) { mask|=1U<<(bit); } } while (0)
    RAW_GATE(0,internal && s->video_primary_boundaries_model && s->video_primary_boundaries_pending);
    RAW_GATE(1,internal && !(sensor_secondary?s->video_secondary_sensor_ready:s->video_raw_frame_ready));
    RAW_GATE(2,!pixels);
    RAW_GATE(3,internal && !s->video_raw_ring_latched &&
        (s->engine1543_buffer_valid[0]&UINT64_C(0xfff0fff0fff0fff0))!=UINT64_C(0xfff0fff0fff0fff0));
    RAW_GATE(4,internal && !s->video_raw_ring_latched && video_main_armed(s) && s->video_main_valid[initial]!=0xffff);
    RAW_GATE(5,video && s->video_receiver_before_supplier_model && !(s->engine1542_config[1][1]&0x200));
    RAW_GATE(6,s->primary_stopped_source_reset_model && s->primary_reset_stage);
    RAW_GATE(7,s->primary_stopped_source_reset_model && !(s->engine150c_active&2));
    RAW_GATE(8,s->primary_stopped_source_reset_model && (s->engine_150c_init[0x18/4]&2));
    RAW_GATE(9,!internal && (!s->engine1543_active[0]||s->engine1543_held[0]));
    RAW_GATE(10,!s->engine151_active[isp]);
    RAW_GATE(11,!s->engine1518[source].config[0x30/4]);
    RAW_GATE(12,video && (!s->engine1543_active[1]||s->engine1543_held[1]));
    RAW_GATE(13,s->engine1542_pending[input]);
    RAW_GATE(14,s->engine1542_pending[10]);
    RAW_GATE(15,s->raw_primary_pending);
    RAW_GATE(16,s->engine1542_pending[14]&1);
    RAW_GATE(17,video && s->engine1542_pending[11]);
    RAW_GATE(18,s->raw_pipeline_1514_completion_model && (s->engine151_pending[isp]&8));
    RAW_GATE(19,s->raw_pipeline_151c_completion_model && (s->engine1518[source].pending&0x08100000));
    RAW_GATE(20,s->video_source_end_model && source==0 && (s->engine1518[0].pending&0x20000000));
    RAW_GATE(21,sensor_secondary && s->video_secondary_source_end_model && (s->engine1518[2].pending&0x20000000));
    RAW_GATE(22,!event_global1_secondary_owns_start(s) && s->event1508_global1_model && (s->event_global1_pending&0x10));
    RAW_GATE(23,s->primary_stopped_source_reset_model && !s->event_timing_active);
    RAW_GATE(24,internal && (s->video_yuv_output_events_model || video_main_armed(s)) && s->raw_pipeline_wait_bank &&
        ((s->raw_pipeline_bank_written[selected]&UINT64_C(0xfff0fff0fff0fff0))!=UINT64_C(0xfff0fff0fff0fff0) ||
         (video_main_armed(s) && s->video_main_written[selected]!=0xffff)));
#undef RAW_GATE
    s->raw_admission_ticks++;
    for (unsigned b=0;b<32;b++) { if (mask&(1U<<b)) { s->raw_admission_counts[b]++; } }
    unsigned route=primary?1:secondary?2:0;
    unsigned previous=(s->raw_admission_changes-1)%128;
    if (s->raw_admission_changes && s->raw_admission_history[previous].blocked==mask &&
        s->raw_admission_history[previous].route==route &&
        s->raw_admission_history[previous].frames==s->raw_pipeline_frames &&
        s->raw_admission_history[previous].epoch==s->video_raw_decoded_epoch) { return; }
    unsigned n=s->raw_admission_changes++%128;
    s->raw_admission_history[n].time_ns=qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    s->raw_admission_history[n].blocked=mask;s->raw_admission_history[n].route=route;
    s->raw_admission_history[n].frames=s->raw_pipeline_frames;
    s->raw_admission_history[n].epoch=s->video_raw_decoded_epoch;
    s->raw_admission_history[n].row=s->raw_pipeline_row;
}

static void raw_admission_json(NikonZ9State *s,GString *out)
{
    static const char *names[]={"primary_boundary","decoded_input","pixels","preview_words","main_words",
        "supplier","reset_stage","source_inactive","source_hold","photo_header","isp_inactive",
        "source_gate","video_header","input_pending","output_pending","primary_pending","histogram_pending",
        "video_header_pending","isp_pending","source_pending","source_end","secondary_end","global_pending",
        "source_clock","fresh_bank"};
    g_string_truncate(out,out->len-1);
    g_string_append_printf(out,",\"admission\":{\"ticks\":%"PRIu64",\"changes\":%"PRIu64",\"gates\":[",
        s->raw_admission_ticks,s->raw_admission_changes);
    for (unsigned b=0;b<G_N_ELEMENTS(names);b++) {
        g_string_append_printf(out,"%s{\"bit\":%u,\"name\":\"%s\",\"ticks\":%"PRIu64"}",
            b?",":"",b,names[b],s->raw_admission_counts[b]);
    }
    g_string_append(out,"],\"history\":[");
    uint64_t start=s->raw_admission_changes>128?s->raw_admission_changes-128:0;
    for (uint64_t i=start;i<s->raw_admission_changes;i++) {
        unsigned n=i%128;
        g_string_append_printf(out,"%s{\"ns\":%"PRIu64",\"mask\":%u,\"route\":%u,"
            "\"frames\":%"PRIu64",\"epoch\":%"PRIu64",\"row\":%u}",i==start?"":",",
            s->raw_admission_history[n].time_ns,s->raw_admission_history[n].blocked,
            s->raw_admission_history[n].route,s->raw_admission_history[n].frames,
            s->raw_admission_history[n].epoch,s->raw_admission_history[n].row);
    }
    g_string_append(out,"]}}");
}

static void raw_pipeline_tick(void *opaque)
{
    NikonZ9State *s = opaque;
    unsigned row = s->raw_pipeline_row;
    bool primary_video = s->engine151_video_stream_model && s->engine151_active[0] &&
        s->engine151_latched[0][0x200/4] == 1;
    bool secondary_video = !primary_video && video_secondary_isp_ready(s);
    bool video = primary_video || secondary_video;
    unsigned isp = primary_video ? 0 : 1, source = primary_video ? 0 : 2, input = video ? 1 : 0;
    uint64_t ys[3], uvs[3], total = 0;
    unsigned widths[3], heights[3], strides[3];
    bool halves[3];
    if (!s->raw_pipeline_model) { return; }
    timer_mod(s->raw_pipeline_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 33333333);
    bool internal = video && s->video_raw_transport_model;
    bool sensor_secondary=secondary_video && video_secondary_sensor_route(s);
    bool yuv_events=internal && s->video_yuv_output_events_model;
    const NikonRawPixels *pixel_source = sensor_secondary ? s->video_secondary_sensor_pixels :
        internal ? s->video_raw_pixels : s->raw_pixels;
    if (internal) { video_primary_input_boundary_tick(s,sensor_secondary); }
    raw_admission_observe(s,primary_video,secondary_video,sensor_secondary,pixel_source);
    if (internal && s->video_primary_boundaries_model && s->video_primary_boundaries_pending) { return; }
    if (internal) {
        if (!(sensor_secondary ? s->video_secondary_sensor_ready : s->video_raw_frame_ready) || !pixel_source) { return; }
        if (!s->video_raw_ring_latched) {
            /* Native video C1 headers own the stream; C0 posts its global
             * output slots without a separate C0 source-start transaction. */
            /* PHOTO leaves valid header words in this retained register
             * table. Only the six paired video addresses in every row own
             * output spans; extra header validity cannot invalidate them.
             * Subsequent refills still require a whole fresh generation. */
            const uint64_t addresses=UINT64_C(0xfff0fff0fff0fff0);
            if ((s->engine1543_buffer_valid[0] & addresses) != addresses) { return; }
            unsigned main_bank=s->engine1542_config[11][2]&1;
            if (video_main_armed(s) && s->video_main_valid[main_bank]!=0xffff) { return; }
            memcpy(s->engine1543_latched[0],s->engine1542_config[10],sizeof(s->engine1543_latched[0]));
            s->raw_pipeline_bank=s->engine1542_config[11][2]&1;
            s->raw_pipeline_row=0;row=0;
            s->raw_pipeline_wait_bank=false;s->raw_pipeline_refill_notified=false;
            s->video_raw_ring_latched=true;s->video_raw_ring_latches++;
            s->raw_pipeline_bank_written[s->raw_pipeline_bank]=0;
            s->video_main_written[s->raw_pipeline_bank]=0;
        }
    }

    /* Receiver START is valid before its already configured A1 producer
     * is enabled. It grants no output transfer: keep an actual decoded or
     * owned sensor input until the original supplier enable is submitted. */
    if (video && s->video_receiver_before_supplier_model &&
        !(s->engine1542_config[1][4/4]&0x200)) { return; }

    /* Stopped-source reset profile: input clock and primary source unit
     * own new pixel work. Holding/resetting primary cancels future work,
     * without acknowledging a completed downstream frame on its behalf. */
    if (s->primary_stopped_source_reset_model &&
        (s->primary_reset_stage || !(s->engine150c_active & 2) ||
         (s->engine_150c_init[0x18 / 4] & 2))) {
        return;
    }
    /* Wait for the original parameter and source-gate submissions. Holding
     * a pending frame prevents overwriting buffers before the ISR accepts it. */
    if (!pixel_source || (!internal && (!s->engine1543_active[0] || s->engine1543_held[0])) ||
        !s->engine151_active[isp] || !s->engine1518[source].config[0x30 / 4] ||
        (video && (!s->engine1543_active[1] || s->engine1543_held[1])) ||
        s->engine1542_pending[input] || s->engine1542_pending[10] || s->raw_primary_pending ||
        (s->engine1542_pending[14] & 1) ||
        (video && s->engine1542_pending[11]) ||
        (s->raw_pipeline_1514_completion_model && (s->engine151_pending[isp] & 8)) ||
        (s->raw_pipeline_151c_completion_model && (s->engine1518[source].pending & 0x08100000)) ||
        (s->video_source_end_model && source==0 && (s->engine1518[0].pending&0x20000000)) ||
        (sensor_secondary && s->video_secondary_source_end_model && (s->engine1518[2].pending&0x20000000)) ||
        (!event_global1_secondary_owns_start(s) && s->event1508_global1_model && (s->event_global1_pending & 0x10))) { return; }
    /* Request the following batch separately, before its final frame.
     * The original callback wakes the frame task before storing its refill
     * byte. Combining bits0/3 can race that task on another CPU. Giving the
     * refill its own IRQ before the next frame preserves the original
     * software ordering without touching task flags or firmware code. */
    if (row == 3 && !s->raw_pipeline_refill_notified) {
        s->raw_pipeline_refill_notified = true;
        s->engine1542_pending[internal ? 11 : 10] |= 8;
        if (yuv_events) { s->engine1542_pending[10]|=8;s->video_yuv_output_refills++; }
        raw_pipeline_irq_update(s);
        return;
    }
    /* A watermark for completed buffers may outlive the input trigger.
     * Native ZR293/295 stop that trigger at row3. Requiring another input
     * clock before requesting its next descriptor bank stranded the owner.
     * New DMA remains strictly gated by the original source control. */
    if (s->primary_stopped_source_reset_model && !s->event_timing_active) {
        return;
    }
    /* Video C1 owns the bank selector while its four output planes are
     * programmed into that bank of C0. They are separate MMIO controllers:
     * consulting the retained photo C0 selector silently skips video bank1.
     * Native bank selection precedes posting the entire paired address table.
     * Keep the current batch until all words for the selected bank arrive.
     * Bank ownership is independent of the optional C0 output IRQ.
     * Four-frame refill pacing remains functional, not measured chip timing. */
    const uint32_t *submitted = s->engine1542_config[10];
    unsigned bank = s->raw_pipeline_bank;
    if (row == 0 && s->raw_pipeline_wait_bank) {
        unsigned selected = s->engine1542_config[internal?11:10][2] & 1;
        const uint64_t required = internal ? UINT64_C(0xfff0fff0fff0fff0) : UINT64_MAX;
        /* Video posts six preview pairs plus the optional main pair per row.
         * C1 selects the next C0 address bank; a retained C0 selector is not
         * video ownership. Require the whole new address generation, even
         * when the selected bank is unchanged. Partial pairs never replace it. */
        if ((internal || selected != bank) &&
            (s->raw_pipeline_bank_written[selected] & required) == required &&
            (!internal || !video_main_armed(s) ||
             s->video_main_written[selected]==0xffff)) {
            memcpy(&s->engine1543_latched[0][(0x200 + selected * 0x200) / 4],
                   &submitted[(0x200 + selected * 0x200) / 4], 0x200);
            s->raw_pipeline_bank_written[internal ? selected : bank] = 0;
            s->video_main_written[selected]=0;
            if (internal) { s->video_raw_ring_latches++; }
            bank = s->raw_pipeline_bank = selected;
            s->raw_pipeline_wait_bank = false;
            if (yuv_events) { s->video_yuv_output_banks++; }
        } else {
            if (yuv_events || (internal && video_main_armed(s))) { s->video_yuv_output_waits++; return; }
            /* Continuous capture retains the current complete table until
             * its replacement is available. 260/262's artificial stall
             * also stopped the combined input boundaries and deadlocked
             * the stream. Count reuse explicitly; do not consume partial
             * new descriptors or invent an LCD destination. */
            s->raw_pipeline_repeated_batches++;
        }
    }
    const uint32_t *ring = s->engine1543_latched[0];
    for (unsigned ch = 0; ch < 3; ch++) {
        const uint32_t *b = s->engine1542_config[3 + ch];
        const uint32_t *f = s->engine1542_config[15 + ch];
        unsigned off = (0x210 + bank * 0x200 + row * 0x80 + ch * 0x10) / 4;
        uint64_t y = ring[off] | (uint64_t)ring[off + 1] << 32;
        uint64_t uv = ring[off + 2] | (uint64_t)ring[off + 3] << 32;
        unsigned w = f[0x2c / 4], h = f[0x30 / 4], stride = b[0x38 / 4];
        bool half = !(b[0xc / 4] & 0x10);
        uint64_t ybytes = (uint64_t)stride * h, uvbytes = ybytes / (half ? 2 : 1);
        if (!w || !h || ((w | h | stride) & 1) || w > 4096 || h > 4096 ||
            stride < w || stride > 8192 || stride != b[0x3c / 4] || ybytes > 16 * MiB ||
            b[0x10 / 4] != 1 || !raw_pipeline_address(s, y, ybytes, &ys[ch]) ||
            !raw_pipeline_address(s, uv, uvbytes, &uvs[ch]) ||
            !(ys[ch] + ybytes <= uvs[ch] || uvs[ch] + uvbytes <= ys[ch])) {
            s->raw_pipeline_rejects++;
            return;
        }
        widths[ch] = w; heights[ch] = h; strides[ch] = stride; halves[ch] = half;
    }
    unsigned histogram_source=(s->engine1542_config[14][4/4]>>4)&15;
    if ((s->engine1542_config[14][0]&1) && histogram_source>=3) {
        s->raw_pipeline_rejects++;return;
    }
    WlMainPlan main_plan={0};
    /* A whole video frame preflights all six output spans together. An
     * overlapping/bad later plane must not corrupt an earlier destination. */
    if (video) {
        uint64_t p[6], n[6];
        for (unsigned ch = 0; ch < 3; ch++) {
            p[2*ch] = ys[ch]; p[2*ch+1] = uvs[ch];
            n[2*ch] = (uint64_t)strides[ch]*heights[ch];
            n[2*ch+1] = n[2*ch]/(halves[ch] ? 2 : 1);
        }
        if (video_main_prepare(s,pixel_source,sensor_secondary ? s->video_secondary_sensor_epoch : s->video_raw_decoded_epoch,
                ring,bank,row,p,n,&main_plan,internal)<0) {
            s->raw_pipeline_rejects++;return;
        }
        for (unsigned j = 0; j < 6; j++) {
            for (unsigned k = 0; k < j; k++) {
                if (p[j] < p[k]+n[k] && p[k] < p[j]+n[j]) {
                    s->raw_pipeline_rejects++;return;
                }
            }
        }
    }
    for (unsigned ch = 0; ch < 3; ch++) {
        unsigned w = widths[ch], h = heights[ch], stride = strides[ch];
        bool half = halves[ch];
        size_t ybytes = (size_t)stride * h, uvbytes = ybytes / (half ? 2 : 1);
        uint64_t input_frame = !s->raw_temporal_input_model ? 0 :
            pixel_source==s->raw_pixels ? s->raw_temporal_input_frames :
            pixel_source==s->video_secondary_sensor_pixels ? s->video_secondary_sensor_input_frame : 0;
        if (!s->raw_pipeline_cache[ch] || s->raw_pipeline_cache_input_frame[ch]!=input_frame ||
            s->raw_pipeline_cache_internal[ch]!=internal || s->raw_pipeline_width[ch] != w ||
            s->raw_pipeline_height[ch] != h || s->raw_pipeline_stride[ch] != stride ||
            s->raw_pipeline_uv_half[ch] != half) {
            g_free(s->raw_pipeline_cache[ch]);
            s->raw_pipeline_cache[ch] = nikon_raw_pixels_yuv(pixel_source, w, h, stride, half);
            s->raw_pipeline_cache_internal[ch]=internal;
            s->raw_pipeline_cache_input_frame[ch]=input_frame;
            s->raw_pipeline_width[ch] = w; s->raw_pipeline_height[ch] = h;
            s->raw_pipeline_stride[ch] = stride; s->raw_pipeline_uv_half[ch] = half;
        }
        if (!s->raw_pipeline_cache[ch] ||
            address_space_write(&address_space_memory, ys[ch], MEMTXATTRS_UNSPECIFIED,
                                s->raw_pipeline_cache[ch], ybytes) != MEMTX_OK ||
            address_space_write(&address_space_memory, uvs[ch], MEMTXATTRS_UNSPECIFIED,
                                s->raw_pipeline_cache[ch] + ybytes, uvbytes) != MEMTX_OK) {
            s->raw_pipeline_rejects++;
            return;
        }
        s->raw_pipeline_y[ch] = ys[ch]; s->raw_pipeline_uv[ch] = uvs[ch];
        total += ybytes + uvbytes;
    }
    if (!video_main_commit(s,&main_plan)) { s->raw_pipeline_rejects++;return; }
    if (main_plan.active) { total+=main_plan.surface.stored[0]+main_plan.surface.stored[1]; }
    if (internal && primary_video) {
        video_source_ram_frame(s, pixel_source, s->video_raw_decoded_epoch);
    }
    /* The enabled inline15430100 stage belongs to this actual image job.
     * Native original event41160002 bit0 releases the graph's auxiliary
     * image wait. Retaining its registers alone left the downstream table
     * refill waiting forever while the other frame counters kept advancing.
     * It now supplies actual Y/RGB histogram bins from a completed output;
     * original processing and silicon timing are unproved. Completion follows all
     * six successful plane writes above, never a configuration/clock alone.
     * Masking retains pending; its own W1C is required before another frame.
     * No firmware version, CPU address, task flag or LCD target is consulted.
     */
    if (s->engine1542_config[14][0] & 1) {
        raw_histogram_complete(s,histogram_source);
        s->engine1542_pending[14] |= 1;
    }
    /* Native ZR40e0db5c -> 40eac290 dispatches event410b0002.
     * Registered consumer4107eaa8 tests bit3 and wakes the stream worker;
     * bit1 only sets a generic driver flag and is ignored by that consumer.
     * Bit3 follows all actual RAW-derived plane writes. W1C owns reuse and
     * continuous mode retains its request. Physical line timing unverified. */
    if (s->raw_pipeline_1514_completion_model) {
        s->engine151_pending[isp] |= 8;
        s->raw_pipeline_1514_completions += !video;
        engine151_update_irq(s, isp);
    }
    /* Functional combined-ISP completion after all three real Y/UV
     * transfers. Original ZR40e0ae38 W1C,40e9c8a8 and4107ae48 deliver
     * these bits to the native graph. No guest references, task flags or
     * callbacks are written. Both causes must be acknowledged before reuse.
     * Disabled masks retain status; reset cancels ownership normally.
     * Physical latency and exact silicon processing remain unverified. */
    if (s->raw_pipeline_151c_completion_model) {
        Z9Engine1518 *e = &s->engine1518[source];
        e->pending |= 0x08100000;
        e->stream_frames++;
        /* Original source1 worker compares its data counter3 and then
         * waits for800000. Publish source2 bit29 only after these three
         * completed owned sensor inputs and all six actual Y/UV writes.
         * Never modify its wait flags, callbacks or acknowledge on behalf. */
        if (sensor_secondary && s->video_secondary_source_end_model) {
            s->video_secondary_source_group++;
            if (s->video_secondary_source_group==3) {
                e->pending|=0x20000000;s->video_secondary_source_ends++;
            }
        }
        /* Only completed, decoded internal RAW transfers advance the group.
         * Source0 bit29 is forwarded by the original client to its ordinary
         * frame-end wait; masks retain it and native W1C alone retires it. */
        if (internal && video && source==0 && s->video_source_end_model) {
            s->video_source_group_transfers++;
            if (s->video_source_group_transfers==3) {
                e->pending|=0x20000000;s->video_source_group_ends++;
            }
        }
        engine1518_update_irq(e);
    }
    /* Complete the configured primary/secondary input pair only
     * after all actual RAW-derived Y/UV transfers succeed. The original
     * group1 ISR/W1C and consumer own the source callbacks and references.
     * Pending is retained while masked and prevents reuse until ACK.
     * The exact physical stage behind bit31 remains unverified. */
    if (s->raw_pipeline_input_pair_model) {
        s->raw_primary_pending |= 0x80000000;
        s->raw_pipeline_input_pair_completions++;
        engine150c_stop_irq_update(s);
    }
    if (internal && s->video_primary_boundaries_model) {
        s->video_primary_boundaries_pending |= s->video_primary_input_boundary_model ? 0x10 : 0x18;
        s->video_primary_boundaries_frames++;
        engine150c_stop_irq_update(s);
    }
    if (sensor_secondary) {
        s->video_secondary_sensor_consumed=s->video_secondary_sensor_epoch;s->video_secondary_sensor_ready=false;
    } else if (internal) {
        s->video_raw_consumed_epoch=s->video_raw_decoded_epoch;s->video_raw_frame_ready=false;
    }
    s->raw_pipeline_frames++;
    s->raw_pipeline_video_frames += video;
    s->video_secondary_isp_frames += secondary_video;
    s->raw_pipeline_bytes += total;
    s->raw_pipeline_row = (row + 1) & 3;
    /* The original40e94934 callback consumes A0 bits0/4 as input boundary
     * notifications, distinct from C0 output bit0. This combined functional
     * ISP completes both sides after the RAW-derived writes. It does not
     * claim physical line timing. Disabled A0 produces no source event. */
    if (s->engine1542_config[input][0x10 / 4] == 1) {
        s->engine1542_pending[input] |= 0x11;
    }
    if (video) { s->engine1542_pending[11] |= 1; }
    /* C1 reports RAW supplier ownership. C0 reports the three actual
     * output plane transfers above. Neither event ACKs the other; the
     * existing pending guard prevents overwriting output until native W1C. */
    if (!internal || yuv_events) { s->engine1542_pending[10] |= 1; }
    if (yuv_events) { s->video_yuv_output_frames++; }
    if (row == 3) {
        s->raw_pipeline_refill_notified = false;
        s->raw_pipeline_wait_bank = true;
        s->raw_pipeline_batches++;
    }
    raw_pipeline_irq_update(s);
    /* Native255 CAPTG waits in410b6c50 for the second input's boundary.
     * The original ISR supplies +210 in payload+4; PPC410b677c maps bit30
     * to flag800000, and410b6ae8 maps bit29 to flag400000. Complete these
     * two active upper paths after real output writes in this combined ISP.
     * This is a functional frame boundary, not measured physical ISP timing. */
    uint32_t primary = ((s->engine150c_active & 2) ? 0x20000000 : 0) |
                       ((s->engine150c_active & 0x10000) ? 0x40000000 : 0);
    primary &= s->engine_150c_init[0x20c / 4];
    if (primary && !s->raw_source_boundary_model) {
        s->raw_primary_pending |= primary;
        s->raw_primary_events++;
        engine150c_stop_irq_update(s);
    }
    /* Native4115a914..a974 consumes global1 bit4, dispatches the firmware's
     * current sequence event and sets its stream boundary flag. The older
     * timing-only model supplied bit7 alone. Here bit4 follows real pixels. */
    event_global1_pixel_boundary(s);
    if (s->raw_pipeline_frames <= 8) {
        fprintf(stderr, "Z9_RAW_PIPELINE frame=%" PRIu64 " row=%u bytes=%" PRIu64
            " Y=%" PRIx64 ":%" PRIx64 ":%" PRIx64 " bank=%u pending=%x; functional Bayer output\n",
            s->raw_pipeline_frames, row, total, ys[0], ys[1], ys[2], bank, s->engine1542_pending[10]);
    }
}

static bool get_raw_pipeline_model(Object *obj, Error **errp)
{
    return NIKON_Z9_LAB(obj)->raw_pipeline_model;
}

static void set_raw_pipeline_model(Object *obj, bool value, Error **errp)
{
    NikonZ9State *s = NIKON_Z9_LAB(obj);
    if (s->engine1542_irq) {
        error_setg(errp, "raw-pipeline-model is immutable after realization");
        return;
    }
    s->raw_pipeline_model = value;
}

static char *get_raw_pipeline_state(Object *obj, Error **errp)
{
    NikonZ9State *s = NIKON_Z9_LAB(obj);
    GString *out = g_string_new(NULL);
    g_string_append_printf(out, "{\"enabled\":%s,\"frames\":%" PRIu64
        ",\"bytes\":%" PRIu64 ",\"rejects\":%" PRIu64 ",\"next_row\":%u,"
        "\"primary_pending\":%u,\"primary_events\":%" PRIu64 ",\"primary_acks\":%" PRIu64 ","
        "\"bank\":%u,\"wait_bank\":%s,\"batches\":%" PRIu64 ",\"repeated_batches\":%" PRIu64 ","
        "\"input_sha256\":\"%s\",\"camera_acceptance\":false,\"outputs\":[",
        s->raw_pipeline_model ? "true" : "false", s->raw_pipeline_frames,
        s->raw_pipeline_bytes, s->raw_pipeline_rejects, s->raw_pipeline_row,
        s->raw_primary_pending, s->raw_primary_events, s->raw_primary_acks,
        s->raw_pipeline_bank, s->raw_pipeline_wait_bank ? "true" : "false", s->raw_pipeline_batches,
        s->raw_pipeline_repeated_batches,
        s->statistics_raw_sha256 ? s->statistics_raw_sha256 : "");
    for (unsigned ch = 0; ch < 3; ch++) {
        g_string_append_printf(out, "%s{\"width\":%u,\"height\":%u,\"stride\":%u,"
            "\"y\":%" PRIu64 ",\"uv\":%" PRIu64 ",\"format\":\"%s\"}",
            ch ? "," : "", s->raw_pipeline_width[ch], s->raw_pipeline_height[ch],
            s->raw_pipeline_stride[ch], s->raw_pipeline_y[ch], s->raw_pipeline_uv[ch],
            s->raw_pipeline_uv_half[ch] ? "NV12" : "NV16");
    }
    g_string_append_printf(out,"],\"video_stream_enabled\":%s,\"video_frames\":%" PRIu64 ",\"video_isp_acks\":%" PRIu64 ",\"video_source_acks\":%" PRIu64 ",\"video_isp_pending\":%u,\"video_source_pending\":%u,\"scope\":\"functional streamed Bayer, simplified ISP, checked RAM outputs and original IRQ ownership; physical sensor protocol and movie encoding unproved\"}",
        s->engine151_video_stream_model ? "true" : "false",s->raw_pipeline_video_frames,
        s->raw_pipeline_video_acks,s->engine1518[0].stream_acks,
        s->engine151_pending[0],s->engine1518[0].pending);
    g_string_truncate(out,out->len-1);
    g_string_append_printf(out,",\"histogram_frames\":%"PRIu64
        ",\"histogram_samples\":%"PRIu64",\"histogram_source\":%u,\"histogram_luma\":%s}",
        s->raw_histogram_frames,s->raw_histogram_samples,s->raw_histogram_source,
        s->raw_histogram_luma?"true":"false");
    raw_admission_json(s,out);
    return g_string_free(out, false);
}

static bool get_raw_pipeline_1514_completion_model(Object *obj, Error **errp)
{
    return NIKON_Z9_LAB(obj)->raw_pipeline_1514_completion_model;
}

static void set_raw_pipeline_1514_completion_model(Object *obj, bool value, Error **errp)
{
    NikonZ9State *s = NIKON_Z9_LAB(obj);
    if (s->engine1542_irq) {
        error_setg(errp, "raw-pipeline-1514-completion-model is immutable after realization");
        return;
    }
    s->raw_pipeline_1514_completion_model = value;
}

static char *get_raw_pipeline_1514_completion_state(Object *obj, Error **errp)
{
    NikonZ9State *s = NIKON_Z9_LAB(obj);
    return g_strdup_printf("{\"enabled\":%s,\"completions\":%" PRIu64
        ",\"acks\":%" PRIu64 ",\"pending\":%u,\"camera_acceptance\":false,"
        "\"scope\":\"optional streaming ISP completion after real pixel DMA; physical timing unverified\"}",
        s->raw_pipeline_1514_completion_model ? "true" : "false",
        s->raw_pipeline_1514_completions, s->raw_pipeline_1514_acks,
        s->engine151_pending[1] & 8);
}

static bool get_raw_pipeline_151c_completion_model(Object *obj, Error **errp)
{
    return NIKON_Z9_LAB(obj)->raw_pipeline_151c_completion_model;
}

static void set_raw_pipeline_151c_completion_model(Object *obj, bool value, Error **errp)
{
    NikonZ9State *s = NIKON_Z9_LAB(obj);
    if (s->image_ram.ram || s->image_ram.rom_device) {
        error_setg(errp, "raw-pipeline-151c-completion-model is immutable after realization");
        return;
    }
    s->raw_pipeline_151c_completion_model = value;
}

static char *get_raw_pipeline_151c_completion_state(Object *obj, Error **errp)
{
    NikonZ9State *s = NIKON_Z9_LAB(obj);
    Z9Engine1518 *e = &s->engine1518[2];
    return g_strdup_printf("{\"enabled\":%s,\"completions\":%" PRIu64
        ",\"acks\":%" PRIu64 ",\"cancelled\":%" PRIu64 ",\"pending\":%u,"
        "\"scope\":\"functional combined ISP source boundary after actual pixel DMA\","
        "\"physical_timing_verified\":false}",
        s->raw_pipeline_151c_completion_model ? "true" : "false",
        e->stream_frames, e->stream_acks, e->stream_cancelled,
        e->pending & 0x08100000);
}

static bool get_raw_pipeline_input_pair_model(Object *obj, Error **errp)
{
    return NIKON_Z9_LAB(obj)->raw_pipeline_input_pair_model;
}

static void set_raw_pipeline_input_pair_model(Object *obj, bool value, Error **errp)
{
    NikonZ9State *s = NIKON_Z9_LAB(obj);
    if (s->image_ram.ram || s->image_ram.rom_device) {
        error_setg(errp, "raw-pipeline-input-pair-model is immutable after realization");
        return;
    }
    s->raw_pipeline_input_pair_model = value;
}

static char *get_raw_pipeline_input_pair_state(Object *obj, Error **errp)
{
    NikonZ9State *s = NIKON_Z9_LAB(obj);
    return g_strdup_printf("{\"enabled\":%s,\"completions\":%" PRIu64
        ",\"acks\":%" PRIu64 ",\"cancelled\":%" PRIu64 ",\"pending\":%u,"
        "\"scope\":\"functional source input pair after actual pixel DMA\","
        "\"physical_timing_verified\":false}",
        s->raw_pipeline_input_pair_model ? "true" : "false",
        s->raw_pipeline_input_pair_completions, s->raw_pipeline_input_pair_acks,
        s->raw_pipeline_input_pair_cancelled, s->raw_primary_pending & 0x80000000);
}
