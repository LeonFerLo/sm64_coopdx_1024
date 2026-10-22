-- name: smnet: ejemplo de ids estables
-- description: Prueba la API de smnet del modo masivo.\n\n/jugadores lista a todos los de la partida (no solo los 15 cercanos).\n/saludar manda un mensaje de mod a todos.\n/tabla TEXTO lo pone en gPlayerSyncTable propia (los cercanos lo ven).\n/paquete TEXTO lo manda con network_send.\nAvisa en el chat cuando alguien entra o se va.

if smnet_active == nil then
    djui_chat_message_create("smnet: este juego no tiene la API (compilalo con SMNET=1)")
    return
end

local entradas = 0

smnet_on_player(function(id, entro)
    if entro then
        entradas = entradas + 1
        -- solo los primeros, para no llenar el chat cuando entran 100 juntos
        if entradas <= 5 then
            djui_chat_message_create("\\#a0ffa0\\entró " .. (smnet_player_name(id) or "?") .. " (id " .. id .. ")")
        end
    else
        djui_chat_message_create("\\#ffa0a0\\se fue " .. (smnet_player_name(id) or "?") .. " (id " .. id .. ")")
    end
end)

smnet_on_message(function(desde, texto)
    djui_chat_message_create("\\#a0c0ff\\[mod] " .. (smnet_player_name(desde) or "?") .. ": " .. texto)
end)

hook_chat_command("jugadores", "- lista a todos los jugadores de la partida (smnet)", function(msg)
    if not smnet_active() then
        djui_chat_message_create("no estás en modo masivo")
        return true
    end
    local ids = smnet_players()
    djui_chat_message_create("jugadores en la partida: " .. smnet_player_count() .. " (yo soy el id " .. tostring(smnet_local_id()) .. ")")
    local cerca, lejos = 0, 0
    for i = 1, #ids do
        local id = ids[i]
        local slot = smnet_player_slot(id)
        if slot ~= nil then cerca = cerca + 1 else lejos = lejos + 1 end
        if i <= 10 then
            local donde = slot ~= nil and ("slot " .. slot) or (smnet_player_pos(id) ~= nil and "impostor" or "en otra área")
            djui_chat_message_create("  " .. id .. ": " .. (smnet_player_name(id) or "?") .. " - " .. donde)
        end
    end
    if #ids > 10 then djui_chat_message_create("  ... y " .. (#ids - 10) .. " más") end
    djui_chat_message_create("con slot: " .. cerca .. ", sin slot: " .. lejos)
    return true
end)

hook_chat_command("saludar", "- manda un mensaje de mod a todos (smnet_send)", function(msg)
    local ok = smnet_send(-1, "hola desde " .. (smnet_player_name(smnet_local_id() or -1) or "alguien"))
    djui_chat_message_create(ok and "saludo enviado" or "no se pudo enviar")
    return true
end)

-- red de Lua de sm64coopdx (en modo masivo va por smnet): gPlayerSyncTable y network_send
for i = 1, MAX_PLAYERS - 1 do
    hook_on_sync_table_change(gPlayerSyncTable[i], "texto", i, function(tag, campo, antes, ahora)
        if ahora ~= nil then
            djui_chat_message_create("\\#ffe080\\[tabla] " .. gNetworkPlayers[tag].name .. " (slot " .. tag .. "): " .. tostring(ahora))
        end
    end)
end

hook_chat_command("tabla", "TEXTO - lo pone en gPlayerSyncTable[0].texto", function(msg)
    gPlayerSyncTable[0].texto = msg
    djui_chat_message_create("puesto en la tabla propia: " .. msg)
    return true
end)

hook_chat_command("paquete", "TEXTO - lo manda a todos con network_send", function(msg)
    network_send(true, { texto = msg })
    djui_chat_message_create("paquete enviado")
    return true
end)

hook_event(HOOK_ON_PACKET_RECEIVE, function(data)
    if data.texto ~= nil then
        djui_chat_message_create("\\#c0ffc0\\[paquete] " .. tostring(data.texto))
    end
end)
