/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Functional programmed-channel waveform from the explicit source clock.
 * Native drivers register fifteen distinct channel callbacks and commit
 * independent configuration. This implements the observed 06011000/20 mode
 * class at one edge per explicit source period. The parameter units, waveform
 * phases and physical timing are not established. No image completion, DMA,
 * application notification or firmware address is selected here.
 * Channels zero and one retain their existing independent producers.
 */

static uint32_t programmed_clock_configured_mask(NikonZ9State *s)
{
    uint32_t mask = 0;
    for (unsigned ch = 2; ch < 15; ch++) {
        const uint32_t *c = s->event_active_1508[ch];
        /* Mode bit5 is retained as an opaque variant, not interpreted as an
         * enable. The original MMIO channel/group controls supply enable.
         * Only a fully committed, observed waveform class is admitted. */
        if ((c[7] & ~UINT32_C(0x20)) == 0x06011000 && c[0] && c[1] && c[4] && c[5]) {
            mask |= 1u << ch;
        }
    }
    return mask;
}

static uint32_t programmed_clock_armed_mask(NikonZ9State *s)
{
    if (!s->event_programmed_clock_model || !s->bootstrap_fixtures ||
        !s->event1508_channel_model || !s->event1508_timing_model ||
        !s->event_timing_active) {
        return 0;
    }
    return programmed_clock_configured_mask(s) & s->event_control_1508[3] &
           s->event_output_group_running;
}

static void programmed_clock_tick(NikonZ9State *s)
{
    uint32_t armed = programmed_clock_armed_mask(s);
    for (unsigned ch = 2; ch < 15; ch++) {
        uint32_t bit = 1u << ch;
        if (!(armed & bit)) {
            continue;
        }
        s->event_programmed_clock_ticks[ch]++;
        if (s->event_pending_1508 & bit) {
            s->event_programmed_clock_coalesced[ch]++;
        } else {
            s->event_pending_1508 |= bit;
            s->event_programmed_clock_events[ch]++;
        }
    }
    /* The caller updates the existing fifteen-slot IRQ after all clock
     * causes are ready. Masks only hide pending; its own W1C releases it. */
}

static bool get_event_programmed_clock_model(Object *obj, Error **errp)
{
    return NIKON_Z9_LAB(obj)->event_programmed_clock_model;
}

static void set_event_programmed_clock_model(Object *obj, bool value, Error **errp)
{
    NikonZ9State *s = NIKON_Z9_LAB(obj);
    if (s->image_ram.ram || s->image_ram.rom_device) {
        error_setg(errp, "programmed channel clock is immutable after realization");
        return;
    }
    s->event_programmed_clock_model = value;
}

static char *get_event_programmed_clock_state(Object *obj, Error **errp)
{
    NikonZ9State *s = NIKON_Z9_LAB(obj);
    GString *out = g_string_new(NULL);
    g_string_append_printf(out, "{\"enabled\":%s,\"configured_mask\":%u,"
        "\"armed_mask\":%u,\"channel_control\":%u,\"group_running\":%u,"
        "\"pending\":%u,\"irq_mask\":%u,\"source_active\":%s,"
        "\"source_period_ns\":%"PRIu64",\"lanes\":[",
        s->event_programmed_clock_model ? "true" : "false",
        programmed_clock_configured_mask(s), programmed_clock_armed_mask(s),
        s->event_control_1508[3], s->event_output_group_running,
        s->event_pending_1508, s->event_mask_1508,
        s->event_timing_active ? "true" : "false", s->event_timing_period_ns);
    for (unsigned ch = 0; ch < 15; ch++) {
        g_string_append_printf(out, "%s{\"channel\":%u,\"ticks\":%"PRIu64
            ",\"events\":%"PRIu64",\"coalesced\":%"PRIu64",\"latched\":[",
            ch ? "," : "", ch, s->event_programmed_clock_ticks[ch],
            s->event_programmed_clock_events[ch], s->event_programmed_clock_coalesced[ch]);
        for (unsigned word = 0; word < 8; word++) {
            g_string_append_printf(out, "%s%u", word ? "," : "", s->event_active_1508[ch][word]);
        }
        g_string_append(out, "]}");
    }
    g_string_append(out, "],\"pixel_bytes\":0,\"physical_timing_verified\":false,"
        "\"scope\":\"functional committed waveform class at explicit source period; "
        "channel/group enable, independent pending/W1C; waveform phases unverified\"}");
    return g_string_free(out, false);
}

/* Extended independent counter outputs. The observed 00011000/00011002
 * register classes on channels13/14 were retained but had no producer.
 * These sources are independent of RAM DMA and of the primary source gate.
 * The low H terminal and V terminal are treated inclusively at the explicit
 * source oscillator, as in the existing source-counter contract. Unequal H
 * fields and waveform phase/polarity remain physically unqualified; no
 * calibrated line/sensor timing or original hardware specification is claimed.
 */
static uint64_t programmed_extended_period(NikonZ9State *s, unsigned ch)
{
    if (!s->event_extended_counters_model || !s->event_source_counter_hz ||
        ch < 2 || ch >= 15) { return 0; }
    const uint32_t *c = s->event_active_1508[ch];
    unsigned h = c[0] & 0xffff;
    if (!h || !c[1] || c[1] > UINT16_MAX || !c[5] ||
        (c[2] && c[2] != UINT16_MAX) || c[6] ||
        (c[7] & ~UINT32_C(0x22)) != 0x00011000 ||
        c[4] > c[1] + 1 || c[5] > c[1] + 1) { return 0; }
    uint64_t ticks = (uint64_t)(h + 1) * (c[1] + 1);
    return (ticks * UINT64_C(1000000000) + s->event_source_counter_hz - 1) /
           s->event_source_counter_hz;
}

static void programmed_extended_update(NikonZ9State *s)
{
    if (!s->event_extended_timer) { return; }
    uint64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL), next = UINT64_MAX;
    uint32_t armed = 0;
    for (unsigned ch = 2; ch < 15; ch++) {
        uint32_t bit = 1u << ch;
        uint64_t period = programmed_extended_period(s, ch);
        if (period && (s->event_control_1508[3] & bit) &&
            (s->event_output_group_running & bit)) {
            armed |= bit;
            /* A repeated START or same-value commit is not a later deadline.
             * Changed committed geometry takes effect at the next cycle.
             * Actual STOP or enable clear cancels only the future clock. */
            if (!s->event_extended_deadline[ch]) {
                s->event_extended_deadline[ch] = now + period;
            }
            next = MIN(next, s->event_extended_deadline[ch]);
        } else { s->event_extended_deadline[ch] = 0; }
    }
    s->event_extended_armed = armed;
    if (next == UINT64_MAX) { timer_del(s->event_extended_timer); }
    else { timer_mod(s->event_extended_timer, next); }
}

static void programmed_extended_expire(void *opaque)
{
    NikonZ9State *s = opaque;
    uint64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    for (unsigned ch = 2; ch < 15; ch++) {
        uint32_t bit = 1u << ch;
        uint64_t deadline = s->event_extended_deadline[ch];
        uint64_t period = programmed_extended_period(s, ch);
        if (!period || !deadline || deadline > now ||
            !(s->event_control_1508[3] & bit) ||
            !(s->event_output_group_running & bit)) { continue; }
        s->event_extended_ticks[ch]++;
        s->event_extended_coalesced[ch] += !!(s->event_pending_1508 & bit);
        s->event_pending_1508 |= bit;
        s->event_extended_deadline[ch] = deadline + period;
    }
    programmed_extended_update(s);
    event_channel_update_irq(s);
}

static bool get_event_extended_counters(Object *obj, Error **errp)
{ return NIKON_Z9_LAB(obj)->event_extended_counters_model; }

static void set_event_extended_counters(Object *obj, bool value, Error **errp)
{
    NikonZ9State *s = NIKON_Z9_LAB(obj);
    if (s->event_extended_timer) {
        error_setg(errp, "extended source counters are immutable after realization");return;
    }
    s->event_extended_counters_model = value;
}

static char *get_event_extended_counter_state(Object *obj, Error **errp)
{
    NikonZ9State *s = NIKON_Z9_LAB(obj);
    GString *out = g_string_new(NULL);
    g_string_append_printf(out, "{\"enabled\":%s,\"armed_mask\":%u,\"pending\":%u,"
        "\"mask\":%u,\"oscillator_hz\":%"PRIu64",\"lanes\":[",
        s->event_extended_counters_model ? "true" : "false",s->event_extended_armed,
        s->event_pending_1508,s->event_mask_1508,s->event_source_counter_hz);
    for (unsigned ch = 2; ch < 15; ch++) {
        g_string_append_printf(out,"%s{\"channel\":%u,\"period_ns\":%"PRIu64
            ",\"deadline_ns\":%"PRIu64",\"ticks\":%"PRIu64",\"coalesced\":%"PRIu64"}",
            ch == 2 ? "" : ",",ch,programmed_extended_period(s,ch),
            s->event_extended_deadline[ch],s->event_extended_ticks[ch],s->event_extended_coalesced[ch]);
    }
    g_string_append(out,"] ,\"pixel_bytes\":0,\"physical_timing_verified\":false,"
        "\"scope\":\"explicit counter input class; independent clocks and own mask/W1C; "
        "waveform phases and unequal-H interpretation unqualified\"}");
    return g_string_free(out,false);
}
