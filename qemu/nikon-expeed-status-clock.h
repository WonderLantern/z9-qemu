/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Explicit laboratory status sources on QEMU virtual timers. These retain
 * existing host-declared periods, mask gates and IRQ/W1C semantics. They are
 * not measured sensor protocols, DMA completions or firmware acknowledgements.
 * All streams are disabled until explicitly configured after realization. */
static void zr_primary_bank1_irq_update(NikonZ9State *s);

static void diagnostic_status_clock_schedule(NikonZ9State *s)
{
    int64_t next = INT64_MAX;
    for (unsigned i = 0; i < 5; i++) {
        if (s->diagnostic_status_period[i]) {
            next = MIN(next, s->diagnostic_status_next[i]);
        }
    }
    if (next == INT64_MAX) {
        timer_del(s->diagnostic_status_timer);
    } else {
        timer_mod(s->diagnostic_status_timer, next);
    }
}

static void diagnostic_status_clock_tick(void *opaque)
{
    NikonZ9State *s = opaque;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    for (unsigned i = 0; i < 5; i++) {
        if (!s->diagnostic_status_period[i] || now < s->diagnostic_status_next[i]) {
            continue;
        }
        /* Drop missed periods, never deliver a catch-up burst. The next
         * virtual deadline exists even while the host services other work. */
        s->diagnostic_status_next[i] = now + s->diagnostic_status_period[i];
        uint32_t bits = 0;
        if (i == 0 && s->event1508_aux_model) {
            bits = s->event_capture_config_1508[0][0x10c / 4] &
                   s->diagnostic_status_variant[i];
            if (bits) {
                s->event_aux0_pending |= bits;
                s->event_aux0_inputs++;
                event_aux0_update_irq(s);
            }
        } else if (i == 1 && s->event1508_aux1_model) {
            bits = s->event_capture_config_1508[1][0x10c / 4] & 1;
            if (bits) {
                s->event_aux1_pending |= bits;
                s->event_aux1_inputs++;
                event_aux1_update_irq(s);
            }
        } else if (i == 2 && s->event1508_global0_model &&
                   s->event1508_global0_bit26_model) {
            bool gate = !s->diagnostic_status_variant[i] ||
                ((s->event_control_1508[0] & 1) &&
                 (s->event_control_1508[3] & 1));
            bits = gate ? s->event_extended_1508[0x50 / 4] & 0x4000000 : 0;
        } else if (i == 3 && s->event1508_global0_model &&
                   s->event1508_global0_bit10_model) {
            bits = s->event_extended_1508[0x50 / 4] & 0x400;
        }
        if (i == 4 && s->zr_primary_bank1_model && s->zr_primary_bank1_irq &&
            (s->engine150c_active & 2) && !(s->engine150c_stop_requested & 2)) {
            /* Same explicit 0x60 source and atomic active/no-stop gate as
             * set_zr_primary_bank1_input. Masking and W1C remain independent.
             * This is status input, never pixels or a completion/ACK. */
            bits = s->diagnostic_status_variant[i];
            s->zr_primary_bank1_pending |= bits;
            s->zr_primary_bank1_inputs++;
            zr_primary_bank1_irq_update(s);
        }
        if (i >= 2 && i < 4 && bits) {
            s->event_global0_pending |= bits;
            s->event_global0_inputs++;
            event_global0_update_irq(s);
        }
        s->diagnostic_status_last_bits[i] = bits;
        if (bits) {
            s->diagnostic_status_pulses[i]++;
            s->diagnostic_status_bit0[i] += !!(bits & 1);
            s->diagnostic_status_bit1[i] += !!(bits & 2);
        }
    }
    diagnostic_status_clock_schedule(s);
}

static void set_diagnostic_status_clock(Object *obj, const char *value,
                                        Error **errp)
{
    NikonZ9State *s = NIKON_Z9_LAB(obj);
    uint64_t values[3] = {0};
    char **parts = value ? g_strsplit(value, ":", -1) : NULL;
    bool valid = parts && g_strv_length(parts) == 3;
    for (unsigned i = 0; valid && i < 3; i++) {
        valid = *parts[i] != 0;
        for (const char *p = parts[i]; valid && *p; p++) {
            valid = *p >= '0' && *p <= '9';
        }
        if (valid && qemu_strtou64(parts[i], NULL, 10, &values[i])) {
            valid = false;
        }
    }
    g_strfreev(parts);
    if (!valid || values[0] >= 5 || values[2] > UINT32_MAX ||
        (values[1] && (values[1] < 1000000 || values[1] > 1000000000))) {
        error_setg(errp, "diagnostic-status-clock requires index0..4:period_ns0_or_1ms..1s:variant");
        return;
    }
    unsigned i = values[0];
    uint64_t period = values[1];
    uint32_t variant = values[2];
    bool model = (i == 0 && s->event1508_aux_model && s->event_aux0_irq &&
                    variant >= 1 && variant <= 3) ||
        (i == 1 && s->event1508_aux1_model && s->event_aux1_irq && variant == 1) ||
        (i == 2 && s->event1508_global0_model && s->event_global0_irq &&
                    s->event1508_global0_bit26_model && variant <= 1) ||
        (i == 3 && s->event1508_global0_model && s->event_global0_irq &&
                    s->event1508_global0_bit10_model && variant == 1024) ||
        (i == 4 && s->zr_primary_bank1_model && s->zr_primary_bank1_irq &&
                    s->engine150c_stop_model && variant == 0x60);
    if (!s->bootstrap_fixtures || !model) {
        error_setg(errp, "diagnostic-status-clock requires realized matching models and a supported variant");
        return;
    }
    if (!s->diagnostic_status_timer) {
        s->diagnostic_status_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                                 diagnostic_status_clock_tick, s);
    }
    s->diagnostic_status_period[i] = period;
    s->diagnostic_status_variant[i] = variant;
    s->diagnostic_status_next[i] = period ?
        qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + period : 0;
    diagnostic_status_clock_schedule(s);
}

static char *get_diagnostic_status_clock(Object *obj, Error **errp)
{
    NikonZ9State *s = NIKON_Z9_LAB(obj);
    GString *out = g_string_new("{\"clock\":\"QEMU_CLOCK_VIRTUAL\",\"pixel_bytes\":0,"
        "\"physical_protocol_verified\":false,\"streams\":[");
    for (unsigned i = 0; i < 5; i++) {
        g_string_append_printf(out, "%s{\"index\":%u,\"period_ns\":%" PRIu64
            ",\"next_ns\":%" PRId64 ",\"variant\":%u,\"pulses\":%" PRIu64
            ",\"bit0_pulses\":%" PRIu64 ",\"bit1_pulses\":%" PRIu64
            ",\"last_bits\":%u}", i ? "," : "", i,
            s->diagnostic_status_period[i], s->diagnostic_status_next[i],
            s->diagnostic_status_variant[i], s->diagnostic_status_pulses[i],
            s->diagnostic_status_bit0[i], s->diagnostic_status_bit1[i],
            s->diagnostic_status_last_bits[i]);
    }
    g_string_append(out, "]}");
    return g_string_free(out, false);
}
