/* SPDX-License-Identifier: GPL-2.0-or-later
 * Explicit, bounded external quiescent MMIO read inputs. Not a chip spec.
 * Profiles must distinguish these values from measured hardware registers.
 */
#ifndef NIKON_EXPEED_REGISTER_IMAGE_H
#define NIKON_EXPEED_REGISTER_IMAGE_H

#define EXPEED_REGISTER_IMAGE_RANGES 16

typedef struct ExpeedRegisterImage {
    uint64_t base[EXPEED_REGISTER_IMAGE_RANGES];
    uint64_t bytes[EXPEED_REGISTER_IMAGE_RANGES];
    uint64_t hits[EXPEED_REGISTER_IMAGE_RANGES];
    uint64_t reads, last;
    unsigned count;
} ExpeedRegisterImage;

static inline bool expeed_register_image_configure(ExpeedRegisterImage *image,
                                                   const char *value,
                                                   Error **errp)
{
    ExpeedRegisterImage candidate = {0};
    if (!value || !*value || !strcmp(value, "off")) {
        *image = candidate;
        return true;
    }
    g_auto(GStrv) ranges = g_strsplit(value, ";", -1);
    unsigned count = g_strv_length(ranges);
    uint64_t total = 0;
    if (!count || count > EXPEED_REGISTER_IMAGE_RANGES) {
        error_setg(errp, "register image requires1..16 explicit bounded MMIO ranges");
        return false;
    }
    for (unsigned i = 0; i < count; i++) {
        g_auto(GStrv) pair = g_strsplit(ranges[i], ":", -1);
        uint64_t base, bytes;
        const char *end;
        if (g_strv_length(pair) != 2 || strncmp(pair[0], "0x", 2) ||
            strncmp(pair[1], "0x", 2) ||
            qemu_strtou64(pair[0], &end, 0, &base) || *end ||
            qemu_strtou64(pair[1], &end, 0, &bytes) || *end ||
            base < 0x10000000 || base >= 0x20000000 || !bytes ||
            bytes > 0x10000 || bytes > 0x20000000 - base ||
            (base & 3) || (bytes & 3) || total + bytes > 0x10000) {
            error_setg(errp, "register image needs aligned0xBASE:0xBYTES MMIO ranges; total<=64KiB; RAM forbidden");
            return false;
        }
        for (unsigned j = 0; j < i; j++) {
            if (base < candidate.base[j] + candidate.bytes[j] &&
                candidate.base[j] < base + bytes) {
                error_setg(errp, "register image ranges must not overlap");
                return false;
            }
        }
        candidate.base[i] = base;
        candidate.bytes[i] = bytes;
        total += bytes;
    }
    candidate.count = count;
    *image = candidate; /* Commit only a fully validated selection. */
    return true;
}

static inline bool expeed_register_image_read(ExpeedRegisterImage *image,
                                              uint64_t address, unsigned size)
{
    if (size != 4 || (address & 3)) {
        return false;
    }
    for (unsigned i = 0; i < image->count; i++) {
        if (address >= image->base[i] &&
            address - image->base[i] < image->bytes[i]) {
            image->reads++;
            image->last = address;
            image->hits[i]++;
            return true;
        }
    }
    return false;
}

static inline char *expeed_register_image_state(const ExpeedRegisterImage *image)
{
    GString *out = g_string_new(NULL);
    g_string_append_printf(out, "{\"enabled\":%s,\"quiescent_reads\":%"PRIu64
        ",\"last_address\":%"PRIu64",\"ranges\":[",
        image->count ? "true" : "false", image->reads, image->last);
    for (unsigned i = 0; i < image->count; i++) {
        g_string_append_printf(out, "%s{\"base\":%"PRIu64",\"bytes\":%"PRIu64
            ",\"reads\":%"PRIu64"}", i ? "," : "", image->base[i],
            image->bytes[i], image->hits[i]);
    }
    g_string_append(out, "],\"physical_values_verified\":false,"
        "\"scope\":\"Explicit default-off quiescent32-bit MMIO inputs after known reads; "
        "every use counted; no RAM, write, IRQ ACK, DMA or timer effects\"}");
    return g_string_free(out, false);
}

#endif
