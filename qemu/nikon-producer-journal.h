/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Metadata only. Monitored writes are not native allocation ownership. */
#ifndef NIKON_PRODUCER_JOURNAL_H
#define NIKON_PRODUCER_JOURNAL_H
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define NJ_CAPACITY 64
#define NJ_OUTPUTS 2
#define NJ_SPANS 2
#define NJ_MAX_SPAN (UINT64_C(128) << 20)
#define NJ_MAX_OPERATION (UINT64_C(256) << 20)
enum { NJ_D980 = 1, NJ_NV16, NJ_RAW151, NJ_RAW1540 };
enum { NJ_PACKED12 = 1, NJ_PACKED14, NJ_NV16_PLANES, NJ_F0_LOSSLESS14 };
enum { NJ_NO_MONITORED_OVERLAP, NJ_OVERWRITTEN, NJ_RESET_BARRIER,
       NJ_COVERAGE_UNKNOWN };
enum { NJ_PARENT_ROOT, NJ_PARENT_MATCH, NJ_PARENT_MISSING,
       NJ_PARENT_HASH_MISMATCH, NJ_PARENT_AMBIGUOUS, NJ_PARENT_HASH_FAILED };
enum { NJ_FAIL_NONE, NJ_FAIL_BOUNDS, NJ_FAIL_COUNTER, NJ_FAIL_CAPACITY,
       NJ_FAIL_PRODUCER, NJ_FAIL_READBACK, NJ_FAIL_PARENT, NJ_FAIL_SEQUENCE };
typedef struct NJSpan { uint64_t pa, bytes; } NJSpan;
typedef struct NJOutput {
    unsigned kind, format, width, height, stride, nspans;
    uint64_t model_frame, model_job;
    bool tagged;
    NJSpan spans[NJ_SPANS];
} NJOutput;
typedef struct NJParent {
    unsigned status;
    uint64_t record_id, operation_id, sensor_record_id, sensor_model_frame;
    bool sensor_tagged;
    bool source_hash_observed;
    NJOutput source;
    uint8_t source_sha256[32];
} NJParent;
typedef struct NJRecord {
    uint64_t record_id, operation_id, write_epoch, reset_epoch;
    uint64_t start_ns, completed_ns, invalidated_ns, invalidated_operation;
    unsigned state;
    NJOutput output;
    NJParent parent;
    uint8_t sha256[NJ_SPANS][32];
} NJRecord;
typedef struct NJFailure {
    unsigned code, kind;
    uint64_t operation_id, pa, bytes, time_ns;
} NJFailure;
typedef struct NJReadFailure {
    bool recorded, memtx_result_present;
    unsigned reason;
    uint64_t operation_id, span_pa, span_bytes, chunk_pa, chunk_bytes, time_ns;
    int64_t memtx_result;
} NJReadFailure;
typedef struct NJournal {
    bool enabled, gap;
    unsigned count;
    uint64_t operation_id, record_id, write_epoch, reset_epoch;
    uint64_t completed_operations, partial_failures, hash_failures;
    uint64_t parent_gaps, overflows, reset_counts[5];
    NJFailure first_failure;
    NJRecord records[NJ_CAPACITY];
} NJournal;
typedef struct NJToken {
    bool enabled, publishable;
    unsigned noutputs;
    uint64_t operation_id, write_epoch, reset_epoch, start_ns;
    NJOutput outputs[NJ_OUTPUTS];
    NJParent parent;
} NJToken;
typedef bool (*NJHash)(void *, uint64_t, uint64_t, uint8_t [32]);

static inline void nj_count(uint64_t *n)
{
    if (*n != UINT64_MAX) { ++*n; }
}
static inline void nj_fail(NJournal *j, unsigned code, unsigned kind,
                           uint64_t op, NJSpan span, uint64_t ns)
{
    j->gap = true;
    if (!j->first_failure.code) {
        j->first_failure = (NJFailure){code, kind, op, span.pa, span.bytes, ns};
    }
}
static inline bool nj_span_known(NJSpan a)
{
    return a.bytes && a.pa <= UINT64_MAX - a.bytes;
}
static inline void nj_unknown(NJournal *j, uint64_t ns)
{
    for (unsigned i = 0; i < j->count; i++) {
        if (j->records[i].state == NJ_NO_MONITORED_OVERLAP) {
            j->records[i].state = NJ_COVERAGE_UNKNOWN;
            j->records[i].invalidated_ns = ns;
        }
    }
}
static inline void nj_overlap(NJournal *j, NJSpan a, uint64_t op, uint64_t ns)
{
    for (unsigned i = 0; i < j->count; i++) {
        NJRecord *r = &j->records[i];
        for (unsigned k = 0; k < r->output.nspans; k++) {
            NJSpan b = r->output.spans[k];
            if (r->state == NJ_NO_MONITORED_OVERLAP &&
                a.pa < b.pa + b.bytes && b.pa < a.pa + a.bytes) {
                r->state = NJ_OVERWRITTEN;
                r->invalidated_operation = op;
                r->invalidated_ns = ns;
            }
        }
    }
}
/* Resolve from already-read source bytes BEFORE destination invalidation. */
static inline NJParent nj_parent(NJournal *j, const NJOutput *source,
                                 const uint8_t sha[32], bool hash_ok)
{
    NJParent p = {.status = NJ_PARENT_MISSING};
    if (!j->enabled) { return p; }
    p.source = *source;
    if (!hash_ok) { p.status = NJ_PARENT_HASH_FAILED; return p; }
    unsigned matches = 0;
    for (unsigned i = 0; i < j->count; i++) {
        NJRecord *r = &j->records[i];
        const NJOutput *o = &r->output;
        if (r->state != NJ_NO_MONITORED_OVERLAP || o->nspans != 1 ||
            source->nspans != 1 || o->kind != source->kind ||
            o->format != source->format || o->width != source->width ||
            o->height != source->height || o->stride != source->stride ||
            o->model_frame != source->model_frame || o->model_job != source->model_job ||
            o->spans[0].pa != source->spans[0].pa ||
            o->spans[0].bytes != source->spans[0].bytes) { continue; }
        ++matches;
        if (memcmp(r->sha256[0], sha, 32)) {
            p.status = NJ_PARENT_HASH_MISMATCH;
        } else {
            p = (NJParent){.status = NJ_PARENT_MATCH, .record_id = r->record_id,
                .operation_id = r->operation_id,
                .sensor_record_id = r->parent.sensor_record_id,
                .sensor_model_frame = r->parent.sensor_model_frame,
                .sensor_tagged = r->parent.sensor_tagged};
        }
    }
    if (matches > 1) { p = (NJParent){.status = NJ_PARENT_AMBIGUOUS}; }
    if (p.status != NJ_PARENT_MATCH) {
        p.record_id = p.operation_id = p.sensor_record_id = p.sensor_model_frame = 0;
        p.sensor_tagged = false;
    }
    p.source = *source;
    p.source_hash_observed = true;
    memcpy(p.source_sha256, sha, 32);
    return p;
}
static inline void nj_begin(NJournal *j, NJToken *t, const NJOutput *out,
                            unsigned n, NJParent parent, uint64_t ns)
{
    memset(t, 0, sizeof(*t));
    if (!j->enabled) { return; }
    t->enabled = true;
    t->start_ns = ns;
    t->reset_epoch = j->reset_epoch;
    t->parent = parent;
    bool good = true;
    if (j->operation_id == UINT64_MAX || j->write_epoch == UINT64_MAX) {
        nj_fail(j, NJ_FAIL_COUNTER, 0, 0, (NJSpan){0}, ns);
        good = false;
    } else {
        t->operation_id = ++j->operation_id;
        t->write_epoch = ++j->write_epoch;
    }
    if (!out || !n || n > NJ_OUTPUTS) {
        nj_unknown(j, ns);
        nj_fail(j, NJ_FAIL_BOUNDS, 0, t->operation_id, (NJSpan){0}, ns);
        return;
    }
    t->noutputs = n;
    memcpy(t->outputs, out, n * sizeof(*out));
    uint64_t total = 0;
    for (unsigned i = 0; i < n; i++) {
        const NJOutput *o = &out[i];
        if (!o->nspans || o->nspans > NJ_SPANS) {
            nj_unknown(j, ns);
            nj_fail(j, NJ_FAIL_BOUNDS, o->kind, t->operation_id, (NJSpan){0}, ns);
            good = false; continue;
        }
        for (unsigned k = 0; k < o->nspans; k++) {
            NJSpan s = o->spans[k];
            if (!nj_span_known(s)) {
                nj_unknown(j, ns); good = false;
                nj_fail(j, NJ_FAIL_BOUNDS, o->kind, t->operation_id, s, ns);
                continue;
            }
            /* Even an over-limit/overflow operation destroys known overlaps. */
            nj_overlap(j, s, t->operation_id, ns);
            if (s.bytes > NJ_MAX_SPAN || total > NJ_MAX_OPERATION - s.bytes) {
                nj_fail(j, NJ_FAIL_BOUNDS, o->kind, t->operation_id, s, ns);
                good = false;
            } else { total += s.bytes; }
        }
        bool nv16 = o->format == NJ_NV16_PLANES;
        bool compressed = o->format == NJ_F0_LOSSLESS14;
        uint64_t minimum_row = nv16 ? o->width :
            ((uint64_t)o->width * (o->format == NJ_PACKED12 ? 12 : 14) + 7) / 8;
        uint64_t expected_bytes = (uint64_t)o->stride * o->height;
        bool shape = o->width && o->height && o->stride >= minimum_row;
        for (unsigned k = 0; k < o->nspans; k++) {
            shape &= compressed ? o->spans[k].bytes <= expected_bytes :
                                  o->spans[k].bytes == expected_bytes;
        }
        if (!shape ||
            o->kind < NJ_D980 || o->kind > NJ_RAW1540 ||
            o->format < NJ_PACKED12 || o->format > NJ_F0_LOSSLESS14 ||
            (nv16 ? o->nspans != 2 : o->nspans != 1) ||
            ((o->kind == NJ_NV16) != nv16) ||
            (o->kind == NJ_RAW151 && o->format != NJ_PACKED14) ||
            (o->kind == NJ_RAW1540 && o->format != NJ_PACKED14 && !compressed) ||
            (compressed && o->kind != NJ_RAW1540)) {
            nj_fail(j, NJ_FAIL_BOUNDS, o->kind, t->operation_id, o->spans[0], ns);
            good = false;
        }
    }
    if (n > NJ_CAPACITY - j->count || j->record_id > UINT64_MAX - n) {
        nj_count(&j->overflows);
        nj_fail(j, NJ_FAIL_CAPACITY, out[0].kind, t->operation_id, out[0].spans[0], ns);
        good = false;
    }
    if (out[0].kind != NJ_D980 && parent.status != NJ_PARENT_MATCH) {
        nj_count(&j->parent_gaps);
        nj_fail(j, NJ_FAIL_PARENT, out[0].kind, t->operation_id, out[0].spans[0], ns);
    }
    t->publishable = good;
}
static inline void nj_finish(NJournal *j, NJToken *t, bool success, uint64_t ns,
                             NJHash readback, void *opaque)
{
    if (!t->enabled) { return; }
    t->enabled = false;
    if (!success) {
        nj_count(&j->partial_failures);
        nj_fail(j, NJ_FAIL_PRODUCER, t->noutputs ? t->outputs[0].kind : 0,
                t->operation_id, t->noutputs ? t->outputs[0].spans[0] : (NJSpan){0}, ns);
        return;
    }
    if (!t->publishable) { return; }
    if (t->reset_epoch != j->reset_epoch || t->write_epoch != j->write_epoch ||
        ns < t->start_ns || t->noutputs > NJ_CAPACITY - j->count ||
        j->record_id > UINT64_MAX - t->noutputs) {
        nj_fail(j, NJ_FAIL_SEQUENCE, t->outputs[0].kind, t->operation_id,
                t->outputs[0].spans[0], ns);
        return;
    }
    NJRecord ready[NJ_OUTPUTS] = {0};
    for (unsigned i = 0; i < t->noutputs; i++) {
        NJRecord *r = &ready[i];
        r->output = t->outputs[i];
        for (unsigned k = 0; k < r->output.nspans; k++) {
            NJSpan s = r->output.spans[k];
            if (!readback || !readback(opaque, s.pa, s.bytes, r->sha256[k])) {
                nj_count(&j->hash_failures);
                nj_fail(j, NJ_FAIL_READBACK, r->output.kind, t->operation_id, s, ns);
                return;
            }
        }
        r->record_id = j->record_id + i + 1;
        r->operation_id = t->operation_id; r->write_epoch = t->write_epoch;
        r->reset_epoch = t->reset_epoch; r->start_ns = t->start_ns;
        r->completed_ns = ns; r->parent = t->parent;
        if (r->output.kind == NJ_D980) {
            r->parent = (NJParent){.status = NJ_PARENT_ROOT,
                .sensor_record_id = r->record_id,
                .sensor_model_frame = r->output.model_frame,
                .sensor_tagged = r->output.tagged};
        }
    }
    memcpy(&j->records[j->count], ready, t->noutputs * sizeof(*ready));
    j->count += t->noutputs; j->record_id += t->noutputs;
    nj_count(&j->completed_operations);
}
static inline void nj_reset(NJournal *j, unsigned kind, uint64_t ns)
{
    if (!j->enabled) { return; }
    if (j->reset_epoch == UINT64_MAX) {
        nj_unknown(j, ns);
        nj_fail(j, NJ_FAIL_COUNTER, kind, 0, (NJSpan){0}, ns);
        return;
    }
    ++j->reset_epoch;
    if (kind >= NJ_D980 && kind <= NJ_RAW1540) { nj_count(&j->reset_counts[kind]); }
    for (unsigned i = 0; i < j->count; i++) {
        NJRecord *r = &j->records[i];
        bool same = r->output.kind == kind ||
            ((kind == NJ_RAW151 || kind == NJ_NV16) &&
             (r->output.kind == NJ_RAW151 || r->output.kind == NJ_NV16));
        if (same && r->state == NJ_NO_MONITORED_OVERLAP) {
            r->state = NJ_RESET_BARRIER; r->invalidated_ns = ns;
        }
    }
}
#endif
