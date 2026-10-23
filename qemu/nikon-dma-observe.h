/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Optional board-write provenance. No address translation, write replacement,
 * completion, interrupt or guest-state repair. Input is an explicit bounded
 * work-RAM interval; no firmware addresses are compiled into this observer. */
static gsize wl_dma_observer_once;
static uint64_t wl_dma_observer_low, wl_dma_observer_high;
static gint wl_dma_observer_records;

static void wl_dma_observer_configure(void)
{
    if (g_once_init_enter(&wl_dma_observer_once)) {
        const char *text = g_getenv("WL_DMA_WRITE_OBSERVE");
        uint64_t low = 0, high = 0;
        char tail;
        if (text && (sscanf(text, "%"SCNx64":%"SCNx64"%c", &low, &high, &tail) != 2 ||
            low < UINT64_C(0xa00000000) || high > UINT64_C(0xa20000000) ||
            low >= high || high - low > 16 * MiB)) {
            error_report("DMA observer requires a bounded work-RAM interval");
            exit(1);
        }
        wl_dma_observer_low = low;
        wl_dma_observer_high = high;
        g_once_init_leave(&wl_dma_observer_once, 1);
    }
}

static void wl_dma_observer_record(AddressSpace *as, hwaddr address,
    MemTxAttrs attrs, const void *buffer, hwaddr bytes, int result,
    const char *producer, unsigned line)
{
    wl_dma_observer_configure();
    uint64_t low = wl_dma_observer_low, high = wl_dma_observer_high;
    if (!high || as != &address_space_memory || !bytes || address >= high ||
        bytes > UINT64_MAX - address || address + bytes <= low) {
        return;
    }
    gint index = g_atomic_int_add(&wl_dma_observer_records, 1);
    if (index > 4096) { return; }
    if (index == 4096) {
        fprintf(stderr, "WL_DMA_OBSERVE {\"limit_reached\":true,\"limit\":4096}\n");
        return;
    }
    uint64_t start = MAX(address, low), end = MIN(address + bytes, high);
    unsigned n = MIN(end - start, 64);
    const uint8_t *expected = (const uint8_t *)buffer + (start - address);
    uint8_t actual[64];
    bool matches = address_space_read(as, start, attrs, actual, n) == MEMTX_OK &&
                   !memcmp(actual, expected, n);
    GString *out = g_string_new(NULL);
    g_string_append_printf(out, "WL_DMA_OBSERVE {\"index\":%d,\"producer\":\"%s\","
        "\"line\":%u,\"virtual_ns\":%"PRId64",\"address\":\"0x%"PRIx64"\","
        "\"bytes\":%"PRIu64",\"api_result\":%d,\"sample_address\":\"0x%"PRIx64"\","
        "\"sample_matches_RAM\":%s,\"sample_hex\":\"",
        index, producer, line, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL),
        (uint64_t)address, (uint64_t)bytes, result, start, matches ? "true" : "false");
    for (unsigned i = 0; i < n; i++) { g_string_append_printf(out, "%02x", expected[i]); }
    g_string_append(out, "\"}\n");
    fputs(out->str, stderr);
    g_string_free(out, true);
}

static MemTxResult wl_observed_address_space_write(AddressSpace *as, hwaddr address,
    MemTxAttrs attrs, const void *buffer, hwaddr bytes, const char *producer, unsigned line)
{
    MemTxResult result = address_space_write(as, address, attrs, buffer, bytes);
    wl_dma_observer_record(as, address, attrs, buffer, bytes, result, producer, line);
    return result;
}

static void wl_observed_cpu_physical_write(hwaddr address, const void *buffer,
    hwaddr bytes, const char *producer, unsigned line)
{
    cpu_physical_memory_write(address, buffer, bytes);
    /* The void API has no completion result. Its sampled RAM check is explicit;
     * -1 must not be interpreted as either a successful or failed transaction. */
    wl_dma_observer_record(&address_space_memory, address, MEMTXATTRS_UNSPECIFIED,
                           buffer, bytes, -1, producer, line);
}

#define address_space_write(as, address, attrs, buffer, bytes) \
    wl_observed_address_space_write(as, address, attrs, buffer, bytes, __func__, __LINE__)
#define cpu_physical_memory_write(address, buffer, bytes) \
    wl_observed_cpu_physical_write(address, buffer, bytes, __func__, __LINE__)
