/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Functional input acceptance, not recovered physical CSI timing. Permit one
 * bootstrap input per source activation; later input notifications require a
 * new completed preview RAM generation or an actual complete D980 transfer. */
static void raw_source_handshake_start(NikonZ9State *s, uint32_t units)
{
    if (!s->raw_source_handshake_model || !units) { return; }
    s->raw_source_handshake_units &= ~units;
    s->raw_source_handshake_starts += !!(units & 2) + !!(units & 0x10000);
}

static uint32_t raw_source_handshake_filter(NikonZ9State *s, uint32_t units,
                                           bool direct_frame)
{
    units &= 0x10002;
    if (!s->raw_source_handshake_model || !units) { return units; }
    if (direct_frame) {
        s->raw_source_handshake_direct++;
        s->raw_source_handshake_frame = s->raw_pipeline_frames;
        s->raw_source_handshake_units |= units;
        return units;
    }
    bool fresh = s->raw_pipeline_frames != s->raw_source_handshake_frame;
    uint32_t accepted = fresh ? units : units & ~s->raw_source_handshake_units;
    if (!accepted) { s->raw_source_handshake_blocked++; return 0; }
    if (fresh) { s->raw_source_handshake_preview++; }
    else { s->raw_source_handshake_bootstrap++; }
    s->raw_source_handshake_frame = s->raw_pipeline_frames;
    s->raw_source_handshake_units |= accepted;
    return accepted;
}

static bool get_raw_source_handshake_model(Object *obj, Error **errp)
{ return NIKON_Z9_LAB(obj)->raw_source_handshake_model; }

static void set_raw_source_handshake_model(Object *obj, bool value, Error **errp)
{
    NikonZ9State *s = NIKON_Z9_LAB(obj);
    if (s->image_ram.ram || s->image_ram.rom_device) {
        error_setg(errp, "raw-source-handshake-model is immutable after realization");
        return;
    }
    s->raw_source_handshake_model = value;
}

static char *get_raw_source_handshake_state(Object *obj, Error **errp)
{
    NikonZ9State *s = NIKON_Z9_LAB(obj);
    return g_strdup_printf("{\"enabled\":%s,\"initialized_units\":%u,"
        "\"preview_frame\":%"PRIu64",\"latest_preview_frame\":%"PRIu64","
        "\"source_starts\":%"PRIu64",\"bootstrap_inputs\":%"PRIu64","
        "\"preview_inputs\":%"PRIu64",\"direct_inputs\":%"PRIu64","
        "\"blocked_ticks\":%"PRIu64",\"physical_protocol_verified\":false,"
        "\"scope\":\"One bootstrap per explicit source START; later notifications follow actual preview/full RAW generations, masks and W1C unchanged\"}",
        s->raw_source_handshake_model ? "true" : "false",
        s->raw_source_handshake_units, s->raw_source_handshake_frame,
        s->raw_pipeline_frames, s->raw_source_handshake_starts,
        s->raw_source_handshake_bootstrap, s->raw_source_handshake_preview,
        s->raw_source_handshake_direct, s->raw_source_handshake_blocked);
}
