#!/bin/sh
# Instala el servidor coopnet de sm64coopdx-1024 en un VPS con Ubuntu o Debian.
# Desde la carpeta coopnet/ (con el código):   sudo sh despliegue/instalar.sh
# Volver a correrlo actualiza el binario y conserva la configuración y la telemetría.
set -e
[ "$(id -u)" = 0 ] || { echo "correlo con sudo"; exit 1; }
cd "$(dirname "$0")/.."
[ -f server/smain.cpp ] || { echo "correlo desde la carpeta coopnet (falta server/smain.cpp)"; exit 1; }

echo "== paquetes"
apt-get update -qq
apt-get install -y -qq build-essential libcurl4-openssl-dev > /dev/null

echo "== compilando"
rm -rf bin
make server -j"$(nproc)" > /dev/null

echo "== instalando"
id coopnet > /dev/null 2>&1 || useradd --system --home /var/lib/coopnet --shell /usr/sbin/nologin coopnet
install -d -o coopnet -g coopnet /var/lib/coopnet
install -d /opt/coopnet
systemctl stop coopnet-1024 2> /dev/null || true
install -m 755 bin/server /opt/coopnet/server
[ -f /etc/coopnet-1024.env ] || install -m 644 despliegue/coopnet-1024.env /etc/coopnet-1024.env
install -m 644 despliegue/coopnet.service /etc/systemd/system/coopnet-1024.service
systemctl daemon-reload
systemctl enable --now coopnet-1024

. /etc/coopnet-1024.env
P=${COOPNET_PUERTO:-34197}; U=${COOPNET_ENCUENTRO:-34198}; U2=$((U + 1))
if command -v ufw > /dev/null 2>&1 && ufw status | grep -q "Status: active"; then
    echo "== firewall (ufw)"
    ufw allow "$P"/tcp > /dev/null
    ufw allow "$U:$U2"/udp > /dev/null
fi

sleep 2
echo
systemctl --no-pager --lines=5 status coopnet-1024 || true
echo
echo "Listo. Puertos: TCP $P y UDP $U-$U2 (si el proveedor tiene firewall propio, abrilos ahí también)."
echo "  log en vivo:      journalctl -u coopnet-1024 -f"
echo "  telemetría:       cat ${COOPNET_TELEMETRIA:-/var/lib/coopnet/telemetria.json}"
echo "  configuración:    /etc/coopnet-1024.env  (después: systemctl restart coopnet-1024)"
