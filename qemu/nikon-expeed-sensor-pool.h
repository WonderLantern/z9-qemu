/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Functional ownership of actual CMOS DMA RAM. No pixel snapshots, guest
 * scheduler edits, inferred completions, or physical FIFO-depth claims. */
static Ex7SensorFrame *sensor_pool_find(NikonZ9State *s, uint64_t address)
{
    if (!s->sensor_pool_model) { return NULL; }
    for (unsigned i=0;i<ARRAY_SIZE(s->sensor_pool);i++) {
        Ex7SensorFrame *f=&s->sensor_pool[i];
        if (f->frame && f->address==address) { return f; }
    }
    return NULL;
}

static bool sensor_pool_protected(NikonZ9State *s, const Ex7SensorFrame *f)
{
    if (!f->frame) { return false; }
    if (f->nr_bank && (!f->isp_done || s->engine153_pending[f->nr_bank-1])) { return true; }
    if (s->capture_nv16_model && f->isp_done && !f->stream_consumed) { return true; }
    if (s->engine151_capture_pending && s->engine151_capture_source==f->address) { return true; }
    return false;
}

static Ex7SensorFrame *sensor_pool_reserve(NikonZ9State *s, uint64_t address, uint64_t bytes)
{
    Ex7SensorFrame *slot=NULL;
    for (unsigned i=0;i<ARRAY_SIZE(s->sensor_pool);i++) {
        Ex7SensorFrame *f=&s->sensor_pool[i];
        if (!f->frame) { if (!slot) { slot=f; } continue; }
        if (s->capture_nv16_model && f->isp_done && !f->stream_consumed &&
            ((address<f->isp_y+f->isp_bytes && f->isp_y<address+bytes) ||
             (address<f->isp_uv+f->isp_bytes && f->isp_uv<address+bytes))) {
            s->sensor_pool_blocked++;return NULL;
        }
        if (address<f->address+f->bytes && f->address<address+bytes) {
            if (sensor_pool_protected(s,f)) { s->sensor_pool_blocked++; return NULL; }
            slot=f;
        }
    }
    if (!slot) { s->sensor_pool_blocked++; }
    return slot;
}

static void sensor_pool_record(NikonZ9State *s, Ex7SensorFrame *slot,
    uint64_t address,uint64_t bytes,unsigned width,unsigned height,unsigned pitch,unsigned format)
{
    for (unsigned i=0;i<ARRAY_SIZE(s->sensor_pool);i++) {
        Ex7SensorFrame *f=&s->sensor_pool[i];
        if (f->frame && address<f->address+f->bytes && f->address<address+bytes) {
            s->sensor_pool_replaced++;f->frame=0;
        }
    }
    *slot=(Ex7SensorFrame){.address=address,.bytes=bytes,.frame=s->primary_d980_frames,
        .width=width,.height=height,.pitch=pitch,.format=format};
}

static void sensor_pool_cancel_nr(NikonZ9State *s,unsigned bank)
{
    if (!s->sensor_pool_model) { return; }
    for (unsigned i=0;i<ARRAY_SIZE(s->sensor_pool);i++) {
        Ex7SensorFrame *f=&s->sensor_pool[i];
        if (f->frame && f->nr_bank==bank+1 && !f->isp_done &&
            !(s->engine151_capture_pending && s->engine151_capture_source==f->address)) {
            f->nr_bank=0;
        }
    }
}

/* A live front2->3 strobe admits the newly committed descriptor. Each commit
 * can supply once; another source tick or sticky IRQ cannot replay it. */
static bool sensor_pool_request(NikonZ9State *s)
{
    if (!s->sensor_pool_model || !(s->engine150c_active&0x4000) ||
        !s->primary_d980_submits || video_raw_descriptor(s)) { return true; }
    if (s->sensor_request_submit==s->primary_d980_submits) { return true; }
    if (s->sensor_request_pending) { return false; }
    memcpy(s->primary_d980_latched,s->primary_d980_submitted,sizeof(s->primary_d980_latched));
    s->primary_d980_slot=0;
    s->sensor_request_submit=s->primary_d980_submits;
    s->sensor_request_pending=true;
    s->sensor_pool_requests++;
    return true;
}

/* Keep the complete actual151 output while its original stream request
 * is delayed. Matching addresses/geometry select a particular ISP job. */
static bool sensor_isp_writable(NikonZ9State *s,uint64_t y,uint64_t uv,uint64_t bytes)
{
    if (!s->sensor_pool_model || !s->capture_nv16_model) { return true; }
    for (unsigned i=0;i<ARRAY_SIZE(s->sensor_pool);i++) {
        const Ex7SensorFrame *f=&s->sensor_pool[i];
        if (!f->frame || !f->isp_done || f->stream_consumed) { continue; }
        if ((y<f->isp_y+f->isp_bytes && f->isp_y<y+bytes) ||
            (y<f->isp_uv+f->isp_bytes && f->isp_uv<y+bytes) ||
            (uv<f->isp_y+f->isp_bytes && f->isp_y<uv+bytes) ||
            (uv<f->isp_uv+f->isp_bytes && f->isp_uv<uv+bytes)) { return false; }
    }
    return true;
}

static Ex7SensorFrame *sensor_isp_find(NikonZ9State *s,uint64_t y,uint64_t uv,
    unsigned width,unsigned height,unsigned stride)
{
    if (!s->sensor_pool_model) { return NULL; }
    for (unsigned i=0;i<ARRAY_SIZE(s->sensor_pool);i++) {
        Ex7SensorFrame *f=&s->sensor_pool[i];
        if (f->frame && f->isp_done && !f->stream_consumed && f->isp_y==y && f->isp_uv==uv &&
            f->isp_width==width && f->isp_height==height && f->isp_stride==stride) { return f; }
    }
    return NULL;
}

/* Functional deferred wake, not measured silicon timing. Avoid reentering
 * the151 producer while the matching stream is still committing its IRQ. */
static void sensor_isp_wake(void *opaque)
{
    engine151_capture_wait_try(opaque);
}

static bool get_sensor_pool_model(Object *obj,Error **errp)
{ return NIKON_Z9_LAB(obj)->sensor_pool_model; }

static void set_sensor_pool_model(Object *obj,bool value,Error **errp)
{
    NikonZ9State *s=NIKON_Z9_LAB(obj);
    if (s->image_ram.ram || s->image_ram.rom_device) {
        error_setg(errp,"primary-d980-frame-pool-model is immutable after realization");return;
    }
    s->sensor_pool_model=value;
}

static char *get_sensor_pool_state(Object *obj,Error **errp)
{
    NikonZ9State *s=NIKON_Z9_LAB(obj);GString *o=g_string_new(NULL);
    g_string_append_printf(o,"{\"enabled\":%s,\"request_pending\":%s,\"request_submit\":%"PRIu64
        ",\"requests\":%"PRIu64",\"blocked\":%"PRIu64",\"replaced\":%"PRIu64",\"frames\":[",
        s->sensor_pool_model?"true":"false",s->sensor_request_pending?"true":"false",
        s->sensor_request_submit,s->sensor_pool_requests,s->sensor_pool_blocked,s->sensor_pool_replaced);
    bool comma=false;
    for (unsigned i=0;i<ARRAY_SIZE(s->sensor_pool);i++) {
        const Ex7SensorFrame *f=&s->sensor_pool[i];if (!f->frame) { continue; }
        g_string_append_printf(o,"%s{\"address\":%"PRIu64",\"bytes\":%"PRIu64
            ",\"frame\":%"PRIu64",\"width\":%u,\"height\":%u,\"pitch\":%u,\"format\":%u,"
            "\"nr_bank\":%u,\"isp_done\":%s,\"protected\":%s}",comma?",":"",f->address,f->bytes,
            f->frame,f->width,f->height,f->pitch,f->format,f->nr_bank,f->isp_done?"true":"false",
            sensor_pool_protected(s,f)?"true":"false");comma=true;
    }
    g_string_append(o,"],\"physical_protocol_verified\":false,\"scope\":\"Actual DMA RAM ownership across committed live front requests; IRQ pending and W1C remain separate\"}");
    return g_string_free(o,false);
}
