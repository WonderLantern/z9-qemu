/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Included after NikonZ9State and dma_ram_range declaration. */
#define NJ_SCRATCH_BYTES 16384

static void producer_journal_read_error(NikonZ9State *s, unsigned reason,
    uint64_t pa, uint64_t bytes, uint64_t chunk, uint64_t length, bool has_result, int result)
{
    NJReadFailure *f = &s->producer_journal_read_failure;
    if (!f->recorded) {
        *f = (NJReadFailure){.recorded = true, .reason = reason,
            .operation_id = s->producer_journal.operation_id,
            .span_pa = pa, .span_bytes = bytes, .chunk_pa = chunk, .chunk_bytes = length,
            .memtx_result_present = has_result, .memtx_result = result,
            .time_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)};
    }
}

static bool producer_journal_digest(NikonZ9State *s, const uint8_t *bytes,
                                    size_t size, uint8_t digest[32])
{
    if (!s->producer_journal.enabled || !s->producer_journal_checksum) {
        return false;
    }
    g_checksum_reset(s->producer_journal_checksum);
    g_checksum_update(s->producer_journal_checksum, bytes, size);
    gsize n = 32;
    g_checksum_get_digest(s->producer_journal_checksum, digest, &n);
    return n == 32;
}

static bool producer_journal_readback(void *opaque, uint64_t pa, uint64_t bytes,
                                      uint8_t digest[32])
{
    NikonZ9State *s = opaque;
    if (!s->producer_journal.enabled) { return false; }
    if (!s->producer_journal_checksum || !s->producer_journal_scratch) {
        producer_journal_read_error(s, 1, pa, bytes, pa, 0, false, 0);
        return false;
    }
    if (!bytes || bytes > NJ_MAX_SPAN || !dma_ram_range(s, pa, bytes)) {
        producer_journal_read_error(s, 2, pa, bytes, pa, bytes, false, 0);
        return false;
    }
    g_checksum_reset(s->producer_journal_checksum);
    for (uint64_t at = 0; at < bytes;) {
        size_t n = MIN(bytes - at, NJ_SCRATCH_BYTES);
        if (!dma_ram_range(s, pa + at, n)) {
            producer_journal_read_error(s, 2, pa, bytes, pa + at, n, false, 0);
            return false;
        }
        MemTxResult result = address_space_read(&address_space_memory, pa + at,
                MEMTXATTRS_UNSPECIFIED, s->producer_journal_scratch, n);
        if (result != MEMTX_OK) {
            producer_journal_read_error(s, 3, pa, bytes, pa + at, n, true, result);
            return false;
        }
        g_checksum_update(s->producer_journal_checksum, s->producer_journal_scratch, n);
        at += n;
    }
    gsize n = 32;
    g_checksum_get_digest(s->producer_journal_checksum, digest, &n);
    return n == 32;
}

static NJParent producer_journal_parent(NikonZ9State *s, const NJOutput *source,
                                        const uint8_t *already_read, size_t bytes)
{
    uint8_t sha[32];
    if (!s->producer_journal.enabled) { return (NJParent){0}; }
    bool ok = source->nspans == 1 && source->spans[0].bytes == bytes &&
        producer_journal_digest(s, already_read, bytes, sha);
    return nj_parent(&s->producer_journal, source, sha, ok);
}

static NJOutput producer_journal_output(unsigned kind, unsigned format,
    uint64_t pa, uint64_t bytes, unsigned width, unsigned height,
    unsigned stride, uint64_t frame, uint64_t job, bool tagged)
{
    return (NJOutput){.kind = kind, .format = format, .width = width,
        .height = height, .stride = stride, .model_frame = frame, .model_job = job,
        .tagged = tagged, .nspans = 1, .spans = {{pa, bytes}}};
}

static void producer_journal_finish(NikonZ9State *s, NJToken *token, bool success)
{
    nj_finish(&s->producer_journal, token, success,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL), producer_journal_readback, s);
}

static bool get_producer_generation_journal(Object *obj, Error **errp)
{
    return NIKON_Z9_LAB(obj)->producer_journal.enabled;
}

static void set_producer_generation_journal(Object *obj, bool value, Error **errp)
{
    NikonZ9State *s = NIKON_Z9_LAB(obj);
    if (s->producer_journal_initialized || s->image_ram.ram || s->image_ram.rom_device) {
        error_setg(errp, "producer-generation-journal is immutable after realization");
        return;
    }
    s->producer_journal.enabled = value;
}

static char *get_producer_generation_journal_state(Object *obj, Error **errp)
{
    NikonZ9State *s = NIKON_Z9_LAB(obj);
    NJournal *j = &s->producer_journal;
    static const char *kinds[] = {"none", "d980_ram", "151_nv16", "151_raw", "1540_raw_copy"};
    static const char *formats[] = {"none", "packed12le", "packed14le", "NV16_Y_UV", "nikon-f0-lossless14"};
    static const char *states[] = {"no_monitored_overlap_since_completion",
        "overwritten_by_monitored_write", "reset_barrier", "coverage_unknown"};
    GString *o = g_string_new(NULL);
    g_string_append_printf(o, "{\"schema\":1,\"enabled\":%s,\"capacity\":%u,"
        "\"run_binding\":\"external_manifest_required\",\"gap\":%s,"
        "\"operation_id\":%" PRIu64 ",\"model_write_epoch\":%" PRIu64
        ",\"reset_epoch\":%" PRIu64 ",\"completed_operations\":%" PRIu64
        ",\"partial_failures\":%" PRIu64 ",\"readback_failures\":%" PRIu64
        ",\"parent_gaps\":%" PRIu64 ",\"overflows\":%" PRIu64 ","
        "\"native_owner_verified\":false,\"native_allocation_generation_verified\":false,"
        "\"cpu_and_other_dma_writes_tracked\":false,\"same_content_aba_excluded\":false,"
        "\"historical_retention_verified\":false,\"reset1540_tracked\":false,"
        "\"coverage\":\"D980 RAM,151 instance0 NV16/RAW,1540/153c packed or F0 RAW output destination writes only\","
        "\"scope\":\"Metadata only; hashes are RAM readback, not global RAM lifetime or native ownership."
        " QEMU/GLib unrecoverable OOM semantics retained.\",\"first_failure\":{"
        "\"code\":%u,\"kind\":%u,\"operation_id\":%" PRIu64
        ",\"pa\":%" PRIu64 ",\"bytes\":%" PRIu64 ",\"virtual_ns\":%" PRIu64 "}",
        j->enabled ? "true" : "false", NJ_CAPACITY, j->gap ? "true" : "false",
        j->operation_id, j->write_epoch, j->reset_epoch, j->completed_operations,
        j->partial_failures, j->hash_failures, j->parent_gaps, j->overflows,
        j->first_failure.code, j->first_failure.kind, j->first_failure.operation_id,
        j->first_failure.pa, j->first_failure.bytes, j->first_failure.time_ns);
    const NJReadFailure *f = &s->producer_journal_read_failure;
    g_string_append_printf(o, ",\"first_readback_failure\":{\"recorded\":%s,\"reason\":%u,"
        "\"operation_id\":%" PRIu64 ",\"span_pa\":%" PRIu64 ",\"span_bytes\":%" PRIu64
        ",\"actual_chunk_pa\":%" PRIu64 ",\"actual_chunk_bytes\":%" PRIu64
        ",\"memtx_result_present\":%s,\"memtx_result\":%" PRId64
        ",\"virtual_ns\":%" PRIu64 "},\"reset_counts\":[%" PRIu64 ",%" PRIu64
        ",%" PRIu64 ",%" PRIu64 "],\"records\":[",
        f->recorded ? "true" : "false", f->reason, f->operation_id, f->span_pa, f->span_bytes,
        f->chunk_pa, f->chunk_bytes, f->memtx_result_present ? "true" : "false", f->memtx_result,
        f->time_ns, j->reset_counts[1], j->reset_counts[2], j->reset_counts[3], j->reset_counts[4]);
    for (unsigned i = 0; i < j->count; i++) {
        const NJRecord *r = &j->records[i]; const NJOutput *p = &r->output;
        char parent_id[24], sensor_id[24];
        if (r->parent.record_id) {
            snprintf(parent_id, sizeof(parent_id), "%" PRIu64, r->parent.record_id);
        } else { strcpy(parent_id, "null"); }
        if (r->parent.sensor_record_id) {
            snprintf(sensor_id, sizeof(sensor_id), "%" PRIu64, r->parent.sensor_record_id);
        } else { strcpy(sensor_id, "null"); }
        g_string_append_printf(o, "%s{\"record_id\":%" PRIu64
            ",\"operation_id\":%" PRIu64 ",\"producer\":\"%s\",\"format\":\"%s\","
            "\"model_write_epoch\":%" PRIu64 ",\"reset_epoch\":%" PRIu64
            ",\"start_virtual_ns\":%" PRIu64 ",\"completed_virtual_ns\":%" PRIu64
            ",\"width\":%u,\"height\":%u,\"stride\":%u,\"model_frame\":%" PRIu64
            ",\"model_job\":%" PRIu64 ",\"state\":\"%s\",\"invalidated_operation\":%" PRIu64
            ",\"invalidated_virtual_ns\":%" PRIu64 ",\"parent_status\":%u,"
            "\"parent_record_id\":%s,\"sensor_record_id\":%s"
            ",\"sensor_model_frame\":%" PRIu64 ",\"sensor_tagged\":%s,\"spans\":[",
            i ? "," : "", r->record_id, r->operation_id, kinds[p->kind], formats[p->format],
            r->write_epoch, r->reset_epoch, r->start_ns, r->completed_ns,
            p->width, p->height, p->stride, p->model_frame, p->model_job, states[r->state],
            r->invalidated_operation, r->invalidated_ns, r->parent.status,
            parent_id, sensor_id, r->parent.sensor_model_frame,
            r->parent.sensor_tagged ? "true" : "false");
        for (unsigned k = 0; k < p->nspans; k++) {
            char hex[65];
            for (unsigned b = 0; b < 32; b++) { snprintf(hex + b * 2, 3, "%02x", r->sha256[k][b]); }
            g_string_append_printf(o, "%s{\"pa\":%" PRIu64 ",\"bytes\":%" PRIu64
                ",\"sha256\":\"%s\"}", k ? "," : "", p->spans[k].pa, p->spans[k].bytes, hex);
        }
        g_string_append(o, "],\"source_observation\":");
        if (r->parent.source.nspans == 1) {
            const NJOutput *q = &r->parent.source;
            char source_hash[67];
            if (r->parent.source_hash_observed) {
                source_hash[0] = '"';
                for (unsigned b = 0; b < 32; b++) {
                    snprintf(source_hash + 1 + b * 2, 3, "%02x", r->parent.source_sha256[b]);
                }
                source_hash[65] = '"'; source_hash[66] = 0;
            } else { strcpy(source_hash, "null"); }
            g_string_append_printf(o, "{\"kind\":%u,\"format\":%u,\"pa\":%" PRIu64
                ",\"bytes\":%" PRIu64 ",\"width\":%u,\"height\":%u,\"stride\":%u,"
                "\"model_frame\":%" PRIu64 ",\"model_job\":%" PRIu64 ",\"actual_read_sha256\":%s}",
                q->kind, q->format, q->spans[0].pa, q->spans[0].bytes,
                q->width, q->height, q->stride, q->model_frame, q->model_job, source_hash);
        } else { g_string_append(o, "null"); }
        g_string_append(o, "}");
    }
    g_string_append(o, "]}");
    return g_string_free(o, false);
}
