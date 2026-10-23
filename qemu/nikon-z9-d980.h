/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Functional external Bayer writer for the observed D980 mode2 profile.
 * Packed12 little-endian pairs and CFA-cell resampling are explicit lab
 * assumptions. Native format2/stride values alone do not prove the physical
 * wire packing, optical margins or actual sensor precision. */
static bool primary_d980_description(NikonZ9State *s, const uint32_t *c,
    unsigned slot, unsigned *width, unsigned *height, unsigned *stride,
    uint64_t *encoded, uint64_t *physical)
{
    *width = c[1] >> 16;
    *height = c[2] >> 16;
    *stride = c[4];
    unsigned bank = c[7];
    unsigned bits = c[0] == 1 && s->primary_d980_packed14_model ? 14 : 12;
    if (!s->raw_pixels || (c[0] != 2 && !(c[0] == 1 && s->primary_d980_packed14_model)) || (c[1] & 0xffff) || (c[2] & 0xffff) ||
        !*width || !*height || ((*width | *height) & 1) ||
        *width > NIKON_RAW_MAX_DIMENSION || *height > NIKON_RAW_MAX_DIMENSION ||
        (uint64_t)*width * *height > NIKON_RAW_MAX_SAMPLES ||
        *stride != ((((*width * bits + 7) / 8) + 31) & ~31u) ||
        (uint64_t)*stride * *height > 128 * MiB ||
        c[3] || c[5] != 1 || c[6] != 1 || c[8] != 1 || c[9] || c[10] || c[11] ||
        bank > 1 || slot > 3) {
        return false;
    }
    unsigned word = 12 + bank * 8 + slot * 2;
    *encoded = c[word] | ((uint64_t)c[word + 1] << 32);
    if (*encoded & 31) { return false; }
    return raw_pipeline_address(s, *encoded, (uint64_t)*stride * *height, physical);
}

static bool primary_d980_ready(NikonZ9State *s)
{
    unsigned w, h, pitch;
    uint64_t encoded, physical;
    const uint32_t *c = s->primary_d980_latched;
    if (!primary_d980_description(s, c, 0, &w, &h, &pitch, &encoded, &physical)) {
        return false;
    }
    unsigned bank = c[7];
    for (unsigned slot = 1; slot < 4; slot++) {
        unsigned word = 12 + bank * 8 + slot * 2;
        if ((c[word] || c[word + 1]) &&
            !primary_d980_description(s, c, slot, &w, &h, &pitch, &encoded, &physical)) {
            return false;
        }
    }
    return true;
}

static uint8_t *primary_d980_pack12(const NikonRawPixels *p,
                                  unsigned width, unsigned height, unsigned stride)
{
    uint8_t *pixels = g_malloc0((size_t)stride * height);
    for (unsigned y = 0; y < height; y++) {
        unsigned sy = ((uint64_t)(y / 2) * (p->height / 2) / (height / 2)) * 2 + (y & 1);
        uint8_t *row = pixels + (size_t)y * stride;
        for (unsigned x = 0; x < width; x += 2) {
            unsigned sx = ((uint64_t)(x / 2) * (p->width / 2) / (width / 2)) * 2;
            uint16_t a = MIN(4095, ((uint64_t)p->samples[(size_t)sy * p->width + sx] * 4095 + p->white / 2) / p->white);
            uint16_t b = MIN(4095, ((uint64_t)p->samples[(size_t)sy * p->width + sx + 1] * 4095 + p->white / 2) / p->white);
            unsigned off = x * 3 / 2;
            row[off] = a;
            row[off + 1] = (a >> 8) | ((b & 15) << 4);
            row[off + 2] = b >> 4;
        }
    }
    return pixels;
}

/* Little-endian contiguous14-bit groups are an explicit lab hypothesis.
 * The native mode1 and pitch alone do not establish physical wire packing. */
static uint8_t *primary_d980_pack14(const NikonRawPixels *p,
                                  unsigned width, unsigned height, unsigned stride)
{
    uint8_t *pixels = g_malloc0((size_t)stride * height);
    for (unsigned y = 0; y < height; y++) {
        unsigned sy = ((uint64_t)(y / 2) * (p->height / 2) / (height / 2)) * 2 + (y & 1);
        uint8_t *row = pixels + (size_t)y * stride;
        for (unsigned x = 0; x < width; x += 4) {
            unsigned count = MIN(4, width - x);
            uint64_t packed = 0;
            for (unsigned j = 0; j < count; j++) {
                unsigned sx = ((uint64_t)((x + j) / 2) * (p->width / 2) / (width / 2)) * 2 + ((x + j) & 1);
                uint16_t value = MIN(16383, ((uint64_t)p->samples[(size_t)sy * p->width + sx] * 16383 + p->white / 2) / p->white);
                packed |= (uint64_t)value << (j * 14);
            }
            for (unsigned byte = 0; byte < (count * 14 + 7) / 8; byte++) {
                row[x * 14 / 8 + byte] = packed >> (byte * 8);
            }
        }
    }
    return pixels;
}

/* Existing explicit full-frame input hypotheses: exactly once after
 * a successful complete DMA. FIFO backpressure/empty ticks cannot
 * notify; masks and original W1C retain completion ownership. */
static void primary_d980_input_trace(NikonZ9State *s, const char *action,
                                      uint32_t previous, uint32_t value);

static void primary_d980_complete_inputs(NikonZ9State *s, bool continuous_dma)
{
    /* C5.32: primary bank0 bit6 -> original041d0005 subscriber ->
     * native SFE completion10000 when its original active mask permits.
     * Only this successful complete Bayer DMA may produce the notification;
     * retain pending/mask/W1C in the existing IRQ device. This models a
     * functional data boundary, not measured physical CMOS timing. */
    if (s->primary_d980_frame_completion_model && !video_raw_descriptor(s)) {
        s->primary_d980_frame_completions++;
        s->primary_d980_frame_completed_epoch = s->primary_d980_input_epoch;
        s->engine150c_bit6_inputs++;
        s->engine150c_bit6_pending |= 0x40;
        engine150c_stop_irq_update(s);
        event_global0_finite_complete(s);
    }
    if (s->primary_d980_source_notify_model) {
        /* Notify the existing source wait only after the complete DMA.
         * Both original callbacks map group1 bit29 to wait400000;
         * no OS wait state or ISR acknowledgement is written here. */
        uint64_t boundaries = s->raw_source_boundaries;
        raw_source_boundary_receive(s, true);
        s->primary_d980_source_notifications += s->raw_source_boundaries != boundaries;
        /* Reuse the input-pair route already used by real streaming DMA.
         * This retained experimental pair does not establish SFE completion;
         * native785 still took SfeTo. Continuous SFE uses bit6/041d0005. A clock edge,
         * cancelled/held source or incomplete RAW cannot satisfy this pair.
         * Physical stage attribution remains unverified. */
        if (s->primary_d980_input_pair_model &&
            s->raw_source_boundaries != boundaries && (s->raw_source_pending & 0x20000000)) {
            s->raw_primary_pending |= 0x80000000;
            s->raw_pipeline_input_pair_completions++;
            s->primary_d980_input_pair_completions++;
            engine150c_stop_irq_update(s);
        }
        /* Same configured source boundary as the existing preview path,
         * only after this complete actual Bayer DMA; never on an empty,
         * blocked, cancelled or merely committed sensor request. */
        event_global1_pixel_boundary(s);
    }
    /* A continuous source-clock cycle is independent of this successful
     * RAM transfer. Do not report the same clock a second time at DMA end. */
    if (s->primary_d980_channel_boundary_model && continuous_dma) {
        s->event_raw_counter_dma_suppressed++;
    }
    if (s->primary_d980_channel_boundary_model && !continuous_dma) {
        /* The complete input frame precedes this existing channel clock
         * boundary. Shared channel pending/ACK semantics remain intact;
         * primary stop/reset does not acknowledge a channel on its behalf. */
        s->primary_d980_channel_boundaries++;
        s->primary_d980_channel_coalesced += !!(s->event_pending_1508 & 1);
        s->event_pending_1508 |= 1;
        s->event_channel_inputs++;
        event_channel_update_irq(s);
    }
    /* A committed FIFO input can finish on either the periodic or finite
     * timer. Native740 wrote every RAW byte but lost the finite bit6 cause
     * when the periodic timer won. Retire only an actually fulfilled epoch,
     * then report the same finite-frame event as the legacy successful path.
     * No source tick, empty queue or ACK can complete this ownership. */
    if (s->sensor_pool_model && !continuous_dma && primary_d980_finite_input_owned(s)) {
        /* A running FIFO can deliver an actual source-clock frame
         * without arming the legacy finite timer. It still fulfils this
         * START epoch. Continuous source-counter ownership is excluded by
         * the outer guard; an empty clock or ACK never reaches this path. */
        if (s->primary_d980_single_armed ||
            (s->primary_d980_input_epoch &&
             s->primary_d980_single_epoch != s->primary_d980_input_epoch)) {
            s->primary_d980_single_armed = false;
            s->primary_d980_single_epoch = s->primary_d980_input_epoch;
            s->primary_d980_single_frames++;
            if (s->primary_d980_single_timer) { timer_del(s->primary_d980_single_timer); }
            primary_d980_input_trace(s, "fifo-dma", 0, 0);
        }
        if (s->primary_reset_queued_start_model) { engine150c_bit6_pulse(s); }
    }
}

static bool primary_d980_receive(NikonZ9State *s, bool single_input)
{
    /* Each original half of the previous input pair retains ownership
     * until its own W1C. Do not overwrite a source frame or invent an ACK. */
    if (s->primary_d980_input_pair_model && !video_raw_descriptor(s) &&
        ((s->raw_primary_pending & 0x80000000) || (s->raw_source_pending & 0x20000000))) {
        s->primary_d980_input_pair_blocks++;
        return false;
    }
    /* A retained source boundary owns its counter slot until original
     * W1C. Keep the real request queued instead of emitting another
     * frame on an unconsumed cause. No pixels or ACK are fabricated. */
    if (s->event_source_frame_arbiter_model && !video_raw_descriptor(s) &&
        s->sensor_request_count && (s->event_pending_1508 & 3)) {
        s->event_source_dma_backpressure++;
        return false;
    }
    if (video_raw_descriptor(s)) {
        if (single_input) { return false; }
        if (s->video_secondary_parallel_model) {
            /* Both configured consumers see this source-clock input. Neither
             * admission retires or overwrites the other's pending output. */
            bool packet=video_raw_receive(s);
            bool sensor=video_secondary_sensor_receive(s);
            return packet || sensor;
        }
        return video_secondary_sensor_receive(s) || video_raw_receive(s);
    }
    if (!s->primary_d980_stream_model || (!single_input && !s->event_timing_active) ||
        s->primary_reset_stage || !(s->engine150c_active & 0x4000) ||
        (s->engine_150c_init[0x18 / 4] & 0x4000) ||
        (s->primary_d980_pending && !s->sensor_pool_model) ||
        (s->sensor_pool_model && !s->sensor_request_pending)) {
        return false;
    }
    if (s->sensor_pool_model && !sensor_request_mature(s)) { return false; }
    unsigned w, h, pitch, slot = s->primary_d980_slot;
    uint64_t encoded, physical;
    /* The native descriptor has four slots; only nonzero submitted slots
     * participate. Slot cycling is a functional model, not chip timing. */
    unsigned bank = s->primary_d980_latched[7];
    if (!s->primary_d980_latched[12 + bank * 8 + slot * 2] &&
        !s->primary_d980_latched[13 + bank * 8 + slot * 2]) {
        slot = 0;
    }
    if (!primary_d980_description(s, s->primary_d980_latched, slot,
                                 &w, &h, &pitch, &encoded, &physical)) {
        s->primary_d980_rejects++;
        unknown_access(s, 0x150d9800, s->primary_d980_latched[0], 4, true);
        return false;
    }
    /* This actual sensor admission may be a finite exposure while preview
     * timing is off. Refresh external pixels here, after all descriptor and
     * ownership guards. Re-entering at the same clock instant is idempotent;
     * protected RAM, original masks/ACKs and successful-write history remain. */
    Ex7SensorFrame *sensor_slot=s->sensor_pool_model ? sensor_pool_reserve(s,physical,(uint64_t)pitch*h) : NULL;
    if (s->sensor_pool_model && !sensor_slot) { return false; }
    temporal_raw_source_tick(s);
    if (!s->primary_d980_cache ||
        (s->raw_temporal_input_model && s->primary_d980_cache_input_frame!=s->raw_temporal_input_frames) ||
        w != s->primary_d980_cache_width ||
        h != s->primary_d980_cache_height || pitch != s->primary_d980_cache_stride || s->primary_d980_latched[0] != s->primary_d980_cache_format) {
        g_free(s->primary_d980_cache);
        s->primary_d980_cache = s->primary_d980_latched[0] == 1 ?
            primary_d980_pack14(s->raw_pixels, w, h, pitch) :
            primary_d980_pack12(s->raw_pixels, w, h, pitch);
        s->primary_d980_cache_input_frame = s->raw_temporal_input_frames;
        s->primary_d980_cache_width = w;
        s->primary_d980_cache_height = h;
        s->primary_d980_cache_stride = pitch;
        s->primary_d980_cache_format = s->primary_d980_latched[0];
    }
    size_t bytes = (size_t)pitch * h;
    if (address_space_write(&address_space_memory, physical, MEMTXATTRS_UNSPECIFIED,
                            s->primary_d980_cache, bytes) != MEMTX_OK) {
        s->primary_d980_rejects++;
        unknown_access(s, 0x150d9920, encoded, 4, true);
        return false;
    }
    /* Snapshot this completed request before sensor_request_done can load a
     * different-kind head. Admission type is independent of stale epochs. */
    bool continuous_dma = s->event_raw_counter_model && s->sensor_pool_model &&
        s->sensor_request_count && !s->sensor_requests[s->sensor_request_head].finite;
    s->primary_d980_frames++;
    s->primary_d980_bytes += bytes;
    if (sensor_slot) {
        sensor_pool_record(s,sensor_slot,physical,bytes,w,h,pitch,s->primary_d980_latched[0]);
        sensor_readout_complete(s,physical,bytes);
        sensor_request_done(s);
    }
    if (s->raw_temporal_input_model) {
        /* Append only after address_space_write succeeded. No source tick,
         * mask operation or blocked producer can manufacture a DMA receipt. */
        uint64_t *r = s->raw_temporal_dma_history[s->raw_temporal_dma_count++%128];
        r[0] = s->primary_d980_frames;
        r[1] = s->raw_temporal_input_frames;
        r[2] = s->raw_temporal_input_ns;
        r[3] = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        r[4] = physical;
        r[5] = bytes;
    }

    s->primary_d980_last_encoded = encoded;
    s->primary_d980_last_physical = physical;
    s->primary_d980_slot = s->sensor_pool_model ? 0 : (slot + 1) & 3;
    engine153_deferred_try(s);
    /* Z9 original40caf3ec and ZR40eb8e18 map logical4000 to
     * primary group1 bit26. Publish only after every output byte exists. */
    s->primary_d980_pending = 0x4000000;
    engine150c_stop_irq_update(s);
    if (s->sensor_pool_model) {
        primary_d980_complete_inputs(s, continuous_dma);
        sensor_request_handoff(s);
    }
    return true;
}

static void primary_d980_tick(NikonZ9State *s)
{
    if (s->sensor_pool_model || !primary_d980_finite_input_owned(s)) { primary_d980_receive(s, false); }
}

/* Admission diagnostics describe real device ownership, never a completion.
 * Native630 loses the third finite request before RAW production; retain the
 * small control transition sequence so reset and front ordering can be
 * compared without dumping image buffers or stopping the original CPU. */
static void primary_d980_input_trace(NikonZ9State *s, const char *action,
                                     uint32_t previous, uint32_t value)
{
    if (!s->primary_d980_single_model) { return; }
    fprintf(stderr, "Z9_D980_INPUT action=%s ns=%" PRId64 " pc=%" PRIx64
        " previous=%x value=%x prepared=%u armed=%u epoch=%" PRIu64
        " completed=%" PRIu64 " frames=%" PRIu64 " primary=%x reset=%u"
        " clock=%u live=%u control=%x front=%x\n", action,
        qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL),
        current_cpu ? ARM_CPU(current_cpu)->env.pc : 0, previous, value,
        s->primary_d980_prepared, s->primary_d980_single_armed,
        s->primary_d980_input_epoch, s->primary_d980_single_epoch,
        s->primary_d980_frames, s->engine150c_active, s->primary_reset_stage,
        s->event_timing_active, s->primary_d980_live_single_model,
        s->event_control_1508[3], s->event_mode_1508[0]);
}

static void primary_d980_single_cancel(NikonZ9State *s)
{
    if (s->primary_d980_prepared || s->primary_d980_single_armed) {
        primary_d980_input_trace(s, "cancel", 0, 0);
    }
    sensor_request_cancel(s);
    s->primary_capture_deferred = false;
    s->primary_d980_prepare_cancelled += s->primary_d980_prepared;
    s->primary_d980_prepared = false;
    s->primary_d980_single_cancelled += s->primary_d980_single_armed;
    s->primary_d980_single_armed = false;
    if (s->primary_d980_single_timer) { timer_del(s->primary_d980_single_timer); }
}

static void primary_d980_single_tick(void *opaque)
{
    NikonZ9State *s = opaque;
    if (!s->primary_d980_single_armed) { return; }
    if (s->primary_reset_stage || !(s->engine150c_active & 0x4000) ||
        (s->event_timing_active && !s->primary_d980_live_single_model) ||
        ++s->primary_d980_single_attempts > 64) {
        primary_d980_single_cancel(s);
        return;
    }
    /* Original460: periodic source stopped, primary4000 enabled, front
     * preparation2->3, then source bitmap200. One external frame and 33ms
     * input pacing are hypotheses, not physical exposure/readout timing.
     * A later mode0 may release preparation; only actual stop/reset cancels
     * this separate input. Never fabricate a front/status result. */
    /* A configured running source can supply the finite capture without
     * output lane9 (native700 control704b). The opt-in live owner still
     * suppresses free-running RAW/source/channel0 events until actual DMA.
     * Stopped-clock captures retain their original201 strobe requirement. */
    bool strobed = (s->event_control_1508[3] & 0x201) == 0x201;
    bool clocked = s->primary_d980_live_single_model && s->event_timing_active &&
                   (s->event_control_1508[3] & 1);
    /* The explicitly selected automatic external frame source is independent
     * of the preview output clock. Its timer is armed by an actual RAW START;
     * descriptor, hold, maturity, RAM owners and cancellation still apply. */
    bool external = s->primary_d980_live_single_model && s->sensor_pool_model &&
                    !s->sensor_manual_acquisition_model && s->sensor_request_pending;
    if ((strobed || clocked || external) && primary_d980_receive(s, true)) {
        if (!s->sensor_pool_model || s->primary_d980_single_armed) {
        s->primary_d980_single_armed = false;
        s->primary_d980_single_epoch = s->primary_d980_input_epoch;
        s->primary_d980_single_frames++;
        primary_d980_input_trace(s, "dma", 0, 0);
        if (!s->sensor_pool_model) {
            primary_d980_complete_inputs(s, false);
        }
        /* The finite input can finish with the preview clock stopped.
         * Complete every actual pixel/source/channel write first, then
         * latch only bit6's finite-frame notification. The original mask,
         * ISR/W1C and deferred drain still own its delivery/retirement.
         * Diagnostic bit2 remains an independent cycle-clock input. */
        if (s->primary_reset_queued_start_model) { engine150c_bit6_pulse(s); }
        }
        return;
    }
    timer_mod(s->primary_d980_single_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 33333333);
}

static void primary_d980_single_arm(NikonZ9State *s)
{
    s->primary_d980_single_armed = true;
    s->primary_d980_single_attempts = 0;
    s->primary_d980_single_requests++;
    primary_d980_input_trace(s, "arm", 0, 0);
    timer_mod(s->primary_d980_single_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 33333333);
}

static void primary_d980_single_mode(NikonZ9State *s, unsigned channel,
                                    uint32_t previous, uint32_t value)
{
    if (!channel && previous != value) {
        primary_d980_input_trace(s, "front", previous, value);
    }
    if (!channel && previous==2 && value==3 &&
        (s->event_timing_active || s->sensor_manual_acquisition_model) &&
        !sensor_pool_request(s)) { unknown_access(s,0x1508008c,value,4,true);return; }
    if (!channel && previous==2 && value==3) { sensor_manual_trigger(s); }
    /* The committed FIFO owns running-clock admission, including when the
     * legacy finite-input options coexist. Keep the stopped-clock timer. */
    if (s->sensor_pool_model && s->event_timing_active) { return; }
    if (!s->primary_d980_single_model || channel || previous != 2 || value != 3 ||
        s->primary_reset_stage || (s->event_timing_active && !s->primary_d980_live_single_model)) {
        return;
    }
    if (!(s->engine150c_active & 0x4000)) {
        if (s->primary_d980_prepared_start_model && s->primary_reset_count &&
            !s->primary_d980_prepared && !s->primary_d980_single_armed) {
            s->primary_d980_prepared = true;
            s->primary_d980_prepare_requests++;
        }
        return;
    }
    /* A stopped source may acquire several explicitly submitted frames in
     * one active-unit epoch. Only a new FIFO head with its own actual manual
     * edge may rearm that finite timer; a clock, ACK or empty edge cannot. */
    bool finite_request = s->sensor_pool_model && s->sensor_source_valid &&
        s->sensor_source_finite && sensor_manual_frame_owned(s);
    if (!s->primary_d980_input_epoch ||
        (s->primary_d980_single_epoch == s->primary_d980_input_epoch && !finite_request) ||
        s->primary_d980_single_armed) {
        return;
    }
    primary_d980_single_arm(s);
}

static bool get_primary_d980_input_pair(Object *obj, Error **errp)
{ return NIKON_Z9_LAB(obj)->primary_d980_input_pair_model; }

static void set_primary_d980_input_pair(Object *obj, bool value, Error **errp)
{
    NikonZ9State *s = NIKON_Z9_LAB(obj);
    if (s->image_ram.ram || s->image_ram.rom_device) {
        error_setg(errp, "full RAW input pair is immutable after realization");return;
    }
    s->primary_d980_input_pair_model = value;
}

static char *get_primary_d980_input_pair_state(Object *obj, Error **errp)
{
    NikonZ9State *s = NIKON_Z9_LAB(obj);
    return g_strdup_printf("{\"enabled\":%s,\"completions\":%"PRIu64
        ",\"backpressure_checks\":%"PRIu64",\"pending\":%u,"
        "\"source_pending\":%u,\"physical_stage_verified\":false}",
        s->primary_d980_input_pair_model ? "true" : "false",
        s->primary_d980_input_pair_completions, s->primary_d980_input_pair_blocks,
        s->raw_primary_pending & 0x80000000, s->raw_source_pending & 0x20000000);
}

static bool get_primary_d980_frame_completion(Object *obj, Error **errp)
{ return NIKON_Z9_LAB(obj)->primary_d980_frame_completion_model; }
static void set_primary_d980_frame_completion(Object *obj, bool value, Error **errp)
{
    NikonZ9State *s = NIKON_Z9_LAB(obj);
    if (s->image_ram.ram || s->image_ram.rom_device) { error_setg(errp, "full RAW completion is immutable after realization");return; }
    s->primary_d980_frame_completion_model = value;
}
static char *get_primary_d980_frame_completion_state(Object *obj, Error **errp)
{
    NikonZ9State *s = NIKON_Z9_LAB(obj);
    return g_strdup_printf("{\"enabled\":%s,\"completions\":%"PRIu64",\"completed_input_epoch\":%"PRIu64",\"pending\":%u,\"physical_stage_verified\":false}",
        s->primary_d980_frame_completion_model ? "true" : "false",
        s->primary_d980_frame_completions, s->primary_d980_frame_completed_epoch, s->engine150c_bit6_pending);
}
