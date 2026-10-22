/* Ajustes → Partida masiva: cómo se ve la multitud, la conexión propia y la telemetría
   (configfile.c, configSmnet*; ver ns_smnet.c). */
#ifdef SMNET
#include "djui.h"
#include "djui_panel.h"
#include "djui_panel_menu.h"
#include "djui_panel_smnet_options.h"
#include "pc/configfile.h"
#include "pc/network/smnet/ns_smnet.h"

static void smnet_opt_changed(UNUSED struct DjuiBase* caller) { ns_smnet_options_apply(); }

void djui_panel_smnet_options_create(struct DjuiBase* caller) {
    struct DjuiThreePanel* panel = djui_panel_menu_create("PARTIDA MASIVA", false);
    struct DjuiBase* body = djui_three_panel_get_body(panel);
    {
        /* cuántos jugadores sin slot se dibujan y con cuánto detalle (se aplica en el momento) */
        char* crowd[] = { "Muy baja", "Baja", "Media", "Alta", "Muy alta" };
        if (configSmnetCrowd > 4) { configSmnetCrowd = 2; }
        djui_selectionbox_create(body, "Multitud", crowd, ARRAY_COUNT(crowd), &configSmnetCrowd, NULL);

        char* far[] = { "Pocas caras", "Gorra", "Completos" };
        if (configSmnetFarMode > 2) { configSmnetFarMode = 0; }
        djui_selectionbox_create(body, "Jugadores lejanos", far, ARRAY_COUNT(far), &configSmnetFarMode, smnet_opt_changed);
        djui_checkbox_create(body, "Animar a los lejanos", &configSmnetFarAnimated, smnet_opt_changed);
        djui_checkbox_create(body, "Medianos con el modelo completo", &configSmnetLowFull, smnet_opt_changed);

        /* lenta: no la eligen para repartir el tráfico de otros (vale desde la próxima partida) */
        char* up[] = { "Normal", "Lenta" };
        if (configSmnetUpload > 1) { configSmnetUpload = 0; }
        djui_selectionbox_create(body, "Mi conexión", up, ARRAY_COUNT(up), &configSmnetUpload, NULL);

        djui_checkbox_create(body, "Estadísticas anónimas", &configSmnetTelemetry, NULL);

        struct DjuiText* t = djui_text_create(body,
            "\\#a0a0a0\\Con muchos jugadores, menos multitud o lejanos con \"Gorra\" dan más FPS. "
            "\"Lenta\" evita que tu PC reparta el tráfico de otros. Las estadísticas (sin nombres) ayudan a mejorar el juego.");
        djui_base_set_size_type(&t->base, DJUI_SVT_RELATIVE, DJUI_SVT_ABSOLUTE);
        djui_base_set_size(&t->base, 1.0f, 96);
        djui_text_set_alignment(t, DJUI_HALIGN_CENTER, DJUI_VALIGN_TOP);

        djui_button_create(body, DLANG(MENU, BACK), DJUI_BUTTON_STYLE_BACK, djui_panel_menu_back);
    }
    djui_panel_add(caller, panel, NULL);
}
#endif
