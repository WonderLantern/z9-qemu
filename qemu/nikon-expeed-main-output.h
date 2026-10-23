/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Explicit functional fourth ISP output and registered YUVCMP source view.
 * Native configuration relates logical C0 aliases to real backing RAM.
 * This optional model stores uncompressed NV12 byte8 or packed10 big-word data;
 * it does not implement Nikon YUVCMP, ISP arithmetic or physical bus timing.
 * Packed10 preserves ten component bits through tone mapping and YUV. No
 * firmware PC, task flag, version or display address selects this behavior.
 */
#include <zlib.h>

#define WL_MAIN_HEADER 64
#define WL_MAIN_SURFACES 32

typedef struct WlMainSurface {
    bool valid, leased, transported;
    unsigned width, height, storage_height, stride, table, table_row, source_format;
    uint64_t alias[2], physical[2], bytes[2], epoch;
    uint64_t capacity[2], stored[2];
} WlMainSurface;

/* Read-only bounded production evidence, retained when slots are recycled. */
typedef struct WlMainProduction {
    uint64_t time_ns, epoch, physical[2], alias[2];
    unsigned bank, row, selected, table, table_row;
} WlMainProduction;

typedef struct WlMainOutputState {
    WlMainSurface surface[WL_MAIN_SURFACES];
    WlMainProduction history[128];
    uint8_t *cache;
    const NikonRawPixels *cache_source;
    uint8_t *packed_packet[2];
    uLongf packet_bytes[2];
    uint32_t cache_crc;
    unsigned cache_w, cache_h, cache_storage_h, cache_stride, cache_format;
    unsigned source_w, source_h, source_white;
    uint64_t cache_input_frame;
    uint64_t frames, bytes, rejects, blocked, cache_builds, acquisitions, releases;
    uint64_t transport_frames, packet_builds;
    const char *rejection;
} WlMainOutputState;

typedef struct WlMainPlan {
    bool active;
    unsigned slot, bank, row;
    WlMainSurface surface;
} WlMainPlan;

static const uint32_t main_table_bases[] = {0x15649000,0x15649800,0x15661000};
/* Output and reader clients have separate alias namespaces. Original client
 * dispatch selects +9000/+9800 versus +21000 from the common15640000 base.
 * Native566 enables both with the same aliases and different backing rows.
 * These are functional routes; physical compression remains unimplemented. */
static const uint32_t main_output_table_bases[] = {0x15649000,0x15649800};

/* Both four-row groups share the controller's geometry/format registers.
 * The original client writes group0 at +280/+2a0 and group1 at +380/+3a0.
 * Keep group identity in table_row rather than treating +100 as a controller. */
static unsigned main_row_offset(unsigned row)
{ return (row&3)*8+(row>>2)*0x100; }

static uint32_t main_register(NikonZ9State *s, uint32_t address)
{
    return s->yuvcmp_config[(address-0x15640000)/4];
}

static uint64_t main_pointer(NikonZ9State *s, uint32_t address)
{
    uint32_t hi=main_register(s,address+4);
    return hi>15 ? UINT64_MAX : main_register(s,address) | (uint64_t)hi<<32;
}

static bool main_overlap(uint64_t a,uint64_t an,uint64_t b,uint64_t bn)
{ return a<b+bn && b<a+an; }

static bool video_main_armed(NikonZ9State *s)
{
    return s->video_main_model && s->engine1542_config[6][0x10/4]==1 &&
        s->engine1542_config[18][0x2c/4] && s->engine1542_config[18][0x30/4];
}

static bool video_main_locked(NikonZ9State *s,uint64_t address,uint64_t bytes)
{
    if (!s->video_main) { return false; }
    for (unsigned k=0;k<WL_MAIN_SURFACES;k++) {
        const WlMainSurface *f=&s->video_main->surface[k];
        for (unsigned p=0;p<2;p++) {
            if (f->leased && main_overlap(address,bytes,f->physical[p],f->stored[p])) { return true; }
        }
    }
    return false;
}

static int main_reject(WlMainOutputState *m,const char *reason)
{ m->rejects++;m->rejection=reason;return -1; }

/* A producer row can be rebound while a separate reader still names its old
 * physical frame. Keep bounded metadata for actual RAM, not a row-index cache.
 * At most24 pairs are named by these three eight-row ports; spare slots allow
 * a new production while preserving all named or borrowed frames. */
static bool main_surface_named(NikonZ9State *s,const WlMainSurface *f)
{
    if (!f->epoch) { return false; }
    for (unsigned k=0;k<G_N_ELEMENTS(main_table_bases);k++) {
        for (unsigned plane=0;plane<2;plane++) {
            uint32_t pb=main_table_bases[k]+plane*0x400;
            for (unsigned row=0;row<8;row++) {
                uint64_t physical=main_pointer(s,pb+0x2a0+main_row_offset(row));
                if (physical==f->physical[0] || physical==f->physical[1]) { return true; }
            }
        }
    }
    return false;
}

static int video_main_prepare(NikonZ9State *s,const NikonRawPixels *pixels,uint64_t epoch,
    const uint32_t *ring,unsigned bank,unsigned row,const uint64_t *preview,
    const uint64_t *preview_bytes,WlMainPlan *plan,bool primary)
{
    memset(plan,0,sizeof(*plan));
    if (!s->video_main_model) { return 0; }
    if (!s->video_main) { s->video_main=g_new0(WlMainOutputState,1); }
    WlMainOutputState *m=s->video_main;
    for (unsigned j=0;j<6;j++) {
        if (video_main_locked(s,preview[j],preview_bytes[j])) {
            m->blocked++;m->rejection="preview overlaps borrowed main surface";return -1;
        }
    }
    if (!primary || !video_main_armed(s)) { return 0; }
    const uint32_t *b=s->engine1542_config[6],*g=s->engine1542_config[18];
    unsigned width=g[0x2c/4],height=g[0x30/4],stride=b[0x38/4];
    /* Original pre-recording B3 lacks bit9, table class0 and byte pitch;
     * REC posts bit9, class9 and three10-bit samples per32-bit word. This
     * is a typed functional view of those two observed configurations,
     * not a verified physical YUVCMP/ISP format specification. F3+18/1c
     * are distinct input/scaler dimensions and need not equal its output. */
    bool packed10=!!(b[0xc/4]&0x200);
    unsigned format=packed10?0x332:0x302;
    unsigned minrow=packed10?(width+2)/3*4:((width+3)&~3U);
    if (!pixels || !epoch || !width || !height || ((width|height)&1) ||
        width>4096 || height>4096 || (stride&3) || stride<minrow ||
        stride>8192 || stride!=b[0x3c/4] ||
        (b[0xc/4]&0x10) || b[0x40/4]!=0x22 || b[0x48/4]!=0x22) {
        return main_reject(m,"unsupported main packed10 geometry or format");
    }
    WlMainSurface f={.width=width,.height=height,.stride=stride,.epoch=epoch,.source_format=format};
    unsigned off=(0x240+bank*0x200+row*0x80)/4,matches=0;
    f.alias[0]=ring[off] | (uint64_t)ring[off+1]<<32;
    f.alias[1]=ring[off+2] | (uint64_t)ring[off+3]<<32;
    for (unsigned k=0;k<G_N_ELEMENTS(main_output_table_bases);k++) {
        uint32_t base=main_output_table_bases[k];
        if (!(main_register(s,base)&1) || !(main_register(s,base+0x400)&1)) { continue; }
        for (unsigned r=0;r<8;r++) {
            unsigned ro=main_row_offset(r);
            if (main_pointer(s,base+0x280+ro)!=f.alias[0] ||
                main_pointer(s,base+0x680+ro)!=f.alias[1]) { continue; }
            unsigned wh=main_register(s,base+0x244),uh=main_register(s,base+0x644);
            unsigned declared_h=wh>>16;
            uint64_t reservation=main_register(s,base+0x218);
            unsigned storage_h=stride ? reservation/stride : 0;
            /* Active producer geometry and allocated rows differ for AVC:
             * native674 posts1080 but reserves1088. Both plane lengths must
             * agree on the padded allocation; no implicit extra RAM span. */
            if ((wh&0xffff)!=width || (uh&0xffff)!=width || storage_h<height ||
                storage_h-height>64 || (storage_h&1) || reservation!=(uint64_t)stride*storage_h ||
                declared_h<height || declared_h>storage_h || (declared_h&1) || (uh>>16)!=declared_h/2 ||
                main_register(s,base+0x200)!=(packed10?9:0) ||
                main_register(s,base+0x600)!=(packed10?9:0)) {
                return main_reject(m,"registered main table geometry or format mismatch");
            }
            for (unsigned p=0;p<2;p++) {
                uint32_t pb=base+p*0x400;
                uint64_t bytes=(uint64_t)stride*storage_h/(p?2:1),physical;
                if (!bytes || bytes>32*MiB || main_register(s,pb+0x21c)!=stride ||
                    main_register(s,pb+0x220)!=stride || main_register(s,pb+0x218)!=bytes ||
                    !raw_pipeline_address(s,main_pointer(s,pb+0x2a0+ro),1,&physical)) {
                    return main_reject(m,"registered main backing span is invalid");
                }
                f.physical[p]=physical;f.bytes[p]=bytes;
            }
            f.table=base;f.table_row=r;f.storage_height=storage_h;matches++;
        }
    }
    if (matches!=1 || f.physical[0]==f.physical[1]) {
        return main_reject(m,"main aliases have no unique separate registered backing");
    }
    /* Logical YUVCMP surface sizes can overlap when applied to compact
     * physical allocations. Bound a functional packet by the next distinct
     * registered backing, rather than writing a logical-sized raw plane
     * through another buffer. This conservative model bound is not a
     * recovered Nikon compression/allocation specification. */
    for (unsigned p=0;p<2;p++) {
        f.capacity[p]=f.bytes[p];
        for (unsigned k=0;k<G_N_ELEMENTS(main_table_bases);k++) {
            uint32_t base=main_table_bases[k];
            for (unsigned plane=0;plane<2;plane++) {
                uint32_t pb=base+plane*0x400;
                if (!main_register(s,pb+0x244)) { continue; }
                for (unsigned r=0;r<8;r++) {
                    uint64_t next=main_pointer(s,pb+0x2a0+main_row_offset(r));
                    if (next>f.physical[p] && next-f.physical[p]<f.capacity[p]) {
                        f.capacity[p]=next-f.physical[p];
                    }
                }
            }
        }
        f.transported|=f.capacity[p]<f.bytes[p];
        if (!dma_ram_range(s,f.physical[p],f.capacity[p])) {
            return main_reject(m,"main physical reservation is unmapped");
        }
    }
    unsigned slot=WL_MAIN_SURFACES;
    for (unsigned k=0;k<WL_MAIN_SURFACES;k++) {
        const WlMainSurface *old=&m->surface[k];
        if (old->epoch && old->physical[0]==f.physical[0] && old->physical[1]==f.physical[1]) {
            slot=k;break;
        }
    }
    if (slot==WL_MAIN_SURFACES && !m->surface[bank*4+row].epoch) { slot=bank*4+row; }
    for (unsigned k=0;slot==WL_MAIN_SURFACES && k<WL_MAIN_SURFACES;k++) {
        if (!m->surface[k].epoch) { slot=k; }
    }
    if (slot==WL_MAIN_SURFACES) {
        uint64_t oldest=UINT64_MAX;
        for (unsigned k=0;k<WL_MAIN_SURFACES;k++) {
            const WlMainSurface *old=&m->surface[k];
            if (!old->leased && old->epoch<oldest && !main_surface_named(s,old)) {
                oldest=old->epoch;slot=k;
            }
        }
    }
    if (slot==WL_MAIN_SURFACES || m->surface[slot].leased) {
        m->blocked++;m->rejection="all physical main slots remain named or borrowed";return -1;
    }
    unsigned white=pixels->white;
    uint64_t input_frame = !s->raw_temporal_input_model ? 0 :
        pixels==s->raw_pixels ? s->raw_temporal_input_frames :
        pixels==s->video_secondary_sensor_pixels ? s->video_secondary_sensor_input_frame : 0;
    if (!m->cache || m->cache_input_frame!=input_frame || m->cache_source!=pixels || m->cache_crc!=s->video_raw_pixels_crc ||
        m->cache_w!=width || m->cache_h!=height || m->cache_storage_h!=f.storage_height ||
        m->cache_stride!=stride || m->cache_format!=format ||
        m->source_w!=pixels->width || m->source_h!=pixels->height ||
        m->source_white!=white) {
        g_autofree uint8_t *nv12=packed10?NULL:nikon_raw_pixels_yuv(pixels,width,height,width,true);
        g_autofree uint16_t *nv12_10=packed10?nikon_raw_pixels_yuv10(pixels,width,height):NULL;
        if (packed10?!nv12_10:!nv12) { return main_reject(m,"main Bayer conversion failed"); }
        uint8_t *packed=g_malloc0(f.bytes[0]+f.bytes[1]);
        for (unsigned p=0;p<2;p++) {
            unsigned rows=height/(p?2:1);
            const uint8_t *input=nv12?nv12+(p?(uint64_t)width*height:0):NULL;
            const uint16_t *input10=nv12_10?nv12_10+(p?(uint64_t)width*height:0):NULL;
            uint8_t *output=packed+(p?f.bytes[0]:0);
            for (unsigned y=0;y<rows;y++) {
                if (format==0x302) {
                    for (unsigned x=0;x<width;x++) {
                        output[(size_t)y*stride+(x^3U)]=input[(size_t)y*width+x];
                    }
                    continue;
                }
                for (unsigned x=0;x<width;x+=3) {
                    uint32_t word=0;
                    for (unsigned n=0;n<3 && x+n<width;n++) {
                        unsigned v=input10[(size_t)y*width+x+n];
                        word|=v<<(22-n*10);
                    }
                    stl_be_p(output+(size_t)y*stride+(x/3)*4,word);
                }
            }
        }
        for (unsigned p=0;p<2;p++) {
            g_clear_pointer(&m->packed_packet[p],g_free);m->packet_bytes[p]=0;
        }
        g_free(m->cache);m->cache=packed;m->cache_crc=s->video_raw_pixels_crc;
        m->cache_source=pixels;m->cache_input_frame=input_frame;
        m->cache_w=width;m->cache_h=height;m->cache_storage_h=f.storage_height;
        m->cache_stride=stride;m->cache_format=format;
        m->source_w=pixels->width;m->source_h=pixels->height;
        m->source_white=white;m->cache_builds++;
    }
    if (f.transported && !m->packed_packet[0]) {
        for (unsigned p=0;p<2;p++) {
            m->packet_bytes[p]=compressBound(f.bytes[p]);
            m->packed_packet[p]=g_malloc(m->packet_bytes[p]);
            if (compress2(m->packed_packet[p],&m->packet_bytes[p],
                m->cache+(p?f.bytes[0]:0),f.bytes[p],1)!=Z_OK) {
                for (unsigned q=0;q<2;q++) { g_clear_pointer(&m->packed_packet[q],g_free); }
                return main_reject(m,"functional compact main packet failed");
            }
        }
        m->packet_builds++;
    }
    for (unsigned p=0;p<2;p++) {
        f.stored[p]=f.transported?WL_MAIN_HEADER+m->packet_bytes[p]:f.bytes[p];
        if (f.stored[p]>f.capacity[p]) {
            return main_reject(m,"functional main packet does not fit compact reservation");
        }
        if (video_main_locked(s,f.physical[p],f.stored[p])) {
            m->blocked++;m->rejection="main backing remains borrowed until result query";return -1;
        }
        for (unsigned j=0;j<6;j++) {
            if (main_overlap(f.physical[p],f.stored[p],preview[j],preview_bytes[j])) {
                return main_reject(m,"main and preview spans overlap");
            }
        }
    }
    plan->active=true;plan->slot=slot;plan->bank=bank;plan->row=row;plan->surface=f;
    m->rejection=NULL;return 1;
}

static bool video_main_commit(NikonZ9State *s,const WlMainPlan *plan)
{
    if (!plan->active) { return true; }
    WlMainOutputState *m=s->video_main;const WlMainSurface *f=&plan->surface;
    for (unsigned p=0;p<2;p++) {
        if (f->transported) {
            uint8_t h[WL_MAIN_HEADER]={0};memcpy(h,"WLUV0001",8);
            stl_le_p(h+8,1);stl_le_p(h+12,p);stl_le_p(h+16,f->width);stl_le_p(h+20,f->height);
            stl_le_p(h+24,f->storage_height);stl_le_p(h+28,f->stride);stl_le_p(h+32,f->source_format);
            stl_le_p(h+36,m->packet_bytes[p]);stq_le_p(h+40,f->bytes[p]);stq_le_p(h+48,f->epoch);
            stl_le_p(h+56,crc32(0,m->cache+(p?f->bytes[0]:0),f->bytes[p]));
            stl_le_p(h+60,crc32(0,h,60));
            if (address_space_write(&address_space_memory,f->physical[p],MEMTXATTRS_UNSPECIFIED,h,sizeof(h))!=MEMTX_OK ||
                address_space_write(&address_space_memory,f->physical[p]+WL_MAIN_HEADER,MEMTXATTRS_UNSPECIFIED,
                    m->packed_packet[p],m->packet_bytes[p])!=MEMTX_OK) {
                main_reject(m,"compact main RAM transfer failed");return false;
            }
        } else if (address_space_write(&address_space_memory,f->physical[p],MEMTXATTRS_UNSPECIFIED,
                       m->cache+(p?f->bytes[0]:0),f->bytes[p])!=MEMTX_OK) {
            main_reject(m,"main RAM transfer failed");return false;
        }
    }
    for (unsigned k=0;k<WL_MAIN_SURFACES;k++) {
        WlMainSurface *old=&m->surface[k];
        if (!old->epoch || old->leased) { continue; }
        bool replaced=false;
        for (unsigned p=0;p<2;p++) {
            for (unsigned q=0;q<2;q++) {
                replaced|=main_overlap(old->physical[p],old->stored[p],f->physical[q],f->stored[q]);
            }
        }
        if (replaced) { old->valid=false;old->epoch=0; }
    }
    m->surface[plan->slot]=*f;m->surface[plan->slot].valid=true;
    WlMainProduction *record=&m->history[m->frames%G_N_ELEMENTS(m->history)];
    *record=(WlMainProduction){.time_ns=qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL),
        .epoch=f->epoch,.physical={f->physical[0],f->physical[1]},
        .alias={f->alias[0],f->alias[1]},.bank=plan->bank,.row=plan->row,
        .selected=s->engine1542_config[10][2]&1,.table=f->table,.table_row=f->table_row};
    m->frames++;m->bytes+=f->stored[0]+f->stored[1];m->transport_frames+=f->transported;return true;
}

static uint64_t video_main_physical_bytes(NikonZ9State *s,unsigned token,unsigned plane)
{ return s->video_main->surface[token-1].stored[plane]; }

static bool video_main_read(NikonZ9State *s,unsigned token,unsigned plane,uint8_t **decoded)
{
    const WlMainSurface *f=&s->video_main->surface[token-1];
    if (!f->transported) { *decoded=NULL;return true; }
    uint8_t h[WL_MAIN_HEADER];
    if (address_space_read(&address_space_memory,f->physical[plane],MEMTXATTRS_UNSPECIFIED,h,sizeof(h))!=MEMTX_OK ||
        memcmp(h,"WLUV0001",8) || ldl_le_p(h+8)!=1 || ldl_le_p(h+12)!=plane ||
        ldl_le_p(h+16)!=f->width || ldl_le_p(h+20)!=f->height || ldl_le_p(h+24)!=f->storage_height ||
        ldl_le_p(h+28)!=f->stride || ldl_le_p(h+32)!=f->source_format ||
        (uint64_t)ldl_le_p(h+36)+WL_MAIN_HEADER!=f->stored[plane] ||
        ldq_le_p(h+40)!=f->bytes[plane] || ldq_le_p(h+48)!=f->epoch ||
        /* QEMU ldl_le_p returns signed int; zlib crc32 returns uLong.
         * Preserve the checksum's 32 bits before the LP64 comparison. */
        (uint32_t)ldl_le_p(h+60)!=crc32(0,h,60)) { return false; }
    g_autofree uint8_t *packet=g_malloc(f->stored[plane]-WL_MAIN_HEADER);
    if (address_space_read(&address_space_memory,f->physical[plane]+WL_MAIN_HEADER,MEMTXATTRS_UNSPECIFIED,
            packet,f->stored[plane]-WL_MAIN_HEADER)!=MEMTX_OK) { return false; }
    uint8_t *body=g_malloc(f->bytes[plane]);uLongf size=f->bytes[plane];
    if (uncompress(body,&size,packet,f->stored[plane]-WL_MAIN_HEADER)!=Z_OK ||
        size!=f->bytes[plane] || crc32(0,body,size)!=(uint32_t)ldl_le_p(h+56)) {
        g_free(body);return false;
    }
    *decoded=body;return true;
}


/* The row DMA client reads one logical plane after codec result retirement.
 * A retired codec token is not erased RAM. Resolve the current reader table
 * to an actually produced backing and read its bytes/packet, without reviving
 * encoder ownership or synthesizing a new frame. */
static int video_main_dma_snapshot(NikonZ9State *s,uint64_t source,uint64_t span,
    uint8_t **snapshot,uint64_t *backing,uint64_t *stored)
{
    if (!s->video_main_model) { return 0; }
    bool reader=(main_register(s,0x15661000)&1)||(main_register(s,0x15661400)&1);
    bool declared=false;unsigned declarations=0,matches=0,token=0,selected_plane=0;
    uint64_t selected_offset=0;
    for (unsigned k=0;k<G_N_ELEMENTS(main_table_bases);k++) {
        uint32_t table=main_table_bases[k];
        if ((table==0x15661000)!=reader) { continue; }
        for (unsigned plane=0;plane<2;plane++) {
            uint32_t pb=table+plane*0x400;
            for (unsigned row=0;row<8;row++) {
                unsigned ro=main_row_offset(row);
                uint64_t alias=main_pointer(s,pb+0x280+ro);
                uint64_t length=main_register(s,pb+0x218);
                if (!alias || alias==UINT64_MAX || source<alias || source-alias>=length) { continue; }
                declared=true;declarations++;uint64_t offset=source-alias,physical;
                if (!(main_register(s,pb)&1) || !span || span>length-offset ||
                    !raw_pipeline_address(s,main_pointer(s,pb+0x2a0+ro),1,&physical) || !s->video_main) { continue; }
                for (unsigned n=0;n<WL_MAIN_SURFACES;n++) {
                    const WlMainSurface *f=&s->video_main->surface[n];
                    if (!f->epoch || f->physical[plane]!=physical || offset>=f->bytes[plane] ||
                        span>f->bytes[plane]-offset ||
                        main_register(s,pb+0x200)!=(f->source_format==0x332?9:0) ||
                        (main_register(s,pb+0x244)&0xffff)!=f->width ||
                        main_register(s,pb+0x21c)!=f->stride || main_register(s,pb+0x220)!=f->stride ||
                        length!=(uint64_t)f->stride*(main_register(s,pb+0x244)>>16) ||
                        (main_register(s,pb+0x244)>>16)<f->height/(plane?2:1)) { continue; }
                    token=n+1;selected_plane=plane;selected_offset=offset;matches++;
                }
            }
        }
    }
    if (matches!=1 || declarations!=1) { return declared ? -1 : 0; }
    const WlMainSurface *f=&s->video_main->surface[token-1];
    g_autofree uint8_t *decoded=NULL;
    if (!video_main_read(s,token,selected_plane,&decoded)) { return -1; }
    uint8_t *body=g_malloc(span);
    if (decoded) { memcpy(body,decoded+selected_offset,span); }
    else if (address_space_read(&address_space_memory,f->physical[selected_plane]+selected_offset,
        MEMTXATTRS_UNSPECIFIED,body,span)!=MEMTX_OK) { g_free(body);return -1; }
    *snapshot=body;*backing=f->physical[selected_plane];*stored=f->stored[selected_plane];
    return 1;
}

/* Reader-side aliases may select a different producer row. Never use
 * alias equality across clients as proof of actual frame identity. */
static int video_main_reader_route(NikonZ9State *s,unsigned ybus,unsigned uvbus,
    unsigned width,unsigned height,unsigned sy,unsigned sc,unsigned format,uint64_t *physical)
{
    const uint32_t base=0x15661000;
    bool yactive=main_register(s,base)&1,uvactive=main_register(s,base+0x400)&1;
    if (!yactive && !uvactive) { return 0; }
    unsigned matches=0;
    for (unsigned r=0;r<8;r++) {
        unsigned ro=main_row_offset(r);
        uint64_t y=main_pointer(s,base+0x280+ro),uv=main_pointer(s,base+0x680+ro);
        if (y==UINT64_MAX || uv==UINT64_MAX || !y || !uv) { continue; }
        if ((uint32_t)y!=ybus || (uint32_t)uv!=uvbus) { continue; }
        if (!yactive || !uvactive || sy!=sc) { return -1; }
        unsigned yh=main_register(s,base+0x244),uh=main_register(s,base+0x644);
        unsigned stored=yh>>16;
        if ((yh&0xffff)!=width || (uh&0xffff)!=width || stored<height ||
            stored-height>64 || (stored&1) || (uh>>16)!=stored/2) { return -1; }
        for (unsigned plane=0;plane<2;plane++) {
            uint32_t pb=base+plane*0x400;
            if (main_register(s,pb+0x200)!=(format==0x332?9:0) ||
                main_register(s,pb+0x21c)!=sy || main_register(s,pb+0x220)!=sy ||
                main_register(s,pb+0x218)!=(uint64_t)sy*stored/(plane?2:1) ||
                !raw_pipeline_address(s,main_pointer(s,pb+0x2a0+ro),1,&physical[plane])) { return -1; }
        }
        matches++;
    }
    return matches==1 ? 1 : -1;
}

/* Source-only alias resolution. Program/work/bitstream DMA retains its own
 * existing address translator. A registered but unproduced or mixed pair
 * is rejected; it cannot fall back to zero-filled code-address RAM. */
static int video_main_resolve(NikonZ9State *s,unsigned ybus,unsigned uvbus,
    unsigned width,unsigned height,unsigned visible_width,unsigned visible_height,
    unsigned sy,unsigned sc,unsigned format,uint64_t *physical,unsigned *token)
{
    if (!s->video_main_model) { return 0; }
    uint64_t reader_physical[2]={0};
    int reader=video_main_reader_route(s,ybus,uvbus,width,height,sy,sc,format,reader_physical);
    if (reader<0) { return -1; }
    bool declared=reader>0;
    for (unsigned k=0;k<G_N_ELEMENTS(main_table_bases);k++) {
        uint32_t base=main_table_bases[k];
        if (!main_register(s,base+0x244)) { continue; }
        for (unsigned r=0;r<8;r++) {
            unsigned ro=main_row_offset(r);
            declared|=(uint32_t)main_pointer(s,base+0x280+ro)==ybus ||
                (uint32_t)main_pointer(s,base+0x680+ro)==uvbus;
        }
    }
    WlMainOutputState *m=s->video_main;unsigned matches=0;
    if (m) {
        for (unsigned k=0;k<WL_MAIN_SURFACES;k++) {
            const WlMainSurface *f=&m->surface[k];
            if (!f->valid) { continue; }
            if (reader>0) {
                if (f->physical[0]!=reader_physical[0] || f->physical[1]!=reader_physical[1]) { continue; }
            } else if ((uint32_t)f->alias[0]!=ybus || (uint32_t)f->alias[1]!=uvbus) { continue; }
            declared=true;
            /* Registered reader reconstructs its declared byte lanes from the
             * stored functional packet. Endian is a reader property, whereas
             * packing, chroma order, visible geometry and owned RAM must match.
             * Byte8 justification is irrelevant; packed10 justification is not. */
            unsigned typed_mask=((format>>3)&3) ? 0x3f : 0x1f;
            if (f->leased || (f->source_format&typed_mask)!=(format&typed_mask) ||
                f->width!=width || f->width!=visible_width || f->height!=visible_height ||
                height<f->height || height>f->storage_height ||
                sy!=f->stride || sc!=f->stride) { continue; }
            bool current=true;
            for (unsigned p=0;p<2;p++) {
                uint32_t base=f->table+p*0x400;
                unsigned ro=main_row_offset(f->table_row);
                current&=main_pointer(s,base+0x280+ro)==f->alias[p] &&
                    main_pointer(s,base+0x2a0+ro)==f->physical[p] &&
                    main_register(s,base+0x218)==f->bytes[p] &&
                    main_register(s,base+0x21c)==f->stride &&
                    main_register(s,base+0x200)==(format==0x332?9:0);
            }
            /* A live reader supplies its own physical mapping. Rebinding the
             * producer does not erase an earlier frame still in actual RAM. */
            if (!reader && !current) { continue; }
            physical[0]=f->physical[0];physical[1]=f->physical[1];*token=k+1;matches++;
        }
    }
    return matches==1 ? 1 : declared ? -1 : 0;
}

static void video_main_acquire(NikonZ9State *s,unsigned token)
{
    if (token) { s->video_main->surface[token-1].leased=true;s->video_main->acquisitions++; }
}

static void video_main_release(NikonZ9State *s,unsigned token)
{
    if (token && s->video_main && s->video_main->surface[token-1].leased) {
        s->video_main->surface[token-1].leased=false;
        s->video_main->surface[token-1].valid=false;s->video_main->releases++;
    }
}

static bool get_video_main_model(Object *obj,Error **errp)
{ return NIKON_Z9_LAB(obj)->video_main_model; }

static void set_video_main_model(Object *obj,bool value,Error **errp)
{
    NikonZ9State *s=NIKON_Z9_LAB(obj);
    if (s->image_ram.ram || s->image_ram.rom_device) {
        error_setg(errp,"video-main-output-model is immutable after realization");return;
    }
    s->video_main_model=value;
}

static char *get_video_main_state(Object *obj,Error **errp)
{
    NikonZ9State *s=NIKON_Z9_LAB(obj);WlMainOutputState empty={0};
    WlMainOutputState *m=s->video_main?s->video_main:&empty;
    GString *out=g_string_new(NULL);
    g_string_append_printf(out,"{\"enabled\":%s,\"frames\":%"PRIu64",\"bytes\":%"PRIu64
        ",\"rejects\":%"PRIu64",\"blocked\":%"PRIu64",\"cache_builds\":%"PRIu64
        ",\"acquisitions\":%"PRIu64",\"releases\":%"PRIu64
        ",\"transport_frames\":%"PRIu64",\"packet_builds\":%"PRIu64",\"valid_words\":[%u,%u],"
        "\"fresh_words\":[%u,%u],\"rejection\":\"%s\",\"surfaces\":[",
        s->video_main_model?"true":"false",m->frames,m->bytes,m->rejects,m->blocked,
        m->cache_builds,m->acquisitions,m->releases,m->transport_frames,m->packet_builds,
        s->video_main_valid[0],s->video_main_valid[1],
        s->video_main_written[0],s->video_main_written[1],m->rejection?m->rejection:"");
    for (unsigned k=0;k<WL_MAIN_SURFACES;k++) {
        WlMainSurface *f=&m->surface[k];
        g_string_append_printf(out,"%s{\"valid\":%s,\"leased\":%s,\"epoch\":%"PRIu64
            ",\"aliases\":[%"PRIu64",%"PRIu64"],\"physical\":[%"PRIu64",%"PRIu64
            "],\"width\":%u,\"height\":%u,\"storage_height\":%u,\"stride\":%u,\"source_format\":%u,"
            "\"transported\":%s,\"logical_bytes\":[%"PRIu64",%"PRIu64"],"
            "\"stored_bytes\":[%"PRIu64",%"PRIu64"],\"capacities\":[%"PRIu64",%"PRIu64"]}",
            k?",":"",f->valid?"true":"false",f->leased?"true":"false",f->epoch,
            f->alias[0],f->alias[1],f->physical[0],f->physical[1],f->width,f->height,
            f->storage_height,f->stride,f->source_format,f->transported?"true":"false",
            f->bytes[0],f->bytes[1],f->stored[0],f->stored[1],f->capacity[0],f->capacity[1]);
    }
    g_string_append(out,"],\"production_history\":[");
    uint64_t count=MIN(m->frames,G_N_ELEMENTS(m->history)),first=m->frames-count;
    for (uint64_t j=0;j<count;j++) {
        WlMainProduction *r=&m->history[(first+j)%G_N_ELEMENTS(m->history)];
        g_string_append_printf(out,"%s{\"sequence\":%"PRIu64",\"time_ns\":%"PRIu64
            ",\"epoch\":%"PRIu64",\"physical\":[%"PRIu64",%"PRIu64
            "],\"aliases\":[%"PRIu64",%"PRIu64"],\"bank\":%u,\"row\":%u,"
            "\"selected\":%u,\"table\":%u,\"table_row\":%u}",
            j?",":"",first+j+1,r->time_ns,r->epoch,r->physical[0],r->physical[1],
            r->alias[0],r->alias[1],r->bank,r->row,r->selected,r->table,r->table_row);
    }
    g_string_append_printf(out,"],\"storage_bits\":%u,\"component_precision_bits\":%u,"
        "\"source_format\":%u,\"original_algorithm\":false,\"compression_verified\":false,"
        "\"transport\":\"optional WLUV/zlib compact RAM; not Nikon YUVCMP\","
        "\"scope\":\"functional configured main NV12 byte8/packed10 RAM view; complete result query owns reuse\"}",
        m->cache_format==0x302?8:10,m->cache_format==0x332?10:8,m->cache_format);
    return g_string_free(out,false);
}
