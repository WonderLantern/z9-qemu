/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nikon-producer-journal.h"

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)
static unsigned checks;
static uint8_t ram[16384];
static int hash_calls, fail_call;
static bool digest(void *opaque, uint64_t pa, uint64_t size, uint8_t out[32])
{
    GChecksum *c = opaque;
    hash_calls++;
    if (hash_calls == fail_call || pa > sizeof(ram) || size > sizeof(ram) - pa) {
        return false;
    }
    g_checksum_reset(c);
    g_checksum_update(c, ram + pa, size);
    gsize n = 32;
    g_checksum_get_digest(c, out, &n);
    return n == 32;
}
static NJOutput output(unsigned kind, uint64_t pa, uint64_t bytes)
{
    NJOutput o = { .kind = kind, .format = NJ_PACKED14, .width = 8,
        .height = 4, .stride = 16, .model_frame = 1, .model_job = 1,
        .nspans = 1, .spans = {{pa, bytes}} };
    return o;
}
static void complete(NJournal *j, NJOutput *o, unsigned n, NJParent parent,
                     GChecksum *c)
{
    NJToken t;
    nj_begin(j, &t, o, n, parent, 10);
    nj_finish(j, &t, true, 20, digest, c);
}
static void test_disabled(GChecksum *c)
{
    NJournal j = {0}; NJOutput o = output(NJ_D980, 32, 64);
    hash_calls = 0;
    complete(&j, &o, 1, (NJParent){0}, c);
    nj_reset(&j, NJ_D980, 30);
    CHECK(!hash_calls && !j.count && !j.operation_id && !j.reset_epoch);
    checks++;
}
static void test_lineage_and_overlap(GChecksum *c)
{
    NJournal j = {.enabled = true}; NJOutput a = output(NJ_D980, 32, 64);
    complete(&j, &a, 1, (NJParent){0}, c);
    CHECK(j.count == 1 && j.records[0].state == NJ_NO_MONITORED_OVERLAP);
    uint8_t sha[32]; CHECK(digest(c, 32, 64, sha));
    NJParent p = nj_parent(&j, &a, sha, true);
    CHECK(p.status == NJ_PARENT_MATCH && p.record_id == 1);
    NJOutput b = output(NJ_RAW151, 512, 64);
    memcpy(ram + 512, ram + 32, 64);
    complete(&j, &b, 1, p, c);
    CHECK(j.count == 2 && j.records[1].parent.record_id == 1);
    CHECK(j.records[1].parent.sensor_record_id == 1);
    CHECK(j.records[1].parent.source.spans[0].pa == 32);
    CHECK(j.records[1].parent.source_hash_observed);
    NJOutput cpy = output(NJ_RAW1540, 1024, 64);
    CHECK(digest(c, 512, 64, sha));
    NJParent p2 = nj_parent(&j, &b, sha, true);
    memcpy(ram + 1024, ram + 512, 64);
    complete(&j, &cpy, 1, p2, c);
    CHECK(j.count == 3 && j.records[2].parent.record_id == 2);
    CHECK(j.records[2].parent.sensor_record_id == 1);
    a.model_frame = 2;
    NJToken t; nj_begin(&j, &t, &a, 1, (NJParent){0}, 40);
    CHECK(j.records[0].state == NJ_OVERWRITTEN);
    CHECK(j.records[1].state == NJ_NO_MONITORED_OVERLAP);
    CHECK(j.records[2].parent.record_id == 2);
    nj_finish(&j, &t, false, 41, digest, c);
    CHECK(j.count == 3 && j.partial_failures == 1 && j.gap);
    checks++;
}
static void test_ab_and_aba(GChecksum *c)
{
    NJournal j = {.enabled = true}; NJOutput a = output(NJ_D980, 32, 64);
    complete(&j, &a, 1, (NJParent){0}, c);
    NJOutput b = output(NJ_D980, 512, 64); b.model_frame = 2;
    complete(&j, &b, 1, (NJParent){0}, c);
    CHECK(j.records[0].state == NJ_NO_MONITORED_OVERLAP);
    CHECK(j.records[1].state == NJ_NO_MONITORED_OVERLAP);
    a.model_frame = 3; complete(&j, &a, 1, (NJParent){0}, c);
    CHECK(j.records[0].state == NJ_OVERWRITTEN);
    CHECK(j.records[2].record_id == 3 && j.records[2].write_epoch == 3);
    CHECK(!memcmp(j.records[0].sha256[0], j.records[2].sha256[0], 32));
    CHECK(j.records[1].state == NJ_NO_MONITORED_OVERLAP);
    checks++;
}
static void test_partial_and_readback_fail(GChecksum *c)
{
    NJournal j = {.enabled = true}; NJOutput a = output(NJ_D980, 32, 64);
    complete(&j, &a, 1, (NJParent){0}, c);
    NJOutput outs[2] = {output(NJ_NV16, 64, 64), output(NJ_RAW151, 512, 64)};
    outs[0].format = NJ_NV16_PLANES; outs[0].nspans = 2;
    outs[0].spans[1] = (NJSpan){256, 64};
    uint8_t sha[32]; CHECK(digest(c, 32, 64, sha));
    NJParent p = nj_parent(&j, &a, sha, true);
    NJToken t; nj_begin(&j, &t, outs, 2, p, 50);
    CHECK(j.records[0].state == NJ_OVERWRITTEN);
    nj_finish(&j, &t, false, 51, digest, c);
    CHECK(j.count == 1 && j.partial_failures == 1);
    hash_calls = 0; fail_call = 3;
    complete(&j, outs, 2, (NJParent){0}, c);
    CHECK(j.count == 1 && j.hash_failures == 1 && j.gap);
    CHECK(j.first_failure.code == NJ_FAIL_PRODUCER);
    fail_call = 0; checks++;
}
static void test_edges_and_unknown(GChecksum *c)
{
    NJournal j = {.enabled = true}; NJOutput a = output(NJ_D980, 32, 64);
    complete(&j, &a, 1, (NJParent){0}, c);
    NJOutput b = output(NJ_D980, 96, 64);
    complete(&j, &b, 1, (NJParent){0}, c);
    CHECK(j.records[0].state == NJ_NO_MONITORED_OVERLAP);
    b.spans[0] = (NJSpan){95, 1};
    NJToken t; nj_begin(&j, &t, &b, 1, (NJParent){0}, 50);
    CHECK(j.records[0].state == NJ_OVERWRITTEN);
    CHECK(j.records[1].state == NJ_NO_MONITORED_OVERLAP);
    nj_finish(&j, &t, false, 51, digest, c);
    b.spans[0] = (NJSpan){UINT64_MAX, 2};
    nj_begin(&j, &t, &b, 1, (NJParent){0}, 60);
    CHECK(j.records[1].state == NJ_COVERAGE_UNKNOWN && j.gap);
    CHECK(!t.publishable); checks++;
}
static void test_limits_and_reset(GChecksum *c)
{
    NJournal j = {.enabled = true}; NJOutput a = output(NJ_D980, 32, 64);
    for (unsigned i = 0; i < NJ_CAPACITY; i++) {
        a.spans[0].pa = 32 + 128 * i; a.model_frame = i + 1;
        complete(&j, &a, 1, (NJParent){0}, c);
    }
    CHECK(j.count == NJ_CAPACITY);
    a.spans[0].pa = 32;
    complete(&j, &a, 1, (NJParent){0}, c);
    CHECK(j.count == NJ_CAPACITY && j.overflows == 1);
    CHECK(j.records[0].state == NJ_OVERWRITTEN);
    j.operation_id = UINT64_MAX;
    a.spans[0].pa = 160;
    complete(&j, &a, 1, (NJParent){0}, c);
    CHECK(j.records[1].state == NJ_OVERWRITTEN && j.operation_id == UINT64_MAX);
    nj_reset(&j, NJ_D980, 80);
    CHECK(j.reset_epoch == 1 && j.records[2].state == NJ_RESET_BARRIER);
    checks++;
}
static void test_parent_before_inplace_and_reset(GChecksum *c)
{
    NJournal j = {.enabled = true}; NJOutput a = output(NJ_D980, 32, 64);
    complete(&j, &a, 1, (NJParent){0}, c);
    uint8_t sha[32]; CHECK(digest(c, 32, 64, sha));
    NJParent p = nj_parent(&j, &a, sha, true);
    NJOutput b = output(NJ_RAW151, 32, 64);
    complete(&j, &b, 1, p, c);
    CHECK(j.records[0].state == NJ_OVERWRITTEN && j.records[1].parent.record_id == 1);
    nj_reset(&j, NJ_D980, 30);
    CHECK(j.records[1].state == NJ_NO_MONITORED_OVERLAP);
    nj_reset(&j, NJ_RAW151, 40);
    CHECK(j.records[1].state == NJ_RESET_BARRIER);
    CHECK(nj_parent(&j, &b, sha, true).status == NJ_PARENT_MISSING);
    checks++;
}
static void test_parent_failures(GChecksum *c)
{
    NJournal j = {.enabled = true}; NJOutput a = output(NJ_D980, 32, 64);
    uint8_t sha[32]; CHECK(digest(c, 32, 64, sha));
    CHECK(nj_parent(&j, &a, sha, true).status == NJ_PARENT_MISSING);
    NJParent missing = nj_parent(&j, &a, sha, true);
    CHECK(missing.source_hash_observed && missing.source.spans[0].pa == 32);
    CHECK(!memcmp(missing.source_sha256, sha, 32));
    CHECK(nj_parent(&j, &a, sha, false).status == NJ_PARENT_HASH_FAILED);
    complete(&j, &a, 1, (NJParent){0}, c);
    sha[31] ^= 1;
    CHECK(nj_parent(&j, &a, sha, true).status == NJ_PARENT_HASH_MISMATCH);
    sha[31] ^= 1;
    NJOutput wrong = a; wrong.format = NJ_PACKED12;
    CHECK(nj_parent(&j, &wrong, sha, true).status == NJ_PARENT_MISSING);
    wrong = a; wrong.spans[0].bytes--;
    CHECK(nj_parent(&j, &wrong, sha, true).status == NJ_PARENT_MISSING);
    j.records[1] = j.records[0]; j.count = 2;
    CHECK(nj_parent(&j, &a, sha, true).status == NJ_PARENT_AMBIGUOUS);
    checks++;
}
static void test_bounds_and_atomic_capacity(GChecksum *c)
{
    NJournal j = {.enabled = true}; NJOutput a = output(NJ_D980, 32, 64);
    complete(&j, &a, 1, (NJParent){0}, c);
    NJOutput big = a; big.spans[0].bytes = NJ_MAX_SPAN + 1;
    NJToken t; nj_begin(&j, &t, &big, 1, (NJParent){0}, 30);
    CHECK(j.records[0].state == NJ_OVERWRITTEN && !t.publishable);
    nj_finish(&j, &t, true, 31, digest, c); CHECK(j.count == 1);
    nj_begin(&j, &t, NULL, 1, (NJParent){0}, 32); CHECK(!t.publishable);
    a.nspans = 3; nj_begin(&j, &t, &a, 1, (NJParent){0}, 33); CHECK(!t.publishable);
    a.nspans = 1; a.width = 0;
    nj_begin(&j, &t, &a, 1, (NJParent){0}, 34); CHECK(!t.publishable);
    memset(&j, 0, sizeof(j)); j.enabled = true;
    a = output(NJ_D980, 32, 64);
    for (unsigned i = 0; i < NJ_CAPACITY - 1; i++) complete(&j, &a, 1, (NJParent){0}, c);
    NJOutput pair[2] = {output(NJ_RAW151, 32, 64), output(NJ_RAW151, 512, 64)};
    nj_begin(&j, &t, pair, 2, (NJParent){0}, 35);
    CHECK(!t.publishable && j.records[62].state == NJ_OVERWRITTEN);
    nj_finish(&j, &t, true, 36, digest, c); CHECK(j.count == 63);
    checks++;
}
static void test_token_barrier(GChecksum *c)
{
    NJournal j = {.enabled = true}; NJOutput a = output(NJ_D980, 32, 64);
    NJToken t; nj_begin(&j, &t, &a, 1, (NJParent){0}, 10);
    nj_reset(&j, NJ_D980, 11);
    nj_finish(&j, &t, true, 12, digest, c);
    CHECK(!j.count && j.gap);
    memset(&j, 0, sizeof(j)); j.enabled = true;
    nj_begin(&j, &t, &a, 1, (NJParent){0}, 10);
    NJToken later; nj_begin(&j, &later, &a, 1, (NJParent){0}, 11);
    nj_finish(&j, &t, true, 12, digest, c);
    CHECK(!j.count && j.gap);
    nj_finish(&j, &later, true, 13, digest, c);
    CHECK(j.count == 1);
    nj_finish(&j, &later, true, 14, digest, c); CHECK(j.count == 1);
    checks++;
}
static void test_shape_contract(GChecksum *c)
{
    NJournal j = {.enabled = true}; NJOutput a = output(NJ_D980, 32, 64);
    complete(&j, &a, 1, (NJParent){0}, c);
    NJToken t; NJOutput wrong = a; wrong.spans[0].bytes--;
    nj_begin(&j, &t, &wrong, 1, (NJParent){0}, 30);
    CHECK(!t.publishable && j.records[0].state == NJ_OVERWRITTEN);
    wrong = a; wrong.stride = 8;
    nj_begin(&j, &t, &wrong, 1, (NJParent){0}, 31); CHECK(!t.publishable);
    wrong = a; wrong.kind = NJ_RAW151; wrong.format = NJ_PACKED12;
    nj_begin(&j, &t, &wrong, 1, (NJParent){0}, 32); CHECK(!t.publishable);
    wrong = a; wrong.kind = NJ_NV16;
    nj_begin(&j, &t, &wrong, 1, (NJParent){0}, 33); CHECK(!t.publishable);
    wrong = a; wrong.kind = NJ_NV16; wrong.format = NJ_NV16_PLANES;
    wrong.nspans = 2; wrong.spans[1] = (NJSpan){512,63};
    nj_begin(&j, &t, &wrong, 1, (NJParent){0}, 34); CHECK(!t.publishable);
    wrong.spans[1].bytes = 64;
    nj_begin(&j, &t, &wrong, 1, (NJParent){0}, 35); CHECK(t.publishable);
    nj_finish(&j, &t, true, 36, digest, c); CHECK(j.count == 2);
    checks++;
}
static void test_compressed_span(GChecksum *c)
{
    NJournal j = {.enabled = true}; NJToken t;
    NJOutput o = output(NJ_RAW1540, 4096, 23);
    o.format = NJ_F0_LOSSLESS14;
    nj_begin(&j, &t, &o, 1, (NJParent){.status=NJ_PARENT_MATCH}, 50);
    CHECK(t.publishable);
    nj_finish(&j, &t, true, 51, digest, c);
    CHECK(j.count == 1 && j.records[0].output.spans[0].bytes == 23);
    uint8_t expected[32]; CHECK(digest(c, 4096, 23, expected));
    CHECK(!memcmp(expected, j.records[0].sha256[0], 32));
    o.spans[0].bytes = 65;
    nj_begin(&j, &t, &o, 1, (NJParent){0}, 52); CHECK(!t.publishable);
    o.spans[0].bytes = 23; o.kind = NJ_D980;
    nj_begin(&j, &t, &o, 1, (NJParent){0}, 53); CHECK(!t.publishable);
    checks++;
}
int main(void)
{
    for (unsigned i = 0; i < sizeof(ram); i++) ram[i] = (i * 31 + 7) & 255;
    GChecksum *c = g_checksum_new(G_CHECKSUM_SHA256);
    test_disabled(c); test_lineage_and_overlap(c); test_ab_and_aba(c);
    test_partial_and_readback_fail(c); test_edges_and_unknown(c);
    test_limits_and_reset(c); test_parent_before_inplace_and_reset(c);
    test_parent_failures(c); test_bounds_and_atomic_capacity(c); test_token_barrier(c);
    test_shape_contract(c); test_compressed_span(c);
    g_checksum_free(c);
    printf("journal helper scenarios passed: %u\n", checks);
    return 0;
}
