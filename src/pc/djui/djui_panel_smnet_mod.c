/* Panel de moderación de las partidas masivas (pausa → Moderación): buscar jugadores entre los de
   toda la partida y echarlos, banearlos o silenciarlos; el host además cierra o abre la entrada.
   La lógica está en pc/network/smnet/ns_smnet_mod.c. */
#ifdef SMNET
#include <stdio.h>
#include <string.h>
#include "djui.h"
#include "djui_panel.h"
#include "djui_panel_menu.h"
#include "djui_panel_confirm.h"
#include "djui_panel_smnet_mod.h"
#include "pc/network/network.h"
#include "pc/network/smnet/ns_smnet.h"
#include "pc/network/smnet/ns_smnet_mod.h"

#define RESULTS_MAX 64

static struct DjuiInputbox* sSearch = NULL;
static struct DjuiFlowLayout* sLayout = NULL;
static struct DjuiPaginated* sPaginated = NULL;
static unsigned int sClosedChoice = 0;
static uint16_t sTarget = 0xFFFF;
static char sTitle[64];

static void mod_list_fill(struct DjuiBase* base);

static void mod_rebuild(UNUSED struct DjuiBase* caller) {
    if (!sLayout) return;
    djui_base_destroy_children(&sLayout->base);
    mod_list_fill(&sLayout->base);
    if (sPaginated) djui_paginated_calculate_height(sPaginated);
}

/* ---- acciones sobre un jugador */
static void mod_do(struct DjuiBase* caller, int op) {
    if (sTarget == 0xFFFF) return;
    if (!ns_smnet_mod_action(sTarget, op)) {
        djui_popup_create("\\#ffa0a0\\No se puede hacer eso con este jugador.", 1);
        return;
    }
    djui_panel_menu_back(caller);
}
static void mod_kick(struct DjuiBase* c)    { mod_do(c, NS_MOD_KICK); }
static void mod_ban(struct DjuiBase* c)     { mod_do(c, NS_MOD_BAN); }
static void mod_permban(struct DjuiBase* c) { mod_do(c, NS_MOD_PERMBAN); }
static void mod_mute(struct DjuiBase* c)    { mod_do(c, ns_smnet_mod_muted(sTarget) ? NS_MOD_UNMUTE : NS_MOD_MUTE); }
static void mod_moder(struct DjuiBase* c)   { mod_do(c, ns_smnet_mod_is_moderator(sTarget) ? NS_MOD_UNMODERATOR : NS_MOD_MODERATOR); }

static void mod_player_create(struct DjuiBase* caller) {
    sTarget = (uint16_t)caller->tag;
    char name[40]; ns_smnet_mod_name(sTarget, name, sizeof name);
    snprintf(sTitle, sizeof sTitle, "%s", name);
    struct DjuiThreePanel* panel = djui_panel_menu_create(sTitle, false);
    struct DjuiBase* body = djui_three_panel_get_body(panel);
    {
        bool host = ns_smnet_mod_is_boss();
        char info[96];
        snprintf(info, sizeof info, "Jugador #%u%s%s", (unsigned)sTarget,
                 ns_smnet_mod_muted(sTarget) ? " (silenciado)" : "", ns_smnet_mod_is_moderator(sTarget) ? " (moderador)" : "");
        struct DjuiText* t = djui_text_create(body, info);
        djui_base_set_size_type(&t->base, DJUI_SVT_RELATIVE, DJUI_SVT_ABSOLUTE);
        djui_base_set_size(&t->base, 1.0f, 32);
        djui_text_set_alignment(t, DJUI_HALIGN_CENTER, DJUI_VALIGN_CENTER);

        djui_button_create(body, "Echar (5 minutos)", DJUI_BUTTON_STYLE_NORMAL, mod_kick);
        djui_button_create(body, "Banear de esta partida", DJUI_BUTTON_STYLE_NORMAL, mod_ban);
        if (host) djui_button_create(body, "Banear para siempre", DJUI_BUTTON_STYLE_NORMAL, mod_permban);
        djui_button_create(body, ns_smnet_mod_muted(sTarget) ? "Quitar el silencio" : "Silenciar el chat", DJUI_BUTTON_STYLE_NORMAL, mod_mute);
        if (host) djui_button_create(body, ns_smnet_mod_is_moderator(sTarget) ? "Quitar de moderador" : "Hacer moderador", DJUI_BUTTON_STYLE_NORMAL, mod_moder);
        djui_button_create(body, DLANG(MENU, BACK), DJUI_BUTTON_STYLE_BACK, djui_panel_menu_back);
    }
    djui_panel_add(caller, panel, NULL);
}

/* ---- lista */
static void mod_list_fill(struct DjuiBase* base) {
    uint16_t ids[RESULTS_MAX];
    int n = ns_smnet_mod_search(sSearch && sSearch->buffer ? sSearch->buffer : "", ids, RESULTS_MAX);
    if (n == 0) {
        struct DjuiText* t = djui_text_create(base, "\\#a0a0a0\\(nadie con ese nombre)");
        djui_base_set_size_type(&t->base, DJUI_SVT_RELATIVE, DJUI_SVT_ABSOLUTE);
        djui_base_set_size(&t->base, 1.0f, 32);
        djui_text_set_alignment(t, DJUI_HALIGN_CENTER, DJUI_VALIGN_CENTER);
        return;
    }
    for (int i = 0; i < n; i++) {
        char name[40], label[80];
        ns_smnet_mod_name(ids[i], name, sizeof name);
        snprintf(label, sizeof label, "#%u %s%s%s", (unsigned)ids[i], name,
                 ns_smnet_mod_muted(ids[i]) ? " (silenciado)" : "", ns_smnet_mod_is_moderator(ids[i]) ? " (moderador)" : "");
        struct DjuiButton* b = djui_button_create(base, label, DJUI_BUTTON_STYLE_NORMAL, mod_player_create);
        b->base.tag = ids[i];
    }
}

static void mod_closed_change(UNUSED struct DjuiBase* caller) { ns_smnet_mod_set_closed(sClosedChoice == 1); }
static void mod_unban_yes(struct DjuiBase* caller) { ns_smnet_mod_unban_all(); djui_panel_menu_back(caller); }
static void mod_unban(struct DjuiBase* caller) {
    djui_panel_confirm_create(caller, "Sacar los baneos",
        "Se sacan todos los baneos (también los guardados para siempre) y los echados pueden volver.", mod_unban_yes);
}

void djui_panel_smnet_mod_create(struct DjuiBase* caller) {
    struct DjuiThreePanel* panel = djui_panel_menu_create("MODERACIÓN", false);
    struct DjuiBase* body = djui_three_panel_get_body(panel);
    {
        if (ns_smnet_mod_is_boss()) {
            sClosedChoice = ns_smnet_mod_closed() ? 1 : 0;
            char* choices[2] = { "Abierta", "Cerrada" };
            djui_selectionbox_create(body, "Entrada a la partida", choices, 2, &sClosedChoice, mod_closed_change);
        }
        struct DjuiSearchbox* sb = djui_searchbox_create(body, mod_rebuild);
        sSearch = sb->inputbox;
        struct DjuiPaginated* pg = djui_paginated_create(body, 7);
        sLayout = pg->layout;
        sPaginated = pg;
        mod_list_fill(&pg->layout->base);
        djui_paginated_calculate_height(pg);
        if (ns_smnet_mod_is_boss()) djui_button_create(body, "Sacar todos los baneos", DJUI_BUTTON_STYLE_NORMAL, mod_unban);
        djui_button_create(body, DLANG(MENU, BACK), DJUI_BUTTON_STYLE_BACK, djui_panel_menu_back);
    }
    djui_panel_add(caller, panel, NULL);
}
#endif
