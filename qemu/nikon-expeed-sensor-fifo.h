/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Functional request/RAW/ISP ownership. The64-entry bounds are simulation
 * limits, not recovered hardware FIFO depths. Metadata never supplies pixels. */
static bool sensor_overlap(uint64_t a,uint64_t n,uint64_t b,uint64_t m)
{ return a<b+m && b<a+n; }

static bool sensor_isp_overlap(NikonZ9State *s,uint64_t y,uint64_t uv,uint64_t bytes)
{
    if (!s->sensor_pool_model || !s->capture_nv16_model) { return false; }
    for (unsigned i=0;i<ARRAY_SIZE(s->sensor_isp_surfaces);i++) {
        const Ex7IspSurface *f=&s->sensor_isp_surfaces[i];
        if (!f->valid || f->consumed) { continue; }
        if (sensor_overlap(y,bytes,f->y,f->bytes) || sensor_overlap(y,bytes,f->uv,f->bytes) ||
            sensor_overlap(uv,bytes,f->y,f->bytes) || sensor_overlap(uv,bytes,f->uv,f->bytes)) { return true; }
    }
    return false;
}

static bool sensor_isp_writable(NikonZ9State *s,uint64_t y,uint64_t uv,uint64_t bytes)
{
    if (!s->sensor_pool_model || !s->capture_nv16_model) { return true; }
    if (sensor_isp_overlap(s,y,uv,bytes)) { return false; }
    for (unsigned i=0;i<ARRAY_SIZE(s->sensor_isp_surfaces);i++) {
        if (!s->sensor_isp_surfaces[i].valid || s->sensor_isp_surfaces[i].consumed) { return true; }
    }
    return false;
}

static void sensor_isp_record(NikonZ9State *s,uint64_t frame,uint64_t job,
    uint64_t y,uint64_t uv,uint64_t bytes,unsigned w,unsigned h,unsigned stride)
{
    if (!s->sensor_pool_model || !s->capture_nv16_model) { return; }
    Ex7IspSurface *slot=NULL;
    for (unsigned i=0;i<ARRAY_SIZE(s->sensor_isp_surfaces);i++) {
        Ex7IspSurface *f=&s->sensor_isp_surfaces[i];
        if (f->valid && (sensor_overlap(y,bytes,f->y,f->bytes) || sensor_overlap(y,bytes,f->uv,f->bytes) ||
            sensor_overlap(uv,bytes,f->y,f->bytes) || sensor_overlap(uv,bytes,f->uv,f->bytes))) {
            g_assert(f->consumed);f->valid=false;
        }
        if ((!f->valid || f->consumed) && !slot) { slot=f; }
    }
    g_assert(slot);
    *slot=(Ex7IspSurface){.frame=frame,.job=job,.y=y,.uv=uv,.bytes=bytes,
        .width=w,.height=h,.stride=stride,.valid=true};
}

static Ex7IspSurface *sensor_isp_find(NikonZ9State *s,uint64_t y,uint64_t uv,
    unsigned width,unsigned height,unsigned stride)
{
    if (!s->sensor_pool_model) { return NULL; }
    for (unsigned i=0;i<ARRAY_SIZE(s->sensor_isp_surfaces);i++) {
        Ex7IspSurface *f=&s->sensor_isp_surfaces[i];
        if (f->valid && !f->consumed && f->y==y && f->uv==uv &&
            f->width==width && f->height==height && f->stride==stride) { return f; }
    }
    return NULL;
}

static Ex7SensorFrame *sensor_pool_find(NikonZ9State *s,uint64_t address)
{
    if (!s->sensor_pool_model) { return NULL; }
    for (unsigned i=0;i<ARRAY_SIZE(s->sensor_pool);i++) {
        Ex7SensorFrame *f=&s->sensor_pool[i];if (f->frame && f->address==address) { return f; }
    }
    return NULL;
}

static bool sensor_pool_protected(NikonZ9State *s,const Ex7SensorFrame *f)
{
    if (!f->frame) { return false; }
    if (f->nr_bank && (!f->isp_done || (s->engine153_pending[f->nr_bank-1] &&
        s->engine153_consumed_dma_frame[f->nr_bank-1]==f->frame))) { return true; }
    return s->engine151_capture_pending && s->engine151_capture_source==f->address;
}

static Ex7SensorFrame *sensor_pool_reserve(NikonZ9State *s,uint64_t address,uint64_t bytes)
{
    if (sensor_isp_overlap(s,address,address,bytes)) { s->sensor_pool_blocked++;return NULL; }
    Ex7SensorFrame *slot=NULL;
    for (unsigned i=0;i<ARRAY_SIZE(s->sensor_pool);i++) {
        Ex7SensorFrame *f=&s->sensor_pool[i];
        if (!f->frame) { if (!slot) { slot=f; }continue; }
        if (sensor_overlap(address,bytes,f->address,f->bytes)) {
            if (sensor_pool_protected(s,f)) { s->sensor_pool_blocked++;return NULL; }
            slot=f;
        }
    }
    if (!slot) { s->sensor_pool_blocked++; }return slot;
}

static void sensor_pool_record(NikonZ9State *s,Ex7SensorFrame *slot,
    uint64_t address,uint64_t bytes,unsigned width,unsigned height,unsigned pitch,unsigned format)
{
    for (unsigned i=0;i<ARRAY_SIZE(s->sensor_pool);i++) {
        Ex7SensorFrame *f=&s->sensor_pool[i];
        if (f->frame && sensor_overlap(address,bytes,f->address,f->bytes)) { s->sensor_pool_replaced++;f->frame=0; }
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
            !(s->engine151_capture_pending && s->engine151_capture_source==f->address)) { f->nr_bank=0; }
    }
}

/* Old prepared finite state cannot own a real running-clock admission.
 * Retire only the legacy timer/preparation, never the actual FIFO or data.
 * A completed finite head is processed before handing over the next head. */
static void sensor_request_handoff(NikonZ9State *s)
{
    if (!s->event_raw_counter_model || !s->sensor_source_valid || s->sensor_source_finite ||
        (!s->primary_d980_single_armed && !s->primary_d980_prepared)) { return; }
    s->sensor_source_handoffs++;
    s->primary_d980_prepare_cancelled += s->primary_d980_prepared;
    s->primary_d980_single_cancelled += s->primary_d980_single_armed;
    s->primary_d980_prepared = false;
    s->primary_d980_single_armed = false;
    if (s->primary_d980_single_timer) { timer_del(s->primary_d980_single_timer); }
}

static void sensor_request_load(NikonZ9State *s)
{
    s->sensor_request_pending=s->sensor_request_count!=0;
    if (s->sensor_request_pending) {
        s->sensor_source_valid = true;
        s->sensor_source_finite = s->sensor_requests[s->sensor_request_head].finite;
        memcpy(s->primary_d980_latched,s->sensor_requests[s->sensor_request_head].descriptor,sizeof(s->primary_d980_latched));
        s->primary_d980_slot=0;
        Ex7SensorRequest *r=&s->sensor_requests[s->sensor_request_head];
        if (s->sensor_readout_floor_model && !r->readout_started &&
            (!s->sensor_manual_acquisition_model || r->manual_triggered)) {
            r->readout_started=true;
            r->readout_begin_ns=qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
            r->readout_period_ns=event_source_period_ns(s,0);
            r->readout_earliest_ns=r->readout_begin_ns+r->readout_period_ns;
            s->sensor_readout_trace_until=r->readout_earliest_ns+r->readout_period_ns;
            sensor_readout_trace(s,"head_acquisition_begin");
            event_global0_finite_acquire(s);
        }
    }
}

static bool sensor_pool_request(NikonZ9State *s)
{
    if (!s->sensor_pool_model || !(s->engine150c_active&0x4000) || !s->primary_d980_submits || video_raw_descriptor(s)) { return true; }
    /* START can reuse a committed descriptor. Its new active-unit epoch
     * reserves another request; its selected source still has to acquire it.
     * Repeated front edges or ACKs in the same epoch remain deduplicated. */
    if (s->sensor_request_submit==s->primary_d980_submits &&
        (!s->primary_d980_live_single_model || !s->primary_d980_single_model ||
         s->sensor_request_epoch==s->primary_d980_input_epoch)) { return true; }
    unsigned w,h,pitch;uint64_t encoded,physical;
    if (s->sensor_request_count==ARRAY_SIZE(s->sensor_requests) ||
        !primary_d980_description(s,s->primary_d980_submitted,0,&w,&h,&pitch,&encoded,&physical)) { return false; }
    unsigned at=(s->sensor_request_head+s->sensor_request_count)%ARRAY_SIZE(s->sensor_requests);
    Ex7SensorRequest *r=&s->sensor_requests[at];
    memcpy(r->descriptor,s->primary_d980_submitted,sizeof(r->descriptor));r->submit=s->primary_d980_submits;
    r->finite = !s->event_timing_active;
    r->admitted_ns=qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    r->manual_triggered=false;
    r->readout_started=false;r->readout_begin_ns=0;
    r->readout_earliest_ns=0;r->readout_period_ns=0;
    s->sensor_request_submit=r->submit;s->sensor_request_epoch=s->primary_d980_input_epoch;
    s->sensor_request_count++;s->sensor_pool_requests++;
    if (s->sensor_request_count==1) { sensor_request_load(s);sensor_request_handoff(s); }return true;
}

static bool sensor_manual_frame_owned(NikonZ9State *s)
{
    return s->sensor_manual_acquisition_model && s->sensor_request_count &&
        s->sensor_requests[s->sensor_request_head].manual_triggered;
}

static void sensor_manual_trigger(NikonZ9State *s)
{
    /* The input edge also starts a finite capture while preview is stopped.
     * The committed descriptor, active RAW unit and actual edge still own
     * admission; neither the preview clock nor its timer is a trigger. */
    if (!s->sensor_manual_acquisition_model ||
        !(s->engine150c_active&0x4000) || video_raw_descriptor(s)) { return; }
    for (unsigned i=0;i<s->sensor_request_count;i++) {
        Ex7SensorRequest *r=&s->sensor_requests[(s->sensor_request_head+i)%ARRAY_SIZE(s->sensor_requests)];
        if (r->manual_triggered) { continue; }
        r->manual_triggered=true;s->sensor_manual_triggers++;
        s->sensor_manual_last_ns=qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        if (!i) { sensor_request_load(s); }
        sensor_readout_trace(s,"actual_manual_acquisition");return;
    }
    s->sensor_manual_empty_edges++;
}

static void sensor_request_done(NikonZ9State *s)
{
    if (s->sensor_request_count) {
        s->sensor_request_head=(s->sensor_request_head+1)%ARRAY_SIZE(s->sensor_requests);s->sensor_request_count--;
    }
    sensor_request_load(s);
}

static void sensor_request_cancel(NikonZ9State *s)
{
    s->sensor_requests_cancelled+=s->sensor_request_count;s->sensor_request_count=0;s->sensor_request_head=0;
    s->sensor_request_pending=false;s->sensor_source_valid=false;
}

static bool sensor_request_mature(NikonZ9State *s)
{
    if (!s->sensor_readout_floor_model) { return true; }
    if (!s->sensor_pool_model || !s->sensor_request_count) { return false; }
    const Ex7SensorRequest *r=&s->sensor_requests[s->sensor_request_head];
    if (!r->readout_started || qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)<r->readout_earliest_ns) {
        s->sensor_readout_blocks++;return false;
    }
    return true;
}

static bool sensor_request_has_source(NikonZ9State *s,uint64_t source)
{
    if (!s->sensor_pool_model) { return false; }
    for (unsigned i=0;i<s->sensor_request_count;i++) {
        const Ex7SensorRequest *r=&s->sensor_requests[(s->sensor_request_head+i)%ARRAY_SIZE(s->sensor_requests)];
        unsigned w,h,pitch;uint64_t encoded,physical;
        if (primary_d980_description(s,r->descriptor,0,&w,&h,&pitch,&encoded,&physical) && physical==source) { return true; }
    }
    return false;
}

/* Deferred functional wake avoids reentering151 inside a stream commit. */
static void sensor_isp_wake(void *opaque) { engine151_capture_wait_try(opaque); }
static bool get_sensor_pool_model(Object *obj,Error **errp) { return NIKON_Z9_LAB(obj)->sensor_pool_model; }
static void set_sensor_pool_model(Object *obj,bool value,Error **errp)
{
    NikonZ9State *s=NIKON_Z9_LAB(obj);
    if (s->image_ram.ram || s->image_ram.rom_device) { error_setg(errp,"primary-d980-frame-pool-model is immutable after realization");return; }
    s->sensor_pool_model=value;
}

static char *get_sensor_pool_state(Object *obj,Error **errp)
{
    NikonZ9State *s=NIKON_Z9_LAB(obj);GString *o=g_string_new(NULL);
    g_string_append_printf(o,"{\"enabled\":%s,\"request_pending\":%s,\"request_submit\":%"PRIu64
        ",\"requests\":%"PRIu64",\"blocked\":%"PRIu64",\"replaced\":%"PRIu64
        ",\"pending_requests\":%u,\"cancelled_requests\":%"PRIu64",\"request_capacity\":64,\"queued\":[",
        s->sensor_pool_model?"true":"false",s->sensor_request_pending?"true":"false",s->sensor_request_submit,
        s->sensor_pool_requests,s->sensor_pool_blocked,s->sensor_pool_replaced,s->sensor_request_count,s->sensor_requests_cancelled);
    for (unsigned i=0;i<s->sensor_request_count;i++) {
        const Ex7SensorRequest *r=&s->sensor_requests[(s->sensor_request_head+i)%ARRAY_SIZE(s->sensor_requests)];
        unsigned w,h,pitch;uint64_t encoded,physical;
        bool valid=primary_d980_description(s,r->descriptor,0,&w,&h,&pitch,&encoded,&physical);
        g_string_append_printf(o,"%s{\"submit\":%"PRIu64",\"physical\":%"PRIu64"}",i?",":"",r->submit,valid?physical:0);
    }
    g_string_append_printf(o,"],\"request_epoch\":%"PRIu64",\"frames\":[",s->sensor_request_epoch);bool comma=false;
    for (unsigned i=0;i<ARRAY_SIZE(s->sensor_pool);i++) {
        const Ex7SensorFrame *f=&s->sensor_pool[i];if (!f->frame) { continue; }
        g_string_append_printf(o,"%s{\"address\":%"PRIu64",\"bytes\":%"PRIu64",\"frame\":%"PRIu64
            ",\"width\":%u,\"height\":%u,\"pitch\":%u,\"format\":%u,\"nr_bank\":%u,\"isp_done\":%s,\"protected\":%s}",
            comma?",":"",f->address,f->bytes,f->frame,f->width,f->height,f->pitch,f->format,f->nr_bank,
            f->isp_done?"true":"false",sensor_pool_protected(s,f)?"true":"false");comma=true;
    }
    g_string_append(o,"],\"isp_frames\":[");comma=false;
    for (unsigned i=0;i<ARRAY_SIZE(s->sensor_isp_surfaces);i++) {
        const Ex7IspSurface *f=&s->sensor_isp_surfaces[i];if (!f->valid) { continue; }
        g_string_append_printf(o,"%s{\"frame\":%"PRIu64",\"job\":%"PRIu64",\"y\":%"PRIu64
            ",\"uv\":%"PRIu64",\"bytes\":%"PRIu64",\"consumed\":%s}",comma?",":"",f->frame,f->job,f->y,f->uv,f->bytes,f->consumed?"true":"false");comma=true;
    }
    g_string_append(o,"],\"physical_protocol_verified\":false,\"scope\":\"Bounded committed request FIFO and separate actual RAW/ISP RAM owners; IRQ and W1C remain independent\"}");
    return g_string_free(o,false);
}
