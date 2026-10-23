/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Functional capture stream: actual owned NV16 RAM -> configured outputs.
 * Internal 16-bit packing and Nikon filter/ISP algorithms are not emulated.
 * The semantic stream retains NV16; fixed-point phases/scales use a declared
 * nearest-sample convention. No CPU state, OS flags or camera files supplied.
 */
static uint64_t capture_nv16_address(const uint32_t *c, unsigned off)
{
    return c[off / 4] | ((uint64_t)(c[(off + 4) / 4] & 15) << 32);
}

static bool capture_nv16_span(NikonZ9State *s, uint64_t p, uint64_t n)
{
    return n && n <= 64 * MiB && !(p & 31) &&
        p >= UINT64_C(0x800000000) && p <= UINT64_C(0x900000000) - n &&
        dma_ram_range(s, p, n);
}

static unsigned capture_nv16_coordinate(const uint32_t *f, unsigned at, bool y)
{
    uint64_t phase = f[(y ? 0xc : 8) / 4];
    uint64_t scale = f[(y ? 0x14 : 0x10) / 4];
    return ((phase + at * scale) << f[0x20 / 4]) >> 14;
}

static bool capture_nv16_prepare(NikonZ9State *s)
{
    const uint32_t *src = s->engine1518[0].config;
    const uint32_t *a = s->engine1542_config[1];
    const uint32_t *c = s->engine1542_config[11];
    unsigned w = src[0x80 / 4] & 0x7fff, h = src[0x80 / 4] >> 16;
    unsigned stride = src[0x88 / 4];
    uint64_t p[8], n[8], total = 0;
    uint64_t fifo0 = capture_nv16_address(src, 0x110);
    uint64_t fifo1 = capture_nv16_address(src, 0x118);
    if (s->capture_nv16_requested || (s->engine1542_pending[11] & 0x20) ||
        (s->engine1542_pending[1] & 0x11) ||
        (s->engine1518[0].pending & (1u << 20)) ||
        !w || w > 16384 || (w & 1) || !h || h > 8192 ||
        stride < w || stride > 32768 || src[0x8c / 4] != stride ||
        src[0xbc / 4] != 2 || src[0xc0 / 4] != 1 ||
        src[0xc4 / 4] != 0x20 || src[0xd4 / 4] != 0x20 ||
        src[0xb0 / 4] != ((w * 2 + 255) & ~255) ||
        src[0xb4 / 4] != src[0xb0 / 4] ||
        fifo0 != UINT64_C(0xe0000000) || fifo1 != fifo0 + src[0xb0 / 4] / 2 ||
        (c[4 / 4] != 0x103 && c[4 / 4] != 0x100) || c[8 / 4] != 2 || c[0xc / 4] ||
        capture_nv16_address(c, 0x200) != fifo0 || capture_nv16_address(c, 0x208) != fifo1 ||
        a[0x4c / 4] != w || a[0x50 / 4] != h ||
        a[0x38 / 4] != src[0xb0 / 4] || a[0x3c / 4] != src[0xb4 / 4] ||
        capture_nv16_address(a, 0x20) != fifo0 || capture_nv16_address(a, 0x28) != fifo1) {
        return false;
    }
    p[0] = capture_nv16_address(src, 0x100);
    p[1] = capture_nv16_address(src, 0x108);
    n[0] = n[1] = (uint64_t)stride * h;
    for (unsigned j = 0; j < 3; j++) {
        const uint32_t *b = s->engine1542_config[6 + j];
        const uint32_t *f = s->engine1542_config[18 + j];
        unsigned ow = f[0x2c / 4], oh = f[0x30 / 4], pitch = b[0x38 / 4];
        if ((f[0] != 2 && f[0] != 4) || (f[1] != 1 && f[1] != 4) ||
            f[0x18 / 4] != w || f[0x1c / 4] != h || f[0x24 / 4] || f[0x28 / 4] ||
            !ow || (ow & 1) || ow > 16384 || !oh || oh > 8192 ||
            pitch < ow || pitch > 32768 || b[0x3c / 4] != pitch || b[0x40 / 4] != 0x22 ||
            b[0xc / 4] != 0x41000010 ||
            !f[0x10 / 4] || !f[0x14 / 4] || f[0x20 / 4] > 7 ||
            capture_nv16_coordinate(f, ow - 1, false) >= w ||
            capture_nv16_coordinate(f, oh - 1, true) >= h) { return false; }
        p[2 + j * 2] = capture_nv16_address(b, 0x20);
        p[3 + j * 2] = capture_nv16_address(b, 0x28);
        n[2 + j * 2] = n[3 + j * 2] = (uint64_t)pitch * oh;
        total += n[2 + j * 2] * 2;
    }
    if (total > 192 * MiB) { return false; }
    for (unsigned j = 0; j < 8; j++) {
        if (!capture_nv16_span(s, p[j], n[j])) { return false; }
        for (unsigned k = 0; k < j; k++) {
            if (p[j] < p[k] + n[k] && p[k] < p[j] + n[j]) { return false; }
        }
    }
    if (!capture_pool_writable(s,p+2,n+2,6,3)) { return false; }
    memcpy(s->capture_nv16_source, src, sizeof(s->capture_nv16_source));
    for (unsigned j = 0; j < 3; j++) {
        memcpy(s->capture_nv16_output[j], s->engine1542_config[6 + j], sizeof(s->capture_nv16_output[j]));
        memcpy(s->capture_nv16_geometry[j], s->engine1542_config[18 + j], sizeof(s->capture_nv16_geometry[j]));
    }
    s->capture_nv16_requested = true; s->capture_nv16_failed = false;
    s->capture_nv16_requests++;
    return true;
}

static void capture_nv16_cancel(NikonZ9State *s)
{
    if (s->capture_nv16_requested) {
        s->capture_nv16_requested = false; s->capture_nv16_cancels++;
    }
}

static void capture_nv16_try(NikonZ9State *s)
{
    if (!s->capture_nv16_model || !s->capture_nv16_requested || s->capture_nv16_failed ||
        !s->engine1543_active[1] || s->engine1543_held[1] ||
        !s->engine1518[0].config[0x30 / 4]) { return; }
    const uint32_t *src = s->capture_nv16_source;
    unsigned w = src[0x80 / 4] & 0x7fff, h = src[0x80 / 4] >> 16;
    unsigned stride = src[0x88 / 4];
    uint64_t y = capture_nv16_address(src, 0x100), uv = capture_nv16_address(src, 0x108);
    Ex7IspSurface *owner=sensor_isp_find(s,y,uv,w,h,stride);
    if (s->sensor_pool_model) {
        if (!owner) { return; }
    } else if (!s->engine151_capture_jobs || s->capture_nv16_consumed151 == s->engine151_capture_jobs ||
        y != s->engine151_capture_y || uv != s->engine151_capture_uv ||
        w != s->engine151_capture_width || h != s->engine151_capture_height ||
        stride != s->engine151_capture_stride) { return; }
    uint64_t dp[6],dn[6];
    for (unsigned j=0;j<3;j++) {
        const uint32_t *b=s->capture_nv16_output[j],*f=s->capture_nv16_geometry[j];
        dp[j*2]=capture_nv16_address(b,0x20);dp[j*2+1]=capture_nv16_address(b,0x28);
        dn[j*2]=dn[j*2+1]=(uint64_t)b[0x38/4]*f[0x30/4];
    }
    if (!capture_pool_writable(s,dp,dn,6,3)) { s->capture_nv16_failed=true;return; }
    size_t plane = (size_t)stride * h;
    uint8_t *input = g_malloc(plane * 2), *outputs[3] = {0};
    bool ok = address_space_read(&address_space_memory, y, MEMTXATTRS_UNSPECIFIED, input, plane) == MEMTX_OK &&
        address_space_read(&address_space_memory, uv, MEMTXATTRS_UNSPECIFIED, input + plane, plane) == MEMTX_OK;
    /* Build every output before any write. Pointers/complete spans were
     * validated and latched together. No internal port is aliased to RAM. */
    for (unsigned j = 0; ok && j < 3; j++) {
        const uint32_t *b = s->capture_nv16_output[j], *f = s->capture_nv16_geometry[j];
        unsigned ow = f[0x2c / 4], oh = f[0x30 / 4], pitch = b[0x38 / 4];
        size_t amount = (size_t)pitch * oh;
        outputs[j] = g_malloc(amount * 2);
        memset(outputs[j], 0, amount); memset(outputs[j] + amount, 128, amount);
        for (unsigned r = 0; r < oh; r++) {
            unsigned sy = capture_nv16_coordinate(f, r, true);
            for (unsigned x = 0; x < ow; x++) {
                unsigned sx = capture_nv16_coordinate(f, x, false);
                outputs[j][(size_t)r * pitch + x] = input[(size_t)sy * stride + sx];
            }
            for (unsigned x = 0; x < ow; x += 2) {
                unsigned sx = capture_nv16_coordinate(f, x, false) & ~1u;
                outputs[j][amount + (size_t)r * pitch + x] = input[plane + (size_t)sy * stride + sx];
                outputs[j][amount + (size_t)r * pitch + x + 1] = input[plane + (size_t)sy * stride + sx + 1];
            }
        }
    }
    for (unsigned j = 0; ok && j < 3; j++) {
        const uint32_t *b = s->capture_nv16_output[j], *f = s->capture_nv16_geometry[j];
        size_t amount = (size_t)b[0x38 / 4] * f[0x30 / 4];
        uint64_t py = capture_nv16_address(b, 0x20), puv = capture_nv16_address(b, 0x28);
        ok = address_space_write(&address_space_memory, py, MEMTXATTRS_UNSPECIFIED, outputs[j], amount) == MEMTX_OK &&
            address_space_write(&address_space_memory, puv, MEMTXATTRS_UNSPECIFIED, outputs[j] + amount, amount) == MEMTX_OK;
    }
    g_free(input);
    for (unsigned j = 0; j < 3; j++) { g_free(outputs[j]); }
    if (!ok) { s->capture_nv16_failed = true; return; }
    s->capture_nv16_jobs++; s->capture_nv16_input_bytes += plane * 2;
    s->capture_nv16_consumed151 = owner ? owner->job : s->engine151_capture_jobs;
    if (owner) {
        owner->consumed=true;
        timer_mod(s->sensor_isp_timer,qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)+1);
    }
    for (unsigned j = 0; j < 3; j++) {
        const uint32_t *b = s->capture_nv16_output[j], *f = s->capture_nv16_geometry[j];
        s->capture_nv16_y[j] = capture_nv16_address(b, 0x20);
        s->capture_nv16_uv[j] = capture_nv16_address(b, 0x28);
        s->capture_nv16_w[j] = f[0x2c / 4]; s->capture_nv16_h[j] = f[0x30 / 4];
        s->capture_nv16_stride[j] = b[0x38 / 4];
        capture_pool_record(s,0,j,s->capture_nv16_jobs,s->capture_nv16_y[j],s->capture_nv16_uv[j],
            s->capture_nv16_w[j],s->capture_nv16_h[j],s->capture_nv16_stride[j]);
        s->capture_nv16_bytes += (uint64_t)b[0x38 / 4] * f[0x30 / 4] * 2;
    }
    s->capture_nv16_requested = false;
    /* The original application waits for A1 input boundary bit4 separately
     * from C1 output completion bit5. Emit both input boundaries only after
     * the actual complete stream/output writes, with the original A1 input
     * gate enabled. Native509 otherwise reaches the thumbnail stage with
     * both source-frame ready bytes still zero. No task flags are supplied. */
    if (s->engine1542_config[1][0x10 / 4] == 1) {
        s->engine1542_pending[1] |= 0x11;
    }
    /* Original ISR dispatches C1 as41160031, raw bit5 subscribed by20.
     * Source ISR dispatches bit20 as the asynchronous source-done event.
     * Functional completion is issued only after all six actual RAM writes. */
    s->engine1542_pending[11] |= 0x20;
    s->engine1518[0].pending |= 1u << 20;
    raw_pipeline_irq_update(s); engine1518_update_irq(&s->engine1518[0]);
    engine15e2_deferred_input_notify(s);
    fprintf(stderr, "Z9_CAPTURE_NV16 jobs=%" PRIu64 " source=%" PRIx64 "/%" PRIx64
        " main=%" PRIx64 "/%" PRIx64 " geometry=%ux%u; actual RAM, functional nearest sampling\n",
        s->capture_nv16_jobs, y, uv, s->capture_nv16_y[0], s->capture_nv16_uv[0], w, h);
}

static int capture_nv16_jpeg_owner(NikonZ9State *s, unsigned bank)
{
    if (!s->capture_nv16_model || !s->capture_nv16_jobs) { return -1; }
    const uint32_t *c = s->engine15e2_config[bank];
    for (unsigned j = 0; j < 3; j++) {
        if (s->capture_nv16_jpeg_consumed[j] != s->capture_nv16_jobs &&
            c[0x418 / 4] == s->capture_nv16_w[j] && c[0x41c / 4] == s->capture_nv16_h[j] &&
            c[0x410 / 4] == s->capture_nv16_stride[j] &&
            ((uint64_t)c[0x430 / 4] << 4) == s->capture_nv16_y[j] &&
            ((uint64_t)c[0x434 / 4] << 4) == s->capture_nv16_uv[j]) { return j; }
    }
    return -1;
}

static bool get_capture_nv16_model(Object *obj, Error **errp)
{ return NIKON_Z9_LAB(obj)->capture_nv16_model; }

static void set_capture_nv16_model(Object *obj, bool value, Error **errp)
{
    NikonZ9State *s = NIKON_Z9_LAB(obj);
    if (s->image_ram.ram || s->image_ram.rom_device) {
        error_setg(errp, "capture-nv16-pipeline-model is immutable after realization"); return;
    }
    s->capture_nv16_model = value;
}

static char *get_capture_nv16_state(Object *obj, Error **errp)
{
    NikonZ9State *s = NIKON_Z9_LAB(obj);
    GString *out = g_string_new(NULL);
    g_string_append_printf(out, "{\"enabled\":%s,\"requested\":%s,\"failed\":%s,"
        "\"requests\":%" PRIu64 ",\"jobs\":%" PRIu64 ",\"bytes\":%" PRIu64 ","
        "\"input_bytes\":%" PRIu64 ",\"cancels\":%" PRIu64 ",\"consumed151job\":%" PRIu64 ","
        "\"source_pending\":%u,\"receiver_pending\":%u,\"input_pending\":%u,\"outputs\":[",
        s->capture_nv16_model ? "true" : "false", s->capture_nv16_requested ? "true" : "false",
        s->capture_nv16_failed ? "true" : "false", s->capture_nv16_requests, s->capture_nv16_jobs,
        s->capture_nv16_bytes, s->capture_nv16_input_bytes, s->capture_nv16_cancels,
        s->capture_nv16_consumed151, s->engine1518[0].pending, s->engine1542_pending[11],
        s->engine1542_pending[1]);
    for (unsigned j = 0; j < 3; j++) {
        g_string_append_printf(out, "%s{\"y\":%" PRIu64 ",\"uv\":%" PRIu64 ","
            "\"width\":%u,\"height\":%u,\"stride\":%u,\"jpeg_consumed_job\":%" PRIu64 "}",
            j ? "," : "", s->capture_nv16_y[j], s->capture_nv16_uv[j],
            s->capture_nv16_w[j], s->capture_nv16_h[j], s->capture_nv16_stride[j], s->capture_nv16_jpeg_consumed[j]);
    }
    g_string_append(out, "],\"physical_algorithm_verified\":false,\"physical_stream_packing_verified\":false,"
        "\"native_file_saved\":false,\"scope\":\"owned151 NV16 RAM through semantic internal stream to three latched real outputs; functional fixed-point nearest sampling, no physical ISP/packing/file proof\"}");
    return g_string_free(out, false);
}
