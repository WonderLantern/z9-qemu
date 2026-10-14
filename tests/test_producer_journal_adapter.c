/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Compile the actual journal QEMU adapter with bounded RAM/transport stubs.
 * This is not a QEMU model transaction or a firmware test. */
#include <glib.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nikon-producer-journal.h"
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)
typedef struct Error { bool raised; } Error;
typedef struct NikonZ9State {
    NJournal producer_journal;
    NJReadFailure producer_journal_read_failure;
    bool producer_journal_initialized;
    GChecksum *producer_journal_checksum;
    uint8_t *producer_journal_scratch;
    struct { bool ram, rom_device; } image_ram;
} NikonZ9State;
typedef NikonZ9State Object;
typedef int MemTxResult;
#define NIKON_Z9_LAB(x) ((NikonZ9State *)(x))
#define MEMTXATTRS_UNSPECIFIED 0
#define MEMTX_OK 0
#define QEMU_CLOCK_VIRTUAL 0
static int address_space_memory, reads, fail_read;
static uint8_t ram[131072];
static uint64_t now = 20;
static uint64_t qemu_clock_get_ns(int clock) { (void)clock; return now; }
static void error_setg(Error **e, const char *msg)
{ (void)msg; static Error raised = {.raised = true}; *e = &raised; }
static bool dma_ram_range(NikonZ9State *s, uint64_t pa, uint64_t bytes)
{ (void)s; return bytes && pa >= 0x1000 && pa <= 0x21000 && bytes <= 0x21000 - pa; }
static int address_space_read(int *as, uint64_t pa, int attrs, void *out, size_t n)
{
    (void)as; (void)attrs; reads++;
    CHECK(pa >= 0x1000 && n <= 0x21000 - pa);
    if (reads == fail_read) { return 1; }
    memcpy(out, ram + pa - 0x1000, n); return 0;
}
#include "nikon-producer-journal-qemu.h"
int main(void)
{
    NikonZ9State s = {0}; uint8_t digest[32]; Error *err = NULL;
    CHECK(!get_producer_generation_journal((Object *)&s, &err));
    CHECK(!producer_journal_readback(&s, 0x1000, 32, digest) && !reads);
    CHECK(!producer_journal_digest(&s, ram, 32, digest));
    s.producer_journal_initialized = true;
    set_producer_generation_journal((Object *)&s, true, &err);
    CHECK(err && !s.producer_journal.enabled); /* post-init OFF cannot enable */
    s.producer_journal_initialized = false; err = NULL;
    set_producer_generation_journal((Object *)&s, true, &err);
    CHECK(!err && s.producer_journal.enabled);
    s.producer_journal_initialized = true;
    set_producer_generation_journal((Object *)&s, false, &err);
    CHECK(err && s.producer_journal.enabled); /* post-init ON cannot disable */
    s.producer_journal_checksum = g_checksum_new(G_CHECKSUM_SHA256);
    uint8_t scratch[NJ_SCRATCH_BYTES]; s.producer_journal_scratch = scratch;
    for (unsigned i = 0; i < sizeof(ram); i++) ram[i] = (i * 13 + 11) & 255;
    reads = 0;
    CHECK(producer_journal_readback(&s, 0x1000, 33001, digest) && reads == 3);
    gchar *oracle = g_compute_checksum_for_data(G_CHECKSUM_SHA256, ram, 33001);
    char hex[65]; for (unsigned i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", digest[i]);
    CHECK(!strcmp(hex, oracle)); g_free(oracle);
    int before = reads;
    CHECK(!producer_journal_readback(&s, 0x150d9800, 32, digest) && reads == before);
    CHECK(!producer_journal_readback(&s, UINT64_MAX - 1, 32, digest) && reads == before);
    CHECK(!producer_journal_readback(&s, 0x20fff, 2, digest) && reads == before);
    CHECK(!producer_journal_readback(&s, 0x1000, 0, digest) && reads == before);
    s.producer_journal_read_failure = (NJReadFailure){0};
    reads = 0; fail_read = 2;
    CHECK(!producer_journal_readback(&s, 0x1000, 33001, digest) && reads == 2);
    CHECK(s.producer_journal_read_failure.recorded);
    CHECK(s.producer_journal_read_failure.span_pa == 0x1000);
    CHECK(s.producer_journal_read_failure.span_bytes == 33001);
    CHECK(s.producer_journal_read_failure.chunk_pa == 0x5000);
    CHECK(s.producer_journal_read_failure.chunk_bytes == 16384);
    CHECK(s.producer_journal_read_failure.memtx_result_present);
    CHECK(s.producer_journal_read_failure.memtx_result == 1);
    NJReadFailure first = s.producer_journal_read_failure;
    reads = 0; fail_read = 3;
    CHECK(!producer_journal_readback(&s, 0x1000, 33001, digest));
    CHECK(!memcmp(&first, &s.producer_journal_read_failure, sizeof(first)));
    s.producer_journal_read_failure = (NJReadFailure){0};
    reads = 0;
    CHECK(!producer_journal_readback(&s, 0x1000, 33001, digest));
    CHECK(s.producer_journal_read_failure.chunk_pa == 0x9000);
    CHECK(s.producer_journal_read_failure.chunk_bytes == 233);
    fail_read = 0;
    NJOutput a = producer_journal_output(NJ_D980, NJ_PACKED14, 0x1000, 33024, 8, 2064, 16, 1, 1, true);
    NJToken t; nj_begin(&s.producer_journal, &t, &a, 1, (NJParent){0}, 10);
    producer_journal_finish(&s, &t, true);
    CHECK(s.producer_journal.count == 1);
    NJParent parent = producer_journal_parent(&s, &a, ram, 33024);
    CHECK(parent.status == NJ_PARENT_MATCH && parent.record_id == 1);
    CHECK(producer_journal_parent(&s, &a, ram, 33000).status == NJ_PARENT_HASH_FAILED);
    /* Missing scratch/readback failure does not turn original success into an IRQ/status edit. */
    unsigned original_counter = 7, original_pending = 0x100;
    s.producer_journal_scratch = NULL;
    NJOutput b = producer_journal_output(NJ_RAW151, NJ_PACKED14, 0x12000, 33024, 8, 2064, 16, 1, 1, true);
    nj_begin(&s.producer_journal, &t, &b, 1, parent, 10);
    producer_journal_finish(&s, &t, true);
    CHECK(s.producer_journal.count == 1 && s.producer_journal.hash_failures == 1);
    CHECK(original_counter == 7 && original_pending == 0x100);
    s.producer_journal_scratch = scratch;
    memcpy(ram + 0x11000, ram, 33024);
    nj_begin(&s.producer_journal, &t, &b, 1, parent, 10);
    producer_journal_finish(&s, &t, true);
    CHECK(s.producer_journal.count == 2);
    NJournal copy = s.producer_journal;
    char *json = get_producer_generation_journal_state((Object *)&s, &err);
    CHECK(!memcmp(&copy, &s.producer_journal, sizeof(copy))); /* getter observational */
    puts(json); g_free(json);
    g_checksum_free(s.producer_journal_checksum);
    return 0;
}
