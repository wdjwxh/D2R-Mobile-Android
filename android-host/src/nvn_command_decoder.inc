/* Keep sequential replay cheap. The full IR planner measured zero hoists and
 * zero merged barriers in camp, so it is off until a useful plan is proven.
 * This decoder only batches CPU-ready reads; it never changes command order. */
#define RE_ENABLE_HOIST 0
// The expanded real-game catalog still produced no accepted camp fusion.
// Keep the implementation available without paying failed capture/proof work
// on the ordinary replay path. Re-enable only for a supported shader pairing.
#ifndef RE_ENABLE_FUSE
#define RE_ENABLE_FUSE 0
#endif
#define RE_WINDOW_BYTES 8192
typedef struct {
    uint8_t bytes[RE_WINDOW_BYTES];uint64_t address;
    size_t size,request;int disabled;
} replay_decoder;
static void replay_decoder_discard(replay_decoder *d) {
    d->address=0;d->size=0;d->request=2048;d->disabled=0;
}
static int replay_decoder_snapshot(replay_decoder *d,uint64_t address,uint8_t record[2048],op_hdr *h) {
    if(address&7)return 0;
    if(!d->disabled && !(address>=d->address && address-d->address<=d->size &&
       d->size-(size_t)(address-d->address)>=sizeof *h)) {
        int sequential=d->size && address==d->address+d->size;
        if(sequential && d->request<RE_WINDOW_BYTES)d->request*=2;
        else if(!sequential)d->request=2048;
        d->size=0;
        if(address<=UINT64_MAX-d->request && !dm_vk_guest_range_peek(address,d->bytes,d->request)) {
            d->address=address;d->size=d->request;
        } else d->disabled=1;
    }
    if(d->size && address>=d->address && address-d->address<=d->size &&
       d->size-(size_t)(address-d->address)>=sizeof *h) {
        size_t offset=(size_t)(address-d->address);memcpy(h,d->bytes+offset,sizeof *h);
        if(h->size<8 || (h->size&7) || h->size>2048 || address>UINT64_MAX-h->size)return 0;
        if(h->size<=d->size-offset){memcpy(record,d->bytes+offset,h->size);return 1;}
    }
    // Let the caller obtain one complete record using its regular ownership
    // path. Never join a cached header to a separately materialized payload.
    return 0;
}
static int replay_decoder_preserve(unsigned op) {
    switch(op) {
    case OP_BEGIN:case OP_JUMP:
    case OP_VIEWPORTS:case OP_SCISSOR:case OP_DEPTH_RANGE:
    case OP_BIND_PROGRAM:case OP_BIND_STATE:case OP_BIND_VATTR:case OP_BIND_VSTREAM:case OP_BIND_VBUF:
    case OP_BIND_UBO:case OP_BIND_SSBO:case OP_BIND_TEX:case OP_BIND_SAMP:case OP_BIND_IMG:
    case OP_STENCIL:case OP_POLY_OFFSET:case OP_BLEND_COLOR:case OP_COPY_STRIDE:
    case OP_SET_TEX_POOL:case OP_SET_SAMP_POOL:return 1;
    default:return 0; // Includes SET_RT's clear flush, draws, clears and CALL.
    }
}
