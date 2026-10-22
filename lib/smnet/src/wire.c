#include <math.h>
#include <string.h>
#include "wire.h"

int smnet_addr_eq(const smnet_addr_t* a, const smnet_addr_t* b) {
    return a->len == b->len && memcmp(a->data, b->data, a->len) == 0;
}

void w_u8(wbuf_t* w, uint8_t v) {
    if (w->len + 1 > w->cap) { w->err = 1; return; }
    w->p[w->len++] = v;
}
void w_u16(wbuf_t* w, uint16_t v) { w_u8(w, (uint8_t)v); w_u8(w, (uint8_t)(v >> 8)); }
void w_u32(wbuf_t* w, uint32_t v) { w_u16(w, (uint16_t)v); w_u16(w, (uint16_t)(v >> 16)); }
void w_bytes(wbuf_t* w, const void* src, int n) {
    if (n < 0 || w->len + n > w->cap) { w->err = 1; return; }
    memcpy(w->p + w->len, src, (size_t)n);
    w->len += n;
}

uint8_t r_u8(rbuf_t* r) {
    if (r->pos + 1 > r->len) { r->err = 1; return 0; }
    return r->p[r->pos++];
}
uint16_t r_u16(rbuf_t* r) { uint16_t lo = r_u8(r); return (uint16_t)(lo | ((uint16_t)r_u8(r) << 8)); }
uint32_t r_u32(rbuf_t* r) { uint32_t lo = r_u16(r); return lo | ((uint32_t)r_u16(r) << 16); }
void r_bytes(rbuf_t* r, void* dst, int n) {
    if (n < 0 || r->pos + n > r->len) { r->err = 1; return; }
    memcpy(dst, r->p + r->pos, (size_t)n);
    r->pos += n;
}

void hdr_write(wbuf_t* w, const smnet_hdr_t* h) {
    w_u8(w, (uint8_t)((h->version << 4) | (h->flags & 0x0F)));
    w_u8(w, h->type);
    w_u16(w, h->sender);
    w_u16(w, h->seq);
    w_u16(w, h->tick);
}

int hdr_read(rbuf_t* r, smnet_hdr_t* h) {
    uint8_t vf = r_u8(r);
    h->version = vf >> 4;
    h->flags = vf & 0x0F;
    h->type = r_u8(r);
    h->sender = r_u16(r);
    h->seq = r_u16(r);
    h->tick = r_u16(r);
    if (r->err) return -1;
    if (h->version != SMNET_PROTO_VERSION) return -2;
    return 0;
}

void addr_write(wbuf_t* w, const smnet_addr_t* a) {
    uint8_t l = a->len > sizeof a->data ? (uint8_t)sizeof a->data : a->len;
    w_u8(w, l);
    w_bytes(w, a->data, l);
}

void addr_read(rbuf_t* r, smnet_addr_t* a) {
    memset(a, 0, sizeof *a);
    uint8_t l = r_u8(r);
    if (l > sizeof a->data) { r->err = 1; return; }
    a->len = l;
    r_bytes(r, a->data, l);
}

/* ---- cuantización (PROTOCOL.md §5.2) ---- */

static int16_t q_pos(float v) {
    float q = roundf(v * 4.0f);
    if (q > 32767.f) q = 32767.f;
    if (q < -32768.f) q = -32768.f;
    return (int16_t)q;
}
static int8_t q_i8(float v, float scale) {
    float q = roundf(v * scale);
    if (q > 127.f) q = 127.f;
    if (q < -128.f) q = -128.f;
    return (int8_t)q;
}

void state_write(wbuf_t* w, const smnet_state_t* s) {
    w_u16(w, (uint16_t)q_pos(s->x));
    w_u16(w, (uint16_t)q_pos(s->y));
    w_u16(w, (uint16_t)q_pos(s->z));
    uint16_t yaw12 = (uint16_t)((uint16_t)s->yaw >> 4);
    w_u16(w, (uint16_t)(yaw12 | ((uint16_t)(s->flags & 0x0F) << 12)));
    w_u8(w, (uint8_t)q_i8(s->fwd_vel, 2.0f));
    w_u8(w, (uint8_t)q_i8(s->vel_y, 1.0f));
    w_u8(w, s->action);
    w_u8(w, s->anim_frame);
}

void state_read(rbuf_t* r, smnet_state_t* s) {
    s->x = (int16_t)r_u16(r) / 4.0f;
    s->y = (int16_t)r_u16(r) / 4.0f;
    s->z = (int16_t)r_u16(r) / 4.0f;
    uint16_t yf = r_u16(r);
    s->yaw = (int16_t)((yf & 0x0FFF) << 4);
    s->flags = (uint8_t)(yf >> 12);
    s->fwd_vel = (int8_t)r_u8(r) / 2.0f;
    s->vel_y = (int8_t)r_u8(r);
    s->action = r_u8(r);
    s->anim_frame = r_u8(r);
}

void state_quantize(const smnet_state_t* in, smnet_state_t* out) {
    uint8_t buf[SMNET_STATE_SIZE];
    wbuf_t w = { buf, 0, sizeof buf, 0 };
    state_write(&w, in);
    rbuf_t r = { buf, w.len, 0, 0 };
    state_read(&r, out);
}
