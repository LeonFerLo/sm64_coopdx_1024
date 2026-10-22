#!/bin/sh
# Host dedicado de la partida masiva en el VPS: el juego sin pantalla (--headless) que abre la
# partida, la lista en el coopnet de esta misma máquina y queda siempre prendido (servicio
# sm64-1024-host). Es un jugador más, invisible, y el coordinador de la partida.
#
# Desde la carpeta del proyecto (la que tiene coopnet/ y juego/sm64coopdx/), después de instalar.sh:
#   sudo sh servidor/coopnet/despliegue/host-dedicado.sh /ruta/a/baserom.us.z64   (repositorio público)
#   sudo sh coopnet/despliegue/host-dedicado.sh /ruta/a/baserom.us.z64            (carpeta de desarrollo)
# Volver a correrlo recompila y reinicia, sin tocar la configuración (/etc/sm64-1024-host.env).
#
# La ROM es la tuya: queda en /var/lib/sm64-1024 (solo la lee el juego, no se publica en ningún lado).
set -e
[ "$(id -u)" = 0 ] || { echo "correlo con sudo"; exit 1; }
ROM="$1"
D=$(cd "$(dirname "$0")/.." && pwd)          # la carpeta de coopnet
# dos formas: el repositorio público (el juego en la raíz, coopnet en servidor/coopnet) o la de desarrollo
if [ -f "$D/../../src/pc/pc_main.c" ]; then G=$(cd "$D/../.." && pwd); else G=$(cd "$D/../juego/sm64coopdx" 2>/dev/null && pwd || true); fi
[ -n "$G" ] && [ -f "$G/Makefile" ] || { echo "no encuentro el código del juego"; exit 1; }
P=$G
[ -f /etc/coopnet-1024.env ] || { echo "primero instalá el servidor: sudo sh $D/despliegue/instalar.sh"; exit 1; }
DATA=/var/lib/sm64-1024
if [ ! -f "$DATA/baserom.us.z64" ]; then
    [ -n "$ROM" ] && [ -f "$ROM" ] || { echo "uso: sudo sh $0 /ruta/a/baserom.us.z64"; exit 1; }
fi

echo "== paquetes para compilar el juego"
apt-get update -qq
apt-get install -y -qq build-essential git python3 libglew-dev libsdl2-dev zlib1g-dev libcurl4-openssl-dev > /dev/null
apt-get install -y -qq bsdextrautils > /dev/null 2>&1 || apt-get install -y -qq bsdmainutils > /dev/null   # hexdump

echo "== libcoopnet (la nuestra) y el juego (tarda unos minutos)"
cd "$D" && make lib -j"$(nproc)" > /dev/null
cp bin/libcoopnet.a "$G/lib/coopnet/linux/libcoopnet.a"
cp common/libcoopnet.h "$G/lib/coopnet/include/libcoopnet.h"
cd "$G"
. /etc/coopnet-1024.env
CP=${COOPNET_PUERTO:-34197}
make -j"$(nproc)" SMNET=1 DISCORD_SDK=0 COOPNET_SERVIDOR=127.0.0.1:$CP > "$P/compilar-host.log" 2>&1 || { tail -20 "$P/compilar-host.log"; echo "falló la compilación (log: $P/compilar-host.log)"; exit 1; }

echo "== instalando"
id sm64 > /dev/null 2>&1 || useradd --system --home "$DATA" --shell /usr/sbin/nologin sm64
install -d -o sm64 -g sm64 "$DATA"
systemctl stop sm64-1024-host 2> /dev/null || true
rm -rf /opt/sm64-1024 && mkdir -p /opt/sm64-1024
cp -r build/us_pc/. /opt/sm64-1024/
[ -n "$ROM" ] && [ -f "$ROM" ] && install -o sm64 -g sm64 -m 600 "$ROM" "$DATA/baserom.us.z64"

if [ ! -f /etc/sm64-1024-host.env ]; then
    cat > /etc/sm64-1024-host.env << 'EOT'
# Host dedicado de sm64coopdx-1024 (después de cambiarlo: sudo systemctl restart sm64-1024-host)
JUGADORES=1024
NOMBRE=Servidor 1024
# puerto UDP de la partida (abrirlo en el firewall del proveedor)
PUERTO=7777
# contraseña de la partida (vacía = pública)
CLAVE=
# mods activos, separados por espacios (los nombres de la carpeta mods/), ej.: MODS="character-select-coop"
MODS=
# administradores: identidades (64 caracteres, separadas por espacios) que tienen todos los permisos
# del host: echar, banear para siempre, nombrar moderadores, cerrar/abrir. Cada uno la ve en el juego
# con /miclave (o en smnet-identidad.txt, al lado de sm64config.txt)
ADMINS=
EOT
fi
grep -q "^ADMINS=" /etc/sm64-1024-host.env || printf '%s\n' "# administradores (ver /miclave en el juego)" "ADMINS=" >> /etc/sm64-1024-host.env

cat > /opt/sm64-1024/arrancar.sh << 'EOT'
#!/bin/sh
# lo usa el servicio sm64-1024-host
. /etc/sm64-1024-host.env
export COOPDX_SMNET_ADMINS="$ADMINS"
set -- --headless --skip-update-check --skip-intro --savepath /var/lib/sm64-1024 \
       --server "${PUERTO:-7777}" --coopnet "$CLAVE" --playercount "${JUGADORES:-1024}" --playername "${NOMBRE:-Servidor}"
for m in $MODS; do set -- "$@" --enable-mod "$m"; done
exec /opt/sm64-1024/sm64coopdx "$@"
EOT
chmod 755 /opt/sm64-1024/arrancar.sh

cat > /etc/systemd/system/sm64-1024-host.service << 'EOT'
[Unit]
Description=Host dedicado de sm64coopdx-1024
After=network-online.target coopnet-1024.service
Wants=network-online.target coopnet-1024.service

[Service]
User=sm64
Group=sm64
WorkingDirectory=/var/lib/sm64-1024
Environment=SDL_AUDIODRIVER=dummy
Environment=SDL_VIDEODRIVER=dummy
Environment=COOPDX_SMNET_LOG=/var/lib/sm64-1024/smnet-host.log
ExecStart=/opt/sm64-1024/arrancar.sh
Restart=always
RestartSec=5
NoNewPrivileges=true
ProtectHome=true
PrivateTmp=true

[Install]
WantedBy=multi-user.target
EOT

# el coopnet de esta máquina ve al host con una dirección local: que a los que entran les dé la pública
if ! grep -q "^COOPNET_IP_PUBLICA=" /etc/coopnet-1024.env; then
    IPP=$(curl -4 -s --max-time 5 https://api.ipify.org || true)
    if [ -n "$IPP" ]; then
        echo "COOPNET_IP_PUBLICA=$IPP" >> /etc/coopnet-1024.env
        systemctl restart coopnet-1024
        echo "== IP pública: $IPP (en /etc/coopnet-1024.env)"
    else
        echo "OJO: no pude averiguar la IP pública; agregá COOPNET_IP_PUBLICA=x.x.x.x a /etc/coopnet-1024.env y reiniciá coopnet-1024"
    fi
fi

. /etc/sm64-1024-host.env
if command -v ufw > /dev/null 2>&1 && ufw status | grep -q "Status: active"; then
    ufw allow "${PUERTO:-7777}"/udp > /dev/null
fi
systemctl daemon-reload
systemctl enable --now sm64-1024-host
sleep 8
echo
systemctl --no-pager --lines=8 status sm64-1024-host || true
echo
echo "Listo. La partida \"${NOMBRE}\" (${JUGADORES} jugadores) tendría que aparecer en Join → CoopNet."
echo "  abrir en el firewall del proveedor: UDP ${PUERTO:-7777} (además de TCP $CP y UDP ${COOPNET_ENCUENTRO:-34198}-$(( ${COOPNET_ENCUENTRO:-34198} + 1 )))"
echo "  log:            journalctl -u sm64-1024-host -f   (y /var/lib/sm64-1024/smnet-host.log)"
echo "  configuración:  /etc/sm64-1024-host.env  (después: systemctl restart sm64-1024-host)"
echo "  moderar:        en el juego escribí /miclave, poné esa identidad en ADMINS= de /etc/sm64-1024-host.env"
echo "                  y reiniciá el host: al entrar tenés los permisos del host (pausa → Moderación, /help)"
