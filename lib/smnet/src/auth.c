/* Autenticación y autorización (PROTOCOL.md §11, v1.4).
 *
 *   clave de partida : par Ed25519 que genera el host al crear la partida; las réplicas reciben
 *                      la secreta (cifrada por el enlace) para poder seguir emitiendo certificados
 *   certificado      : id u16 | clave del nodo 32 B | firma de la partida 64 B   (98 B)
 *                      lo entrega el coordinador en WELCOME, atado a la identidad del enlace
 *   PRESENT          : clave pública de la partida + certificado, al abrir cada sesión; quien lo
 *                      recibe comprueba la firma y que la clave coincida con la del handshake, y
 *                      etiqueta la sesión con ese id. Desde ahí, el remitente de una cabecera solo
 *                      vale si coincide con esa etiqueta.
 *   NEW_COORD        : firmado con la clave de partida (solo una réplica la tiene).
 *
 * Con encrypt = 0 (bancos de escala) las firmas no se calculan ni se verifican, pero los formatos,
 * las etiquetas y las reglas de autorización son las mismas. */
#include <string.h>
#include "node_internal.h"
#include "../third_party/monocypher/monocypher.h"

static void game_msg(const smnet_node_t* n, uint8_t* m, int* len, const char* label, const uint8_t* body, int blen) {
    int ll = (int)strlen(label);
    memcpy(m, label, (size_t)ll);
    memcpy(m + ll, n->game_pk, 32);
    memcpy(m + ll + 32, body, (size_t)blen);
    *len = ll + 32 + blen;
}

static void game_sign(const smnet_node_t* n, uint8_t sig[64], const char* label, const uint8_t* body, int blen) {
    if (!n->cfg.encrypt || !n->have_game_sk) { memset(sig, 0, 64); return; }
    uint8_t m[256]; int len;
    game_msg(n, m, &len, label, body, blen);
    crypto_eddsa_sign(sig, n->game_sk, m, (size_t)len);
}

static int game_check(const smnet_node_t* n, const uint8_t sig[64], const char* label, const uint8_t* body, int blen) {
    if (!n->cfg.encrypt) return 0;
    if (!n->have_game) return -1;
    uint8_t m[256]; int len;
    game_msg(n, m, &len, label, body, blen);
    return crypto_eddsa_check(sig, n->game_pk, m, (size_t)len);
}

void auth_init_game(smnet_node_t* n) {
    uint8_t seed[32];
    link_random(n->link, seed, 32);
    crypto_eddsa_key_pair(n->game_sk, n->game_pk, seed);
    n->have_game = n->have_game_sk = 1;
}

void auth_make_cert(const smnet_node_t* n, uint16_t id, const uint8_t pk[32], uint8_t out[CERT_SIZE]) {
    out[0] = (uint8_t)id; out[1] = (uint8_t)(id >> 8);
    memcpy(out + 2, pk, 32);
    game_sign(n, out + 34, "smnet-cert", out, 34);
}

int auth_check_cert(const smnet_node_t* n, const uint8_t cert[CERT_SIZE]) {
    return game_check(n, cert + 34, "smnet-cert", cert, 34);
}

int auth_build_present(const smnet_node_t* n, uint8_t* buf, int cap) {
    wbuf_t w = { buf, 0, cap, 0 };
    node_hdr((smnet_node_t*)n, &w, MSG_PRESENT, 0, 0);
    w_bytes(&w, n->game_pk, 32);
    w_bytes(&w, n->cert, CERT_SIZE);
    return w.err ? 0 : w.len;
}

/* PRESENT recibido: etiqueta la sesión con el id del certificado. */
int auth_on_present(smnet_node_t* n, const smnet_addr_t* from, rbuf_t* r) {
    uint8_t gpk[32], cert[CERT_SIZE];
    r_bytes(r, gpk, 32);
    r_bytes(r, cert, CERT_SIZE);
    if (r->err) return 0;
    if (!n->have_game) {
        /* primera vez (entrando): se confía en la clave que presenta el nodo por el que entramos.
           En el juego, el lobby puede fijarla de antemano (cfg.game_pk). */
        int via_entry = smnet_addr_eq(from, &n->bootstrap) || (n->have_coord && smnet_addr_eq(from, &n->coord_addr));
        if (!via_entry || n->peer.status != SMNET_PEER_JOINING) { n->stats.auth_rejected++; return 0; }
        memcpy(n->game_pk, gpk, 32);
        n->have_game = 1;
    }
    if (memcmp(gpk, n->game_pk, 32) != 0) { n->stats.auth_rejected++; return 0; }   /* otra partida */
    if (auth_check_cert(n, cert) != 0) { n->stats.auth_rejected++; return 0; }
    const uint8_t* pk = link_peer_identity(n->link, from);
    if (!pk || memcmp(pk, cert + 2, 32) != 0) { n->stats.auth_rejected++; return 0; }   /* certificado ajeno */
    uint16_t id = (uint16_t)(cert[0] | (cert[1] << 8));
    if (id >= SMNET_MAX_PLAYERS) return 0;
    link_set_tag(n->link, from, id);
    return 1;
}

int auth_build_new_coord(const smnet_node_t* n, uint32_t epoch, uint8_t* buf, int cap) {
    wbuf_t w = { buf, 0, cap, 0 };
    node_hdr((smnet_node_t*)n, &w, MSG_NEW_COORD, 0, 0);
    w_u32(&w, epoch);
    w_bytes(&w, link_identity(n->link), 32);
    uint8_t body[38];
    body[0] = (uint8_t)epoch; body[1] = (uint8_t)(epoch >> 8); body[2] = (uint8_t)(epoch >> 16); body[3] = (uint8_t)(epoch >> 24);
    body[4] = (uint8_t)n->id; body[5] = (uint8_t)(n->id >> 8);
    memcpy(body + 6, link_identity(n->link), 32);
    uint8_t sig[64];
    game_sign(n, sig, "smnet-newcoord", body, 38);
    w_bytes(&w, sig, 64);
    return w.err ? 0 : w.len;
}

/* NEW_COORD: firmado por la partida, por el id del remitente y con la identidad de su sesión. */
int auth_check_new_coord(const smnet_node_t* n, const smnet_addr_t* from, const smnet_hdr_t* h, const rbuf_t* r0) {
    rbuf_t r = *r0;
    uint32_t epoch = r_u32(&r);
    uint8_t cpk[32], sig[64];
    r_bytes(&r, cpk, 32);
    r_bytes(&r, sig, 64);
    if (r.err) return -1;
    const uint8_t* pk = link_peer_identity(n->link, from);
    if (!pk || memcmp(pk, cpk, 32) != 0) return -1;
    uint8_t body[38];
    body[0] = (uint8_t)epoch; body[1] = (uint8_t)(epoch >> 8); body[2] = (uint8_t)(epoch >> 16); body[3] = (uint8_t)(epoch >> 24);
    body[4] = (uint8_t)h->sender; body[5] = (uint8_t)(h->sender >> 8);
    memcpy(body + 6, cpk, 32);
    return game_check(n, sig, "smnet-newcoord", body, 38);
}

static int is_sn_id(const smnet_node_t* n, uint16_t id) { return node_is_sn_or_recent(n, id); }

/* ¿Se acepta este mensaje de esta sesión? (no se llama para la entrega local) */
int auth_allowed(smnet_node_t* n, const smnet_addr_t* from, const smnet_hdr_t* h) {
    switch (h->type) {
    case MSG_HELLO: case MSG_PRESENT: case MSG_REDIRECT:
        return 1;   /* antes de tener certificado */
    default: break;
    }
    uint16_t bound = link_tag(n->link, from);
    if (bound == SMNET_NO_ID || bound != h->sender) return 0;
    int active = n->co && n->co->role == SMNET_COORD_ACTIVE;
    switch (h->type) {
    case MSG_WELCOME: case MSG_ASSIGN: case MSG_PROMOTE: case MSG_DEMOTE: case MSG_SN_LIST:
        /* órdenes de coordinador: solo del coordinador vigente (al entrar, del que nos dio la clave) */
        return !n->have_coord || h->sender == n->coord_id;
    case MSG_COORD_HB: case MSG_COORD_SYNC:
        /* a una réplica, solo del coordinador vigente; al coordinador activo le llegan de uno viejo
           (y les contesta con la época vigente) */
        return active || !n->have_coord || h->sender == n->coord_id;
    case MSG_INTRO:
        return h->sender == n->coord_id || is_sn_id(n, h->sender);
    case MSG_STATE_RELAY: case MSG_SUBSCRIBE: case MSG_RELAY_FULL: case MSG_RELAY_PROOF:
        return is_sn_id(n, h->sender);
    default:
        return 1;
    }
}
